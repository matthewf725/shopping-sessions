// Index exceptions to the nine-byte ID and same-UTC-date prediction.
#include "input.hpp"
#include <iostream>

namespace {
using namespace shop;

struct ShapeIndex {
    struct Word { uint64_t before, mask; };
    struct Location { uint64_t row, byte; size_t file; };
    std::vector<fs::path> files;
    std::vector<Word> words;
    std::vector<Location> locations;
    uint64_t rows = 0, exceptions = 0;

    ShapeIndex(const fs::path& data, bool keep_locations) {
        Window window(data / "manifest.json");
        for (const auto& f : fs::directory_iterator(data / "events"))
            if (f.is_regular_file() && f.path().extension() == ".csv") files.push_back(f.path());
        std::sort(files.begin(), files.end());
        for (size_t i = 0; i < files.size(); ++i) {
            MappedCsv mapped(files[i]);
            std::vector<Chunk> chunks;
            mapped.chunks(chunks);
            if (chunks.empty()) continue;
            const char* begin = chunks.front().begin;
            const char* end = chunks.front().file_end;
            const char* header_end = static_cast<const char*>(memchr(begin, '\n', end - begin));
            if (!header_end) continue;
            const char* row = header_end + 1;
            while (row < end) {
                bool matches = false;
                auto classify = [&](const auto& f) {
                    if (f[0].size() != 9 || f[0].front() != 'e' ||
                        f[1].size() != 9 || f[1].front() != 'u' || f[3].size() != 20) return;
                    Time ts = window.clock.parse(f[2]);
                    if (ts < window.start || ts >= window.end) return;
                    Time ingest = window.clock.iso(f[3]);
                    matches = ingest != invalid_time && utc_day(ts) == utc_day(ingest);
                };
                const char* next = scalar_row<7>(row, end, classify);
                unsigned bit = rows & 63;
                if (bit == 0) words.push_back({exceptions, 0});
                if (!matches) {
                    words.back().mask |= uint64_t(1) << bit;
                    ++exceptions;
                    if (keep_locations) locations.push_back({rows, uint64_t(row - begin), i});
                }
                ++rows;
                row = next;
            }
        }
        words.push_back({exceptions, 0});  // Prefix after the last word, also for an empty file set.
    }

    uint64_t rank(uint64_t row) const {
        auto [before, mask] = words[row >> 6];
        return before + std::popcount(mask & ((uint64_t(1) << (row & 63)) - 1));
    }

    uint64_t select(uint64_t first, uint64_t stop, uint64_t k, uint64_t before_range, uint64_t count) const {
        uint64_t ordinal = before_range + k;
        uint64_t earliest = first + k, matches = stop - first - count;
        // Predict the intervening matches from the range's exact density. Under any
        // shuffle the answer is between earliest and earliest + matches, inclusive.
        uint64_t guess = earliest + uint64_t((__uint128_t(k + 1) * matches) / (__uint128_t(count) + 1));
        size_t lo = guess >> 6, hi = lo;
        size_t low_word = earliest >> 6, high_word = (earliest + matches) >> 6;
        for (size_t step = 1; words[lo].before > ordinal; step *= 2) {
            hi = lo - 1;
            lo -= std::min(step, lo - low_word);
        }
        for (size_t step = 1; words[hi + 1].before <= ordinal; step *= 2) {
            lo = hi + 1;
            hi += std::min(step, high_word - hi);
        }
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (words[mid + 1].before <= ordinal) lo = mid + 1;
            else hi = mid;
        }
        auto [before, mask] = words[lo];
        return lo * 64 + select_bit(mask, unsigned(ordinal - before));
    }

    void query(uint64_t first, uint64_t stop, Text mode, uint64_t k) const {
        if (first > stop || stop > rows) throw std::runtime_error("range outside the event rows");
        uint64_t a = rank(first), b = rank(stop);
        if (mode == "--select") {
            if (k >= b - a) std::cout << "none\n";
            else std::cout << select(first, stop, k, a, b - a) << '\n';
        } else if (mode == "--list") {
            std::cout << "range " << first << ' ' << stop << '\n';
            for (uint64_t i = a; i < b; ++i) {
                const auto& e = locations[i];
                std::cout << e.row << '\t' << files[e.file].filename() << '\t' << e.byte << '\n';
            }
        } else std::cout << b - a << '\n';
    }
};
}

int main(int argc, char** argv) {
    try {
        shop::Text mode = argc == 3 ? argv[2] : "--count";
        if (argc < 2 || argc > 3 || (mode != "--count" && mode != "--select" && mode != "--list"))
            throw std::runtime_error("usage: shape-index DATA [--count|--select|--list] < queries");
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);
        ShapeIndex index(argv[1], mode == "--list");
        std::cerr << index.rows << " physical event rows, " << index.exceptions << " exceptions\n";
        uint64_t first, stop;
        while (std::cin >> first) {
            if (!(std::cin >> stop)) throw std::runtime_error("expected an exclusive end row");
            uint64_t k = 0;
            if (mode == "--select" && !(std::cin >> k)) throw std::runtime_error("expected a zero-based exception ordinal");
            index.query(first, stop, mode, k);
        }
        if (!std::cin.eof()) throw std::runtime_error("expected a start row");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
