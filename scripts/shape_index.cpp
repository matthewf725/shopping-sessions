// Index exceptions to the nine-byte ID and same-UTC-date prediction.
#include "input.hpp"
#include <iostream>

namespace {
using namespace shop;

struct Exception {
    uint64_t row, byte;
    size_t file;
};

struct ShapeIndex {
    std::vector<fs::path> files;
    std::vector<Exception> exceptions;
    uint64_t rows = 0;

    explicit ShapeIndex(const fs::path& data) {
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
                    matches = ts >= window.start && ts < window.end &&
                              utc_day(ts) == utc_day(window.clock.iso<true>(f[3]));
                };
                const char* next = scalar_row<7>(row, end, classify);
                if (!matches) exceptions.push_back({rows, uint64_t(row - begin), i});
                ++rows;
                row = next;
            }
        }
    }

    void report(uint64_t first, uint64_t stop) const {
        if (first > stop || stop > rows) throw std::runtime_error("range outside the event rows");
        auto it = std::lower_bound(exceptions.begin(), exceptions.end(), first,
                                   [](const Exception& e, uint64_t key) { return e.row < key; });
        std::cout << "range " << first << ' ' << stop << '\n';
        for (; it != exceptions.end() && it->row < stop; ++it)
            std::cout << it->row << '\t' << files[it->file].filename() << '\t' << it->byte << '\n';
    }
};
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: shape-index DATA < ranges");
        ShapeIndex index(argv[1]);
        std::cerr << index.rows << " physical event rows, " << index.exceptions.size() << " exceptions\n";
        uint64_t first, stop;
        while (std::cin >> first) {
            if (!(std::cin >> stop)) throw std::runtime_error("expected an exclusive end row");
            index.report(first, stop);
        }
        if (!std::cin.eof()) throw std::runtime_error("expected a start row");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
