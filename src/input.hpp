#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__SSE2__)
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

struct Chunk { const char* begin; const char* limit; const char* file_end; };

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
        constexpr size_t target = 512 * 1024;
        for (size_t offset = 0; offset < size_; offset += target)
            out.push_back({data_ + offset, data_ + std::min(offset + target, size_), end});
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

// Position of the zero-based ordinal set bit. The caller has checked the word's rank.
inline unsigned select_bit(uint64_t mask, unsigned ordinal) {
#if defined(__BMI2__) && defined(__x86_64__)
    return std::countr_zero(_pdep_u64(uint64_t(1) << ordinal, mask));
#else
    unsigned bit = 0;
    for (unsigned half = 32; half; half /= 2) {
        unsigned lower = std::popcount(mask & ((uint64_t(1) << half) - 1));
        if (ordinal >= lower) {
            bit += half;
            mask >>= half;
            ordinal -= lower;
        }
    }
    return bit;
#endif
}

#if defined(__SSE2__) || defined(__ARM_NEON)
struct Marks { uint64_t commas, lines, quotes; };
#if defined(__ARM_NEON) && !defined(__aarch64__)
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
#if defined(__aarch64__) && defined(__ARM_NEON)
inline uint64_t neon_mask64(const uint8x16x4_t& blocks, uint8_t c) {
    // Reduce eight match lanes to one mask byte while the data is still in NEON registers.
    const uint8x16_t weights = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    auto match = [&](uint8x16_t b) { return vandq_u8(vceqq_u8(b, vdupq_n_u8(c)), weights); };
    auto ab = vpaddq_u8(match(blocks.val[0]), match(blocks.val[1]));
    auto cd = vpaddq_u8(match(blocks.val[2]), match(blocks.val[3]));
    auto four = vpaddq_u8(ab, cd);
    auto eight = vpadd_u8(vget_low_u8(four), vget_high_u8(four));
    return vget_lane_u64(vreinterpret_u64_u8(eight), 0);
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
#elif defined(__SSE2__)
    Marks m{};
    for (unsigned i = 0; i < 64; i += 16) {
        auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        m.commas |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(',')))) << i;
        m.lines  |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8('\n')))) << i;
        m.quotes |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')))) << i;
    }
    return m;
#elif defined(__aarch64__) && defined(__ARM_NEON)
    auto blocks = vld1q_u8_x4(reinterpret_cast<const uint8_t*>(p));
    return {neon_mask64(blocks, ','), neon_mask64(blocks, '\n'), neon_mask64(blocks, '"')};
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

#if defined(__SSE2__) || defined(__ARM_NEON)
inline uint64_t quote_parity(uint64_t q) {
#if defined(__PCLMUL__)
    auto v = _mm_clmulepi64_si128(_mm_cvtsi64_si128(q), _mm_set1_epi64x(-1), 0);
    return uint64_t(_mm_cvtsi128_si64(v));
#elif defined(__ARM_FEATURE_AES)
    return vgetq_lane_u64(vreinterpretq_u64_p128(vmull_p64(q, ~uint64_t(0))), 0);
#else
    q ^= q << 1; q ^= q << 2; q ^= q << 4;
    q ^= q << 8; q ^= q << 16; return q ^ (q << 32);
#endif
}

