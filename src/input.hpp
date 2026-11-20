#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace shop {
namespace fs = std::filesystem;
using Text = std::string_view;
using Time = int64_t;
constexpr Time day_ms = 86'400'000;
constexpr Time week_ms = 7 * day_ms;
constexpr Time invalid_time = std::numeric_limits<Time>::min();

struct Chunk { const char* begin; const char* end; };

class MappedCsv {
    char* data_ = nullptr;
    size_t size_ = 0;
public:
    explicit MappedCsv(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        struct stat info{};
        if (fstat(fd, &info) != 0) { close(fd); throw std::runtime_error("cannot stat " + path.string()); }
        size_ = info.st_size;
        if (size_) data_ = static_cast<char*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        close(fd);
        if (data_ == MAP_FAILED) throw std::runtime_error("cannot map " + path.string());
    }
    ~MappedCsv() { if (data_) munmap(data_, size_); }
    MappedCsv(const MappedCsv&) = delete;
    MappedCsv& operator=(const MappedCsv&) = delete;
    size_t size() const { return size_; }
    void chunks(std::vector<Chunk>& out) const {
        if (!size_) return;
        const char* end = data_ + size_;
        auto after_line = [end](const char* p) {
            const auto* nl = static_cast<const char*>(memchr(p, '\n', end - p));
            return nl ? nl + 1 : end;
        };
        const char* begin = after_line(data_);  // Only the first physical line is a header.
        constexpr size_t target = 512 * 1024;
        while (begin < end) {
            const char* stop = size_t(end - begin) > target ? after_line(begin + target) : end;
            out.push_back({begin, stop});
            begin = stop;
        }
    }
};

class CsvFiles {
    std::vector<std::unique_ptr<MappedCsv>> mappings_;
public:
    std::vector<Chunk> chunks;
    size_t bytes = 0;
    explicit CsvFiles(const fs::path& path) {
        auto add = [&](const fs::path& p) {
            auto file = std::make_unique<MappedCsv>(p);
            bytes += file->size();
            file->chunks(chunks);
            mappings_.push_back(std::move(file));
        };
        if (fs::is_directory(path)) {
            for (const auto& f : fs::directory_iterator(path))
                if (f.is_regular_file() && f.path().extension() == ".csv") add(f.path());
        } else add(path);
    }
};

template<size_t N, class F>
inline const char* scalar_row(const char* row, const char* end, F& handle) {
    const auto* nl = static_cast<const char*>(memchr(row, '\n', end - row));
    const char* stop = nl ? nl : end;
    const char* next = nl ? nl + 1 : end;
    std::array<Text, N> fields;
    const char* field = row;
    for (size_t i = 0; i < N; ++i) {
        const auto* comma = static_cast<const char*>(memchr(field, ',', stop - field));
        if ((i + 1 < N) != (comma != nullptr)) return next;
        const char* tail = comma ? comma : stop;
        Text value(field, tail - field);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (value.find('"') != Text::npos) return next;
        fields[i] = value;
        if (comma) field = comma + 1;
    }
    handle(fields);
    return next;
}

#if defined(__AVX2__) || defined(__ARM_NEON)
struct Marks { uint64_t commas, lines, quotes; };
#if defined(__ARM_NEON)
inline uint16_t bits16(uint8x16_t v) {
    // Narrow adjacent comparison bytes, then multiply to gather their top bits.
    uint8x8_t nibbles = vshrn_n_u16(vreinterpretq_u16_u8(v), 4);
    uint64_t x = vget_lane_u64(vreinterpret_u64_u8(nibbles), 0) & 0x1111111111111111ULL;
    x = (x | (x >> 3)) & 0x0303030303030303ULL;
    x = (x | (x >> 6)) & 0x000f000f000f000fULL;
    x = (x | (x >> 12)) & 0x000000ff000000ffULL;
    return static_cast<uint16_t>(x | (x >> 24));
}
#endif
inline Marks scan64(const char* p) {
#if defined(__AVX512BW__)
    auto bytes = _mm512_loadu_si512(p);
    return {_mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8(',')),
            _mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8('\n')),
            _mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8('"'))};
