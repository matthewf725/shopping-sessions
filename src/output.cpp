#include "output.hpp"
#include <cerrno>
#include <memory>
#include <span>
#include <system_error>

namespace shop {
namespace {
class Bytes {
    std::unique_ptr<uint8_t[]> owned_;
    uint8_t* memory_ = nullptr;
    size_t size_ = 0, capacity_ = 0;
    [[gnu::noinline]] void grow(size_t need) {
        size_t cap = std::max(need, std::max<size_t>(64, capacity_ + capacity_ / 2));
        auto next = std::make_unique_for_overwrite<uint8_t[]>(cap);
        if (size_)
            memcpy(next.get(), memory_, size_);
        memory_ = next.get();
        owned_ = std::move(next);
        capacity_ = cap;
    }

public:
    explicit Bytes(std::span<uint8_t> spare = {}) : memory_(spare.data()), capacity_(spare.size()) {}
    size_t size() const {
        return size_;
    }
    uint8_t* data() const {
        return memory_;
    }
    void reset(size_t capacity = 0) {
        size_ = 0;
        if (capacity > capacity_)
            grow(capacity);
    }
    uint8_t* append(size_t n) {
        if (n > SIZE_MAX - size_)
            throw std::length_error("Parquet buffer is too large");
        if (size_ + n > capacity_)
            grow(size_ + n);
        auto* dest = memory_ + size_;
        size_ += n;
        return dest;
    }
    void byte(uint8_t x) {
        *append(1) = x;
    }
    void varint(uint64_t n) {
        while (n >= 128) {
            byte(uint8_t(n) | 128);
            n >>= 7;
        }
        byte(uint8_t(n));
    }
    void integer(uint64_t n) {
        varint(n << 1);
    } // Metadata integers in these schemas are nonnegative.
    void raw(Text s) {
        if (!s.empty())
            memcpy(append(s.size()), s.data(), s.size());
    }
    void thrift_string(Text s) {
        varint(s.size());
        raw(s);
    }
    static uint32_t narrow(size_t n) {
        if (n > INT32_MAX)
            throw std::length_error("Parquet page or byte array exceeds INT32");
        return uint32_t(n);
    }
    template <class T> static void write_le(uint8_t* p, T n) {
        if constexpr (std::endian::native == std::endian::big) {
            if constexpr (sizeof(T) == 4)
                n = T(__builtin_bswap32(uint32_t(n)));
            else
                n = T(__builtin_bswap64(uint64_t(n)));
        }
        memcpy(p, &n, sizeof n);
    }
    void plain_string(Text value) {
        write_le(append(4), narrow(value.size()));
        raw(value);
    }
};
enum CompactType : uint8_t { i32 = 5, i64 = 6, binary = 8, list = 9, structure = 12 };
struct Fields {
    Bytes& data;
    unsigned last = 0;
    void field(unsigned id, CompactType type) {
        data.byte(uint8_t((id - last) << 4) | type);
        last = id;
    }
    void number(unsigned id, CompactType type, uint64_t n) {
        field(id, type);
        data.integer(n);
    }
    void string(unsigned id, Text v) {
        field(id, binary);
        data.thrift_string(v);
    }
    void sequence(unsigned id, CompactType elem, size_t count) {
        field(id, list);
        data.byte(uint8_t(std::min<size_t>(count, 15) << 4) | elem);
        if (count >= 15)
            data.varint(count);
    }
    void end() {
        data.byte(0);
    }
};
enum class Storage { integer32 = 1, integer64 = 2, text = 6 };
struct Column {
    const char* name;
    Storage storage;
    int logical = -1;
    bool nullable = false;
};
constexpr Column user_schema[] = {
    {"user_id", Storage::text, 0},         {"day", Storage::integer32, 6},
    {"sessions", Storage::integer32},      {"events", Storage::integer32},
    {"revenue_cents", Storage::integer64}, {"top_category", Storage::text, 0, true}};
constexpr Column campaign_schema[] = {{"campaign_id", Storage::text, 0},
                                      {"channel", Storage::text, 0},
                                      {"day", Storage::integer32, 6},
                                      {"sessions", Storage::integer64},
                                      {"purchasing_sessions", Storage::integer64},
                                      {"revenue_cents", Storage::integer64},
                                      {"assisted_sessions", Storage::integer64}};
struct ChunkLocation {
    uint64_t start, bytes;
};
struct Group {
    size_t rows;
    uint64_t bytes = 0;
    std::array<ChunkLocation, 7> columns{};
};
struct FileCloser {
    void operator()(FILE* f) const {
        fclose(f);
    }
};
class File {
    std::unique_ptr<FILE, FileCloser> stream_;
    std::span<const Column> schema_;
    uint64_t offset_ = 0, rows_ = 0;
    std::vector<Group> groups_;
    Bytes header_;
    void write(const void* data, size_t n) {
        if (n && fwrite(data, 1, n, stream_.get()) != n)
            throw std::system_error(errno, std::generic_category(), "write Parquet");
        offset_ += n;
    }
    void column_meta(const Column& c, const ChunkLocation& chunk, size_t n) {
        Fields container{header_};
        container.number(2, i64, 0);
        container.field(3, structure);
        Fields m{header_};
        m.number(1, i32, unsigned(c.storage));
        m.sequence(2, i32, c.nullable ? 2 : 1);
        header_.integer(0);
        if (c.nullable)
            header_.integer(3);
        m.sequence(3, binary, 1);
        header_.thrift_string(c.name);
        m.number(4, i32, 0);
        m.number(5, i64, n);
        m.number(6, i64, chunk.bytes);
        m.number(7, i64, chunk.bytes);
        m.number(9, i64, chunk.start);
        m.end();
        container.end();
    }

public:
    Bytes page;
    File(const fs::path& name, std::span<const Column> schema, std::span<uint8_t> spare = {})
        : stream_(fopen(name.c_str(), "wb")), schema_(schema), page(spare) {
        if (!stream_)
            throw std::system_error(errno, std::generic_category(), "open " + name.string());
        write("PAR1", 4);
    }
    void group(size_t rows) {
        groups_.push_back({rows});
        rows_ += rows;
    }
    void column(size_t index) {
        auto& g = groups_.back();
        unsigned bytes = Bytes::narrow(page.size());
        header_.reset(80);
        Fields head{header_};
        head.number(1, i32, 0);
        head.number(2, i32, bytes);
        head.number(3, i32, bytes);
        head.field(5, structure);
        Fields d{header_};
        d.number(1, i32, g.rows);
        d.number(2, i32, 0);
        d.number(3, i32, 3);
        d.number(4, i32, 3);
        d.end();
        head.end();
        auto& loc = g.columns[index];
        loc = {offset_, header_.size() + bytes};
        g.bytes += loc.bytes;
        write(header_.data(), header_.size());
        write(page.data(), page.size());
    }
    template <class T, class F> void numbers(size_t index, size_t n, F get) {
        page.reset(n * sizeof(T));
        uint8_t* dest = page.append(n * sizeof(T));
        for (size_t i = 0; i < n; ++i)
            Bytes::write_le(dest + i * sizeof(T), T(get(i)));
        column(index);
    }
    void close() {
        header_.reset();
        Fields m{header_};
        m.number(1, i32, 1);
        m.sequence(2, structure, schema_.size() + 1);
        Fields root{header_};
        root.string(4, "data");
        root.number(5, i32, schema_.size());
        root.end();
        for (const auto& c : schema_) {
            Fields s{header_};
            s.number(1, i32, unsigned(c.storage));
            s.number(3, i32, unsigned(c.nullable));
            s.string(4, c.name);
            if (c.logical >= 0)
                s.number(6, i32, unsigned(c.logical));
            s.end();
        }
        m.number(3, i64, rows_);
        m.sequence(4, structure, groups_.size());
        for (const auto& g : groups_) {
            Fields rg{header_};
            rg.sequence(1, structure, schema_.size());
            for (size_t i = 0; i < schema_.size(); ++i)
                column_meta(schema_[i], g.columns[i], g.rows);
            rg.number(2, i64, g.bytes);
            rg.number(3, i64, g.rows);
            rg.end();
        }
        m.end();
        uint8_t size[4];
        Bytes::write_le(size, Bytes::narrow(header_.size()));
        write(header_.data(), header_.size());
        write(size, 4);
        write("PAR1", 4);
        if (fclose(stream_.release()))
            throw std::system_error(errno, std::generic_category(), "close Parquet");
    }
};
// Allow the four-byte length, the definition bit, and the page's level header.
size_t rows_per_group(size_t longest_string) {
    if (longest_string > size_t(INT32_MAX) - 12)
        throw std::length_error("String exceeds Parquet page capacity");
    return std::min<size_t>(65536, (size_t(INT32_MAX) - 7) / (longest_string + 5));
}
} // namespace

void write_users(const fs::path& path, const UserDays& rows, const Keys& users,
                 const Dimensions& dimensions, std::span<uint8_t> spare) {
    File f(path, user_schema, spare);
    size_t width = users.longest();
    for (const auto& category : dimensions.categories)
        width = std::max(width, category.size());
    const size_t group_rows = rows_per_group(width);
    for (size_t base = 0; base < rows.size();) {
        auto group = rows.range(base, group_rows);
        const auto* row = group.data();
        size_t n = group.size();
        base += n;
        f.group(n);
        auto& data = f.page;
        data.reset(n * 13);
        Key previous = 0;
        std::string id;
        for (size_t i = 0; i < n; ++i) {
            if (row[i].person != previous) {
                previous = row[i].person;
                id = users.decode(previous);
            }
            data.plain_string(id);
        }
        f.column(0);
        f.numbers<int32_t>(1, n, [&](size_t i) { return row[i].day; });
        f.numbers<int32_t>(2, n, [&](size_t i) { return row[i].sessions; });
        f.numbers<int32_t>(3, n, [&](size_t i) { return row[i].events; });
        f.numbers<int64_t>(4, n, [&](size_t i) { return row[i].revenue; });
        size_t groups = (n + 7) / 8;
        data.reset(12 + groups + n * 3);
        data.append(4);
        data.varint(2 * groups + 1);
        size_t bitstart = data.size();
        auto* levels = data.append(groups);
        for (size_t i = 0; i < n; i += 8) {
            unsigned bits = 0;
            for (unsigned j = 0; j < std::min<size_t>(8, n - i); ++j)
                bits |= unsigned(row[i + j].top != 0) << j;
            levels[i >> 3] = uint8_t(bits);
        }
        Bytes::write_le(data.data(), uint32_t(bitstart + groups - 4));
        for (size_t i = 0; i < n; ++i)
            if (row[i].top)
                data.plain_string(dimensions.categories[row[i].top - 1]);
        f.column(5);
    }
    f.close();
}
void write_campaigns(const fs::path& path, const CampaignDays& totals, const Dimensions& dimensions) {
    struct Row {
        uint32_t campaign;
        int32_t day;
        Totals sum;
    };
    std::vector<Row> rows;
    totals.each([&](uint32_t c, int32_t day, const Totals& t) { rows.push_back({c, day, t}); });
    File f(path, campaign_schema);
    size_t width = 0;
    for (const auto& name : dimensions.names)
        width = std::max({width, name.id.size(), name.channel.size()});
    const size_t group_rows = rows_per_group(width);
    for (size_t base = 0; base < rows.size(); base += group_rows) {
        auto* row = rows.data() + base;
        size_t n = std::min(group_rows, rows.size() - base);
        f.group(n);
        for (unsigned j = 0; j < 2; ++j) {
            f.page.reset(n * 12);
            for (size_t i = 0; i < n; ++i) {
                auto& c = dimensions.names[row[i].campaign];
                f.page.plain_string(j ? c.channel : c.id);
            }
            f.column(j);
        }
        f.numbers<int32_t>(2, n, [&](size_t i) { return row[i].day; });
        f.numbers<int64_t>(3, n, [&](size_t i) { return row[i].sum.sessions; });
        f.numbers<int64_t>(4, n, [&](size_t i) { return row[i].sum.purchasing; });
        f.numbers<int64_t>(5, n, [&](size_t i) { return row[i].sum.revenue; });
        f.numbers<int64_t>(6, n, [&](size_t i) { return row[i].sum.assists; });
    }
    f.close();
}
} // namespace shop