// Index a tile with aligned-in-stream, non-overlapping 64-byte scans. Quote
// parity must be even at every separator, and a quote must touch a field edge.
// A violation certifies a bad physical line. Retain every fully indexed line before it.
template<class Sink>
inline size_t scan_csv_tile(const char* data, size_t length, Sink& sink) {
    uint64_t carry = 0, starts_field = 1;
    size_t offset = 0;
#if defined(__AVX512DQ__) && defined(__AVX512BW__)
    for (; offset + 512 <= length; offset += 512) {
        alignas(64) uint64_t edges[10], quotes[8], lines[8];
        edges[0] = starts_field << 63;
        for (unsigned j = 0; j < 8; ++j) {
            auto m = scan64(data + offset + j * 64);
            edges[j + 1] = m.commas | m.lines;
            quotes[j] = m.quotes;
            lines[j] = m.lines;
        }
        edges[9] = offset + 512 < length && (data[offset + 512] == ',' || data[offset + 512] == '\n');
        auto e = _mm512_loadu_si512(edges + 1), q = _mm512_load_si512(quotes);
        auto previous = _mm512_load_si512(edges), following = _mm512_loadu_si512(edges + 2);
        auto adjacent = _mm512_slli_epi64(e, 1) | _mm512_srli_epi64(previous, 63) | _mm512_srli_epi64(e, 1) |
                        _mm512_slli_epi64(following, 63);
        // Compute quote parity in eight independent words. Prefix XOR of their
        // high bits supplies the incoming state for every word in the group.
        auto par = q;
        par ^= _mm512_slli_epi64(par, 1);
        par ^= _mm512_slli_epi64(par, 2);
        par ^= _mm512_slli_epi64(par, 4);
        par ^= _mm512_slli_epi64(par, 8);
        par ^= _mm512_slli_epi64(par, 16);
        par ^= _mm512_slli_epi64(par, 32);
        unsigned words = _mm512_movepi64_mask(par);
        words ^= words << 1;
        words ^= words << 2;
        words ^= words << 4;
        unsigned starting = (words << 1) ^ unsigned(carry);
        par = _mm512_mask_xor_epi64(par, __mmask8(starting), par, _mm512_set1_epi64(-1));
        auto bad = _mm512_andnot_si512(adjacent, q) | (par & e);
        unsigned failed = _mm512_test_epi64_mask(bad, bad);
        if (failed) {
            unsigned j = std::countr_zero(failed);
            for (unsigned i = 0; i < j; ++i)
                sink.append_separators(offset + 64 * i, edges[i + 1], lines[i]);
            alignas(64) uint64_t errors[8];
            _mm512_store_si512(errors, bad);
            uint64_t before = (errors[j] & -errors[j]) - 1;
            sink.append_separators(offset + 64 * j, edges[j + 1] & before, lines[j] & before);
            return offset + 64 * j + std::countr_zero(errors[j]);
        }
        carry ^= uint64_t(0) - uint64_t((words >> 7) & 1);
        for (unsigned i = 0; i < 8; ++i)
            sink.append_separators(offset + 64 * i, edges[i + 1], lines[i]);

        starts_field = edges[8] >> 63;
    }
#endif
    for (; offset < length; offset += 64) {
        size_t left = length - offset;
        Marks m;
        if (left >= 64) m = scan64(data + offset);
        else {
            alignas(64) char tail[64]{};
            memcpy(tail, data + offset, left);
            m = scan64(tail);
        }
        uint64_t ends = m.commas | m.lines;
        if (m.quotes || carry) {
            uint64_t next_end = left > 64 && (data[offset + 64] == ',' || data[offset + 64] == '\n');
            uint64_t adjacent = (ends << 1) | starts_field | (ends >> 1) | (next_end << 63);
            uint64_t parity = quote_parity(m.quotes) ^ carry;
            uint64_t errors = (m.quotes & ~adjacent) | (parity & ends);
            if (errors) {
                uint64_t before = (errors & -errors) - 1;
                sink.append_separators(offset, ends & before, m.lines & before);
                return offset + std::countr_zero(errors);
            }
            carry = uint64_t(0) - (parity >> 63);
        }
        starts_field = ends >> 63;
        sink.append_separators(offset, ends, m.lines);
    }
    return length;
}

template<size_t N>
struct CsvTile {
    static constexpr unsigned target = 8192, slack = 192;
    static constexpr unsigned words = (target + slack + 63) / 64;
    static constexpr uint16_t wrong_columns = 0x8000;
    std::array<uint16_t, target + slack> line_ends;
    std::array<uint64_t, words + 1> commas{};
    std::array<uint16_t, words + 1> prefix;
    unsigned lines, comma_count, previous_count;