#elif defined(__AVX2__)
    auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    auto b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
    auto mask = [&](char c) {
        auto needle = _mm256_set1_epi8(c);
        uint32_t lo = _mm256_movemask_epi8(_mm256_cmpeq_epi8(a, needle));
        uint32_t hi = _mm256_movemask_epi8(_mm256_cmpeq_epi8(b, needle));
        return uint64_t(lo) | (uint64_t(hi) << 32);
    };
    return {mask(','), mask('\n'), mask('"')};
#else
    Marks m{};
    for (unsigned i = 0; i < 64; i += 16) {
        auto v = vld1q_u8(reinterpret_cast<const uint8_t*>(p + i));
        m.commas |= uint64_t(bits16(vceqq_u8(v, vdupq_n_u8(',')))) << i;
        m.lines  |= uint64_t(bits16(vceqq_u8(v, vdupq_n_u8('\n')))) << i;
        m.quotes |= uint64_t(bits16(vceqq_u8(v, vdupq_n_u8('"')))) << i;
    }
    return m;
#endif
}
#endif

template<size_t N, class F>
void read_rows(Chunk chunk, F&& handle) {
    auto [row, end] = chunk;
#if defined(__AVX2__) || defined(__ARM_NEON)
    while (end - row >= 128) {
        Marks low = scan64(row), last = low;
        bool crosses = !low.lines;
        if (crosses) last = scan64(row + 64);
        if (!last.lines) { row = scalar_row<N>(row, end, handle); continue; }
        unsigned last_pos = std::countr_zero(last.lines);
        unsigned length = last_pos + (crosses ? 64 : 0);
        uint64_t before = (uint64_t(1) << last_pos) - 1;
        uint64_t a = crosses ? low.commas : (low.commas & before);
        uint64_t b = crosses ? (last.commas & before) : 0;
        if (std::popcount(a) + std::popcount(b) == N - 1) {
            unsigned quotes = std::popcount(last.quotes & before) + (crosses ? std::popcount(low.quotes) : 0);
            unsigned start = 0, opens = 0, mismatch = 0;
            std::array<Text, N> fields;
            for (size_t i = 0; i < N; ++i) {
                unsigned finish = length;
                if (i != N - 1) {
                    if (a) { finish = std::countr_zero(a); a &= a - 1; }
                    else { finish = 64 + std::countr_zero(b); b &= b - 1; }
                }
                bool open_quote = row[start] == '"';
                bool close_quote = finish > start && row[finish - 1] == '"';
                opens += open_quote;
                mismatch |= (open_quote != close_quote) | (open_quote && finish - start < 2);
                // A lone quote is rejected below. Clamp the view length even on that path.
                unsigned trim = open_quote && finish - start >= 2 ? 2 : 0;
                fields[i] = Text(row + start + open_quote, finish - start - trim);
                start = finish + 1;
            }
            if (!mismatch && quotes == 2 * opens) handle(fields);
        }
        row += length + 1;
    }
#endif
    while (row < end) row = scalar_row<N>(row, end, handle);
}

