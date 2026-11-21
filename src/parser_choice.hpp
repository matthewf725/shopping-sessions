#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <limits>

namespace shop {

// Time disjoint input chunks. Every sample computes its actual output exactly once.
class ParserChoice {
    static constexpr unsigned modes = 3;
    static constexpr unsigned per_mode = 12;
    static constexpr unsigned sample_count = modes * per_mode;
    std::atomic<unsigned> issued_{0}, completed_{0}, best_{modes};
    std::array<uint64_t, sample_count> rates_{};

    static uint64_t cpu_time() {
        timespec ts{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return 0;
        return uint64_t(ts.tv_sec) * 1000000000 + uint64_t(ts.tv_nsec);
    }
    static unsigned method(unsigned i) { return (i + i / modes) % modes; }
    void choose() {
        std::array<std::array<uint64_t, per_mode>, modes> rates;
        for (unsigned i = 0; i < sample_count; ++i)
            rates[method(i)][i / modes] = rates_[i];
        unsigned selected = 0;
        for (unsigned k = 0; k < modes; ++k) {
            auto& values = rates[k];
            std::nth_element(values.begin(), values.begin() + per_mode / 2, values.end());
            if (values[per_mode / 2] < rates[selected][per_mode / 2]) selected = k;
        }
        best_.store(selected, std::memory_order_relaxed);
    }
public:
    unsigned selected() const { return best_.load(std::memory_order_relaxed); }

    template<class Parse> void run(size_t bytes, Parse parse) {
        unsigned mode = selected();
        if (mode < modes || bytes < 32 * 1024) {
            parse(mode < modes ? mode : 0);
            return;
        }
        const unsigned ticket = issued_.fetch_add(1, std::memory_order_relaxed);
        if (ticket >= sample_count + modes) {
            parse(0);
            return;
        }
        // Give each implementation one untimed call before comparing its samples.
        if (ticket < modes) {
            parse(ticket);
            return;
        }
        const unsigned sample = ticket - modes;
        uint64_t start = cpu_time();
        parse(method(sample));
        uint64_t finish = cpu_time();
        rates_[sample] = start && finish >= start
            ? (finish - start) * (1024 * 1024) / bytes : std::numeric_limits<uint64_t>::max();
        // The final worker reads all sample slots after the release sequence.
        if (completed_.fetch_add(1, std::memory_order_acq_rel) + 1 == sample_count)
            choose();
    }
};
} // namespace shop