    void append_separators(size_t offset, uint64_t ends, uint64_t nl) {
        unsigned word = unsigned(offset >> 6);
        uint64_t mask = ends ^ nl;
        commas[word] = mask;
        for (uint64_t bits = nl; bits; bits &= bits - 1) {
            uint64_t bit = bits & -bits;
            unsigned count = comma_count + std::popcount(mask & (bit - 1));
            unsigned end = unsigned(offset) + std::countr_zero(bit);
            if (count - previous_count != N - 1) end |= wrong_columns;
            line_ends[lines++] = uint16_t(end);
            previous_count = count;
        }
        comma_count += std::popcount(mask);
        prefix[word + 1] = uint16_t(comma_count);
    }
    size_t index(const char* data, size_t length) {
        lines = comma_count = previous_count = 0;
        prefix[0] = 0;
        return scan_csv_tile(data, length, *this);
    }
    uint64_t commas_from(unsigned pos) const {
        unsigned w = pos >> 6, shift = pos & 63;
        return shift ? (commas[w] >> shift) | (commas[w + 1] << (64 - shift)) : commas[w];
    }
    unsigned rank(unsigned pos) const {
        unsigned w = pos >> 6;
        return prefix[w] + std::popcount(commas[w] & ((uint64_t(1) << (pos & 63)) - 1));
    }
    unsigned select(unsigned from, unsigned end, unsigned ordinal) const {
        unsigned lo = from >> 6, hi = end >> 6;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            if (prefix[mid + 1] <= ordinal) lo = mid + 1;
            else hi = mid;
        }
        return 64 * lo + select_bit(commas[lo], ordinal - prefix[lo]);
    }
    static Text field(const char* first, const char* stop) {
        unsigned quote = *first == '"';
        return Text(first + quote, size_t(stop - first) - 2 * quote);
    }
    // The row's column count was certified by the structural pass. Short fields
    // expose the next delimiters in one mask. Wider fields use directory ranks.
    template<size_t Count>
    std::array<unsigned, Count> next_commas(unsigned start, unsigned end) const {
        if constexpr (Count == 0) return {};
        std::array<unsigned, Count> result;
        uint64_t mask = commas_from(start);
        for (auto& pos : result) {
            pos = start + std::countr_zero(mask);
            mask &= mask - 1;
        }
        if (result.back() == start + 64) {
            unsigned ordinal = rank(start);
            for (unsigned i = 0; i < Count; ++i) result[i] = select(start, end, ordinal + i);
        }
        return result;
    }
    std::array<unsigned, 5> event_stops(unsigned start, unsigned end) const {
        // Two nine-byte IDs, including optional quotes and both separators, fit
        // in 24 bytes. This allows both bitmap loads to issue independently.
        if (end - start >= 24) {
            uint64_t ids = commas_from(start) & 0xffffff;
            unsigned after_ids = start + 24;
            uint64_t body = commas_from(after_ids);
            unsigned time = after_ids + std::countr_zero(body);
            body &= body - 1;
            unsigned ingest = after_ids + std::countr_zero(body);
            body &= body - 1;
            unsigned kind = after_ids + std::countr_zero(body);
            if (std::popcount(ids) == 2 && body) {
                unsigned id = start + std::countr_zero(ids);
                unsigned user = start + 63 - std::countl_zero(ids);
                return {id, user, time, ingest, kind};
            }
        }
        auto [id, user, time] = next_commas<3>(start, end);
        auto [ingest, kind] = next_commas<2>(time + 1, end);
        return {id, user, time, ingest, kind};
    }
    template<class F, class Project>
    void visit(const char* data, F& handle, Project& project) const {
        unsigned start = 0;
        for (unsigned i = 0; i < lines; ++i) {
            unsigned marked = line_ends[i], end = marked & ~wrong_columns;
            if (!(marked & wrong_columns)) {
                if constexpr (std::is_same_v<Project, std::nullptr_t>) {
                    auto stops = next_commas<N - 1>(start, end);
                    std::array<Text, N> fields;
                    unsigned first = start;
                    for (size_t j = 0; j < N - 1; ++j) {
                        fields[j] = field(data + first, data + stops[j]);
                        first = stops[j] + 1;
                    }
                    fields[N - 1] = field(data + first, data + end);
                    handle(fields);
                } else {
                    static_assert(N == 7);
                    auto [id, user, time, ingest, kind] = event_stops(start, end);
                    unsigned product = kind + 1;
                    bool has_product = data[product] != ',' && !(data[product] == '"' && data[product + 1] == '"');
                    auto purchase = [&] {
                        unsigned stop = next_commas<1>(product, end)[0];
                        return std::pair{field(data + product, data + stop), field(data + stop + 1, data + end)};
                    };
                    project(field(data + start, data + id), field(data + id + 1, data + user),
                            field(data + user + 1, data + time), field(data + time + 1, data + ingest),
                            field(data + ingest + 1, data + kind), has_product, purchase);
                }
            }
            start = end + 1;
        }
    }
};