inline int pair_value(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
inline bool digits2(const char* p) { return unsigned(p[0] - '0') < 10 && unsigned(p[1] - '0') < 10; }
inline int64_t decimal(Text s, int64_t limit) {
    if (s.empty()) return -1;
    int64_t n = 0;
    for (char ch : s) {
        unsigned digit = static_cast<unsigned>(ch - '0');
        if (digit > 9 || n > limit / 10 || (n == limit / 10 && digit > unsigned(limit % 10))) return -1;
        n = n * 10 + digit;
    }
    return n;
}

// Parse the common 13-digit epoch form without a serial multiply-by-ten chain.
inline Time epoch13(const char* p) {
    uint64_t head, tail;
    memcpy(&head, p, 8);
    memcpy(&tail, p + 5, 8);
    if constexpr (std::endian::native == std::endian::big) {
        head = __builtin_bswap64(head);
        tail = __builtin_bswap64(tail);
    }
    constexpr uint64_t zero = 0x3030303030303030ULL;
    constexpr uint64_t upper = 0x4646464646464646ULL;
    constexpr uint64_t high = 0x8080808080808080ULL;
    if (((head - zero) | (head + upper) | (tail - zero) | (tail + upper)) & high) return invalid_time;
    head -= zero;
    head = (head * 10 + (head >> 8)) & 0x00ff00ff00ff00ffULL;
    head = (head * 100 + (head >> 16)) & 0x0000ffff0000ffffULL;
    uint32_t first_eight = uint32_t(head) * 10000 + uint32_t(head >> 32);
    return Time(first_eight) * 100000 + (p[8] - '0') * 10000 + pair_value(p+9) * 100 + pair_value(p+11);
}

class Clock {
    struct Month { uint64_t prefix = 0; int first = 0, days = 0; };
    std::array<Month, 16> months_{};
    static Month month(const char* p) {
        using namespace std::chrono;
        if (p[4] != '-' || p[7] != '-' || !digits2(p) || !digits2(p+2) || !digits2(p+5)) return {};
        year_month ym = year(100 * pair_value(p) + pair_value(p+2)) / unsigned(pair_value(p+5));
        if (!ym.ok()) return {};
        int first = sys_days(ym / 1).time_since_epoch().count();
        return {0, first, int(unsigned((ym / last).day()))};
    }
    static unsigned slot(const char* p) { return static_cast<unsigned char>(p[6]) & 15; }
public:
    // The manifest locates a small month cache. Other dates take the same calendar path on demand.
    void warm(Text f1) {
        using namespace std::chrono;
        year_month center = year(100 * pair_value(f1.data()) + pair_value(f1.data()+2)) / unsigned(pair_value(f1.data()+5));
        for (int delta = -2; delta <= 2; ++delta) {
            year_month ym = center + months(delta);
            int y = int(ym.year()), m = unsigned(ym.month());
            if (y < 0 || y > 9999) continue;
            char prefix[16];
            snprintf(prefix, sizeof(prefix), "%04d-%02d-", y, m);
            auto& v = months_[slot(prefix)];
            v = month(prefix);
            memcpy(&v.prefix, prefix, 8);
        }
    }
    template<bool trusted = false>
    Time iso(Text s) const {
        const char* p = s.data();
        if constexpr (!trusted) {
            if (p[10] != 'T' || p[13] != ':' || p[16] != ':' || !digits2(p+8) ||
                !digits2(p+11) || !digits2(p+14) || !digits2(p+17)) return invalid_time;
        }
        uint64_t prefix;
        memcpy(&prefix, p, 8);
        Month m = months_[slot(p)];
        if (m.prefix != prefix || !m.days) m = month(p);
        int d = pair_value(p+8), h = pair_value(p+11), min = pair_value(p+14), sec = pair_value(p+17);
        if constexpr (!trusted) {
            if (d < 1 || d > m.days || h > 23 || min > 59 || sec > 59) return invalid_time;
        }
        int offset = 0;
        if constexpr (!trusted) {
            if (s.size() == 20) { if (p[19] != 'Z') return invalid_time; }
            else {
                if ((p[19] != '+' && p[19] != '-') || p[22] != ':' || !digits2(p+20) || !digits2(p+23)) return invalid_time;
                int oh = pair_value(p+20), om = pair_value(p+23);
                if (oh > 23 || om > 59) return invalid_time;
                offset = (p[19] == '+' ? 1 : -1) * (oh * 60 + om);
            }
        }
        return (Time(m.first) + d - 1) * day_ms + ((h * 60 + min - offset) * 60 + sec) * 1000LL;
    }
    Time parse(Text s) const {
        if (s.size() == 13) return epoch13(s.data());
        if ((s.size() == 20 || s.size() == 25) && s[4] == '-') return iso(s);
        const int64_t n = decimal(s, std::numeric_limits<int64_t>::max());
        return n < 0 ? invalid_time : n;
    }
};

struct Window {
    Clock clock;
    Time start, end;
    explicit Window(const fs::path& manifest) {
        std::ifstream f(manifest);
        std::string json((std::istreambuf_iterator<char>(f)), {});
        auto field = [&](const std::string& key) -> Text {
            auto name = json.find('"' + key + '"');
            auto colon = json.find(':', name);
            auto quote = json.find('"', colon);
            auto last = quote == std::string::npos ? quote : json.find('"', quote + 1);
            if (last == std::string::npos || last - quote != 21) throw std::runtime_error("bad manifest " + key);
            return Text(json).substr(quote + 1, 20);
        };
        auto begin = field("window_start");
        clock.warm(begin);
        start = clock.parse(begin);
        end = clock.parse(field("window_end"));
        if (start == invalid_time || end == invalid_time || end <= start) throw std::runtime_error("invalid window");
    }
};
inline int32_t utc_day(Time t) { return int32_t(t / day_ms - (t % day_ms < 0)); }
} // namespace shop
