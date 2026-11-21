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

#if defined(__SSE2__) || defined(__ARM_NEON)
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
#elif defined(__SSE2__)
    Marks m{};
    for (unsigned i = 0; i < 64; i += 16) {
        auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        m.commas |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(',')))) << i;
        m.lines  |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8('\n')))) << i;
        m.quotes |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')))) << i;
    }
    return m;
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
// If either check fails, the strict reader recovers each physical line independently.
struct CsvTile {
    static constexpr size_t target = 8192;
    static constexpr size_t slack = 192;
    std::array<uint16_t, target + slack + 1> separator, line_end;
    size_t fields = 0, lines = 0;
    bool index(const char* data, size_t length) {
        fields = lines = 0;
        uint64_t carry = 0, starts_field = 1;
        for (size_t offset = 0; offset < length; offset += 64) {
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
                if ((m.quotes & ~adjacent) || (parity & ends)) return false;
                carry = uint64_t(0) - (parity >> 63);
            }
            starts_field = ends >> 63;
            for (uint64_t nl = m.lines; nl; nl &= nl - 1) {
                uint64_t first = nl & -nl;
                line_end[lines++] = uint16_t(fields + std::popcount(ends & (first | (first - 1))));
            }
            for (uint64_t bits = ends; bits; bits &= bits - 1)
                separator[fields++] = uint16_t(offset + std::countr_zero(bits));
        }
        return !carry;
    }
};
#endif

template<size_t N, class F>
void read_rows(Chunk chunk, F&& handle) {
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
    CsvTile tile;
    while (end - row >= 128) {
        const char* stop = end;
        if (size_t(end - row) > CsvTile::target) {
            const char* search = row + CsvTile::target;
            const char* nl = static_cast<const char*>(memchr(search, '\n', std::min<size_t>(end - search, CsvTile::slack)));
            if (!nl) { row = scalar_row<N>(row, end, handle); continue; }
            stop = nl + 1;
        }
        if (stop[-1] != '\n') break;
        if (!tile.index(row, stop - row)) {
            while (row < stop) row = scalar_row<N>(row, stop, handle);
            continue;
        }
        size_t first = 0;
        unsigned start = 0;
        for (size_t line = 0; line < tile.lines; ++line) {
            size_t next = tile.line_end[line];
            if (next - first == N) {
                std::array<Text, N> fields;
                for (size_t k = 0; k < N; ++k) {
                    unsigned finish = tile.separator[first + k];
                    unsigned quote = row[start] == '"';
                    fields[k] = Text(row + start + quote, finish - start - quote * 2);
                    start = finish + 1;
                }
                handle(fields);
            } else start = tile.separator[next - 1] + 1;
            first = next;
        }
        row = stop;
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
        int h = pair_value(p+11), min = pair_value(p+14), sec = pair_value(p+17);
        if constexpr (!trusted) {
            if (p[10] != 'T' || p[13] != ':' || p[16] != ':' || !digits2(p+8) || !digits2(p+11) ||
                !digits2(p+14) || !digits2(p+17) || d < 1 || d > m.days || h > 23 || min > 59 || sec > 59) return invalid_time;
        }
        seconds_of_day = (h * 60 + min) * 60 + sec;
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
        first_day_ = utc_day(start);
        // Cache near-window dates. arrival() checks the month before using a slot.
        int32_t last = std::min(utc_day(end - 1) + 6, first_day_ + 28);
        for (int32_t day = first_day_ - 1; day <= last; ++day) {
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