#if defined(__AVX512BW__)
// Store every separator's byte offset. A valid row then occupies exactly N
// consecutive slots, regardless of quoted fields, length, or timestamp form.
template<size_t N>
struct OffsetTile {
    static constexpr unsigned target = CsvTile<N>::target;
    static constexpr unsigned slack = CsvTile<N>::slack;
    std::array<uint32_t, target + slack + 1> offsets;
    std::array<uint16_t, (target + slack) / N + 1> rows;
    unsigned count, lines, previous_stop, available;

    void append_separators(size_t offset, uint64_t ends, uint64_t newlines) {
        const auto lanes = _mm512_setr_epi32(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
        const auto positions = _mm512_add_epi32(lanes, _mm512_set1_epi32(int(offset)));
        auto* dest = offsets.data() + count + 1;
        _mm512_mask_compressstoreu_epi32(dest, __mmask16(ends), positions);
        const auto step = _mm512_set1_epi32(16);
        _mm512_mask_compressstoreu_epi32(dest + std::popcount(uint16_t(ends)), __mmask16(ends >> 16),
                                         _mm512_add_epi32(positions, step));
        _mm512_mask_compressstoreu_epi32(dest + std::popcount(uint32_t(ends)), __mmask16(ends >> 32),
                                         _mm512_add_epi32(positions, _mm512_set1_epi32(32)));
        _mm512_mask_compressstoreu_epi32(dest + std::popcount(ends & 0xffffffffffffULL), __mmask16(ends >> 48),
                                         _mm512_add_epi32(positions, _mm512_set1_epi32(48)));
        for (uint64_t bits = newlines; bits; bits &= bits - 1) {
            auto bit = bits & -bits;
            unsigned stop = count + std::popcount(ends & (bit - 1)) + 1;
            if (stop - previous_stop == N) rows[lines++] = uint16_t(previous_stop);
            previous_stop = stop;
        }
        count += std::popcount(ends);
    }
    size_t index(const char* data, size_t length) {
        available = unsigned(length);
        count = lines = previous_stop = 0;
        offsets[0] = UINT32_MAX;
        return scan_csv_tile(data, length, *this);
    }
    template<class F, class Project>
    void visit(const char* data, F& handle, Project& project) const {
        auto one = [&](unsigned i) {
            const uint32_t* cuts = offsets.data() + rows[i];
            auto text = [&](size_t column) {
                const char* first = data + uint32_t(cuts[column] + 1);
                return CsvTile<N>::field(first, data + cuts[column + 1]);
            };
            if constexpr (std::is_same_v<Project, std::nullptr_t> || N == 4) {
                std::array<Text, N> fields;
                for (size_t j = 0; j < N; ++j) fields[j] = text(j);
                handle(fields);
            } else {
                static_assert(N == 7);
                unsigned product = cuts[5] + 1;
                bool has_product = data[product] != ',' && !(data[product] == '"' && data[product + 1] == '"');
                project(text(0), text(1), text(2), text(3), text(4), has_product,
                        [&] { return std::pair{text(5), text(6)}; });
            }
        };
        unsigned i = 0;
        if constexpr (requires { project.batch(data, offsets.data(), rows.data()); }) {
            for (; i + 8 <= lines && offsets[rows[i + 7] + N] + 40 <= available; i += 8) {
                unsigned pending = unsigned(~project.batch(data, offsets.data(), rows.data() + i)) & 255;
                for (; pending; pending &= pending - 1) one(i + std::countr_zero(pending));
            }
        }
        for (; i < lines; ++i) one(i);
    }
};
#endif

inline const char* last_complete_line(const char* first, const char* stop) {
    while (stop - first >= 64) {
        stop -= 64;
        uint64_t lines = scan64(stop).lines;
        if (lines) return stop + 64 - std::countl_zero(lines);
    }
    while (stop > first) {
        if (*--stop == '\n') return stop + 1;
    }
    return first;
}
#endif

template<size_t N, class F, class Project = std::nullptr_t>
void read_rows(Chunk chunk, F&& handle, Project project = nullptr) {
    // A cut belongs to the preceding chunk through its next newline. Resolve
    // cuts on the parsing threads, so opening the file never faults in every
    // 512 KiB boundary serially. At byte zero this also skips the header.
    const char* boundary = static_cast<const char*>(memchr(chunk.begin, '\n', chunk.limit - chunk.begin));
    if (!boundary) return;
    const char* row = boundary + 1;
    const char* end = chunk.file_end;
    if (chunk.limit < end) {
        const char* nl = static_cast<const char*>(memchr(chunk.limit, '\n', end - chunk.limit));
        if (nl) end = nl + 1;
    }
#if defined(__SSE2__) || defined(__ARM_NEON)
#if defined(__AVX512BW__)
    OffsetTile<N> tile;
#else
    CsvTile<N> tile;
#endif
    while (row < end) {
        const char* stop = end;
        if (size_t(end - row) > CsvTile<N>::target) {
            const char* search = row + CsvTile<N>::target;
            const char* nl = static_cast<const char*>(memchr(search, '\n', std::min<size_t>(end - search, CsvTile<N>::slack)));
            stop = nl ? nl + 1 : last_complete_line(row, search);
            if (stop == row) { row = scalar_row<N>(row, end, handle); continue; }
        } else if (stop[-1] != '\n') {
            stop = last_complete_line(row, stop);
            if (stop == row) break;
        }
        const char* witness = row + tile.index(row, stop - row);
        tile.visit(row, handle, project);
        row = stop;
        if (witness < stop) {
            // Values cannot contain newlines. This tile ends at a newline, so
            // the first one at or after the invalid byte is the end of its row.
            row = static_cast<const char*>(memchr(witness, '\n', stop - witness)) + 1;
        }
    }
#endif
    while (row < end) row = scalar_row<N>(row, end, handle);
}

inline int pair_value(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
inline bool digits2(const char* p) { return unsigned(p[0] - '0') < 10 && unsigned(p[1] - '0') < 10; }
inline int clock_seconds(const char* p) {
    uint64_t word;
    memcpy(&word, p, 8);
    if constexpr (std::endian::native == std::endian::big) word = __builtin_bswap64(word);
    uint64_t digits = word ^ 0x30303a30303a3030ULL;  // "00:00:00" in little-endian order.
    constexpr uint64_t nibbles = 0x0f0f000f0f000f0fULL;
    // Adding 15 minus each digit's maximum sets bit 4 precisely on overflow.
    constexpr uint64_t limits = 0x060a00060a00060dULL, carries = 0x1010001010001010ULL;
    uint64_t pairs = (digits * 10 + (digits >> 8)) & 0x00ff0000ff0000ffULL;
    int hour = int(pairs & 255);
    if ((digits & ~nibbles) || ((digits + limits) & carries) || hour > 23) return -1;
    return hour * 3600 + int((pairs >> 24) & 255) * 60 + int(pairs >> 48);
}
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

// Input is eight validated ASCII digits in little-endian byte order.
inline uint32_t eight_digits(uint64_t ascii) {
    uint64_t digits = ascii - 0x3030303030303030ULL;
    digits = (digits * 10 + (digits >> 8)) & 0x00ff00ff00ff00ffULL;
    digits = (digits * 100 + (digits >> 16)) & 0x0000ffff0000ffffULL;
    return uint32_t(digits) * 10000 + uint32_t(digits >> 32);
}

// Parse the common 13-digit epoch form without a serial multiply-by-ten chain.
inline Time epoch13(const char* p) {
#if defined(__SSSE3__) && defined(__SSE4_1__)
    const auto zero = _mm_set1_epi8('0');
    // Left-pad 13 decimal digits to 16. Every load stays inside the field.
#if defined(__AVX512VL__) && defined(__AVX512BW__)
    auto raw = _mm_maskz_loadu_epi8(0x1fff, p);
    auto digits = _mm_slli_si128(_mm_sub_epi8(raw, zero), 3);
#else
    auto head = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    auto tail = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + 5));
    auto raw = _mm_unpacklo_epi64(head, tail);
    auto digits = _mm_shuffle_epi8(_mm_sub_epi8(raw, zero),
        _mm_setr_epi8(-1,-1,-1,0,1,2,3,4,8,9,10,11,12,13,14,15));
#endif
    auto invalid = _mm_subs_epu8(digits, _mm_set1_epi8(9));
    if (!_mm_testz_si128(invalid, invalid)) return invalid_time;
    auto pairs = _mm_maddubs_epi16(digits, _mm_set1_epi16(0x010a));  // [10, 1]
    auto fours = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010064));   // [100, 1]
    auto groups = _mm_packs_epi32(fours, fours);
    auto eights = _mm_madd_epi16(groups, _mm_set1_epi32(0x00012710)); // [10000, 1]
    return Time(_mm_cvtsi128_si32(eights)) * 100'000'000 + _mm_extract_epi32(eights, 1);
#else
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
    return Time(eight_digits(head)) * 100000 + (p[8] - '0') * 10000 + pair_value(p+9) * 100 + pair_value(p+11);
#endif
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
        uint64_t prefix;
        memcpy(&prefix, p, 8);
        Month m = months_[slot(p)];
        if (m.prefix != prefix || !m.days) m = month(p);
        int d, seconds_of_day;
#if defined(__SSSE3__) && defined(__SSE4_1__)
        // Gather DD hh mm ss with one shuffle. PMADDUBSW evaluates all four
        // decimal pairs at once on the scorer's Xeons. This load ends at byte 19.
        auto raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4));
        auto positions = _mm_setr_epi8(4,5,7,8,10,11,13,14, -1,-1,-1,-1,-1,-1,-1,-1);
        auto digits = _mm_shuffle_epi8(_mm_sub_epi8(raw, _mm_set1_epi8('0')), positions);
        auto pairs = _mm_maddubs_epi16(digits, _mm_set1_epi16(0x010a));
        d = _mm_extract_epi16(pairs, 0);
        auto parts = _mm_madd_epi16(pairs, _mm_setr_epi16(0,3600,60,1,0,0,0,0));
        seconds_of_day = _mm_cvtsi128_si32(parts) + _mm_extract_epi32(parts, 1);
        if constexpr (!trusted) {
            auto bad_digits = _mm_subs_epu8(digits, _mm_set1_epi8(9));
            auto limits = _mm_setr_epi16(short(m.days),23,59,59,0,0,0,0);
            auto bad = _mm_or_si128(bad_digits, _mm_cmpgt_epi16(pairs, limits));
            if (!_mm_testz_si128(bad, bad) || !d || p[10] != 'T' || p[13] != ':' || p[16] != ':') return invalid_time;
        }
#else
        d = pair_value(p+8);
        if constexpr (trusted) {
            seconds_of_day = (pair_value(p+11) * 60 + pair_value(p+14)) * 60 + pair_value(p+17);
        } else {
            seconds_of_day = clock_seconds(p+11);
            if (p[10] != 'T' || !digits2(p+8) || d < 1 || d > m.days || seconds_of_day < 0) return invalid_time;
        }
#endif
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
        return (Time(m.first) + d - 1) * day_ms + (seconds_of_day - offset * 60) * 1000LL;
    }
    Time parse(Text s) const {
        if (s.size() == 13) return epoch13(s.data());
        if ((s.size() == 20 || s.size() == 25) && s[4] == '-') return iso(s);
        const int64_t n = decimal(s, std::numeric_limits<int64_t>::max());
        return n < 0 ? invalid_time : n;
    }
};

inline int32_t utc_day(Time t) { return int32_t(t / day_ms - (t % day_ms < 0)); }

struct Window {
    friend struct EventLanes;
    friend struct ClickLanes;
    Clock clock;
    Time start, end;
    struct Arrival {
        int64_t order;
        Time midnight;
        bool on_time(Time ts, Text ingest) const {
            // Only a next-day arrival can straddle the exact 24-hour cutoff.
            if (ts >= midnight) return true;
            if (midnight - ts > day_ms) return false;
            const char* p = ingest.data();
            int seconds = (pair_value(p+11) * 60 + pair_value(p+14)) * 60 + pair_value(p+17);
            return midnight + seconds * 1000LL - ts <= day_ms;
        }
    };
private:
    struct EpochPrefix { uint64_t text; Time base; };
    std::array<EpochPrefix, 64> epoch_prefixes_{};
    static unsigned epoch_slot(const char* p) {
        return ((unsigned(p[3]) & 3) << 4) | (unsigned(p[4]) & 15);
    }
    Time event_epoch13(const char* p) const {
        uint64_t prefix = 0;
        memcpy(&prefix, p, 5);
        const auto& known = epoch_prefixes_[epoch_slot(p)];
        if (!known.text || known.text != prefix) return epoch13(p);
        uint64_t digits;
        memcpy(&digits, p + 5, 8);
        if constexpr (std::endian::native == std::endian::big) digits = __builtin_bswap64(digits);
        constexpr uint64_t zero = 0x3030303030303030ULL;
        if (((digits - zero) | (digits + 0x4646464646464646ULL)) & 0x8080808080808080ULL) return invalid_time;
        return known.base + eight_digits(digits);
    }
    struct IngestDay { uint64_t month_prefix; Time midnight; int64_t order_base; };
    std::array<IngestDay, 64> ingest_days_{};
    int32_t first_day_;

    // Ingest is valid UTC F1. The packed decimal HHMMSS is ordered and fits in
    // 22 bits. Bias the day so dates near the window fit in the record's 31 bits.
    int64_t order_base(int32_t day) const {
        return (int64_t(day) - first_day_ + 256) * (1 << 22);
    }
    static unsigned day_slot(const char* date) {
        return ((date[8] & 3) << 4) | (date[9] & 15);
    }
    static uint32_t clock_digits(const char* date) {
        uint64_t word;
        memcpy(&word, date + 11, 8);  // HH:MM:SS
        if constexpr (std::endian::native == std::endian::little) word = __builtin_bswap64(word);
        constexpr uint64_t digit_mask = 0x0f0f000f0f000f0fULL;
#if defined(__BMI2__) && defined(__x86_64__)
        return uint32_t(_pext_u64(word, digit_mask));
#else
        uint64_t digits = word & digit_mask;
        uint64_t pairs = (digits | (digits >> 4)) & 0x00ff0000ff0000ffULL;
        return uint32_t(pairs | (pairs >> 16) | (pairs >> 32)) & 0xffffff;
#endif
    }
public:
    Time event_time(Text event, Text ingest, Time midnight) const {
        if (event.size() == 13) return event_epoch13(event.data());
        if (event.size() == 20 && memcmp(event.data(), ingest.data(), 11) == 0) {
            int seconds = clock_seconds(event.data() + 11);
            return seconds >= 0 && event[19] == 'Z' ? midnight + Time(seconds) * 1000 : invalid_time;
        }
        return clock.parse(event);
    }
    Arrival arrival(Text ingest) const {
        const char* p = ingest.data();
        uint64_t month;
        memcpy(&month, p, 8);
        const auto& cached = ingest_days_[day_slot(p)];
        if (cached.month_prefix == month) return {cached.order_base + clock_digits(p), cached.midnight};
        int32_t day = utc_day(clock.iso<true>(ingest));
        return {order_base(day) + clock_digits(p), Time(day) * day_ms};
    }
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
        using namespace std::chrono;
        // Include the seven-day click lookback when warming the epoch prefix cache.
        constexpr Time unit = 100'000'000;
        Time low = std::max<Time>(start - week_ms, 0) / unit;
        Time high = std::min<Time>(99'999, (end - 1) / unit);
        for (Time n = low; n <= std::min(high, low + 31); ++n) {
            char digits[6];
            snprintf(digits, sizeof digits, "%05d", int(n));
            auto& cached = epoch_prefixes_[epoch_slot(digits)];
            memcpy(&cached.text, digits, 5);
            cached.base = n * unit;
        }
        first_day_ = utc_day(start);
        // Cache near-window dates. arrival() checks the month before using a slot.
        int32_t last = std::min(utc_day(end - 1) + 6, first_day_ + 28);
        for (int32_t day = first_day_ - 8; day <= last; ++day) {
            year_month_day d{sys_days{days(day)}};
            int y = int(d.year());
            if (y < 0 || y > 9999) continue;
            char date[16];
            snprintf(date, sizeof date, "%04d-%02u-%02u", y, unsigned(d.month()), unsigned(d.day()));
            auto& cached = ingest_days_[day_slot(date)];
            memcpy(&cached.month_prefix, date, 8);
            cached.midnight = Time(day) * day_ms;
            cached.order_base = order_base(day);
        }
    }
};
} // namespace shop
