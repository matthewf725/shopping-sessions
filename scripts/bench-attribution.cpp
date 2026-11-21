#include <map>
#define SHOP_BENCH_ATTRIBUTION
#include "sessions.hpp"
#include <chrono>
#include <iostream>
#include <random>

namespace shop {
enum class Method { rank, sweep, folded };
const char *method_name(Method m) {
    switch (m) {
    case Method::rank:
        return "week-rank";
    case Method::sweep:
        return "forward-sweep";
    case Method::folded:
        return "folded-bins";
    }
    std::abort();
}
struct AttributionTrial {
    Shopper &shopper;
    explicit AttributionTrial(Shopper &source) : shopper(source) {}
    struct SweepRange {
        size_t first = SIZE_MAX;
        size_t stop = 0;
    };
    std::vector<Click> ordered_clicks_, radix_clicks_;
    std::vector<SweepRange> campaign_ranges_;
    std::vector<uint32_t> active_campaigns_;

    void order_clicks(const Segment *chain) {
        ordered_clicks_.clear();
        uint64_t varying = 0;
        bool ordered = true;
        const Time earliest = shopper.sessions_.front().start - week_ms;
        const Time latest = shopper.sessions_.back().start;
        for (auto seg = chain; seg; seg = seg->next) {
            for (uint32_t i = 0; i < seg->m; ++i) {
                const auto &c = seg->clicks[i];
                if (c.ts < earliest || c.ts > latest)
                    continue;
                if (!ordered_clicks_.empty()) {
                    ordered &= ordered_clicks_.back().ts <= c.ts;
                    varying |= uint64_t(c.ts) ^ uint64_t(ordered_clicks_.front().ts);
                }
                ordered_clicks_.push_back(c);
            }
        }
        if (ordered)
            return;
        if (ordered_clicks_.size() <= 32) {
            // The small-set path has a fixed upper bound.
            for (size_t i = 1; i < ordered_clicks_.size(); ++i) {
                Click c = ordered_clicks_[i];
                size_t j = i;
                while (j && ordered_clicks_[j - 1].ts > c.ts) {
                    ordered_clicks_[j] = ordered_clicks_[j - 1];
                    --j;
                }
                ordered_clicks_[j] = c;
            }
            return;
        }
        radix_clicks_.resize(ordered_clicks_.size());
        for (unsigned shift = 0; shift < 64; shift += 8) {
            if (!(varying & (uint64_t{255} << shift)))
                continue;
            auto digit = [shift](Time t) {
                return uint8_t((uint64_t(t) ^ (uint64_t{1} << 63)) >> shift);
            };
            std::array<size_t, 256> counts{};
            for (const auto &c : ordered_clicks_)
                ++counts[digit(c.ts)];
            size_t offset = 0;
            for (auto &n : counts) {
                size_t next = offset + n;
                n = offset;
                offset = next;
            }
            for (const auto &c : ordered_clicks_)
                radix_clicks_[counts[digit(c.ts)]++] = c;
            ordered_clicks_.swap(radix_clicks_);
        }
    }

    void sweep_assists(const Segment *chain, CampaignDays &campaigns) {
        order_clicks(chain);
        if (campaign_ranges_.empty())
            campaign_ranges_.resize(shopper.campaign_count_);
        auto flush = [&](uint32_t campaign, const SweepRange &range) {
            const auto *day = shopper.session_days_.data() + shopper.sessions_[range.first].day_index;
            campaigns.add_assists(campaign, day, range.first, range.stop);
        };
        size_t first = 0, stop = 0;
        for (const auto &c : ordered_clicks_) {
            // Both endpoints advance monotonically for equal-length closed windows.
            while (first < shopper.sessions_.size() && shopper.sessions_[first].start < c.ts)
                ++first;
            stop = std::max(stop, first);
            while (stop < shopper.sessions_.size() && shopper.sessions_[stop].start - c.ts <= week_ms)
                ++stop;
            if (first == stop)
                continue;
            auto &winner = shopper.sessions_[first].credit;
            if (c.ts > winner.ts || (c.ts == winner.ts && shopper.click_keys_.less(c.id, winner.id)))
                winner = c;
            auto &range = campaign_ranges_[c.campaign];
            if (range.first == SIZE_MAX) {
                active_campaigns_.push_back(c.campaign);
                range.first = first;
            } else if (range.stop < first) {
                flush(c.campaign, range);
                range.first = first;
            }
            range.stop = stop;
        }
        for (uint32_t campaign : active_campaigns_) {
            flush(campaign, campaign_ranges_[campaign]);
            campaign_ranges_[campaign] = {};
        }
        active_campaigns_.clear();
        shopper.credit_sessions(campaigns);
    }

    void replay(Method method, const Segment *seg, CampaignDays &totals) {
        for (auto &s : shopper.sessions_)
            s.credit = {invalid_time, 0, 0};
        switch (method) {
        case Method::rank:
            shopper.rank_assists(seg, totals);
            break;
        case Method::sweep:
            sweep_assists(seg, totals);
            break;
        case Method::folded:
            shopper.fold_clicks(seg, totals);
            break;
        }
    }
};
} // namespace shop
int main(int argc, char **argv) {
    using namespace shop;
    if (argc != 2) {
        std::cerr << "Usage: bench-attribution /path/to/practice-input\n";
        return 2;
    }
    fs::path data = argv[1];
    Window w(data / "manifest.json");
    Dimensions d(data);
    if (d.names.size() <= 1 || w.end - w.start < 7 * day_ms) {
        std::cerr << "The replay needs a window of at least seven days and a registered campaign.\n";
        return 2;
    }
    Keys ids('k');
    const std::array methods{Method::rank, Method::sweep, Method::folded};
    std::cout << "sessions,clicks,ordered,method,round,nanoseconds_per_shopper\n";
    for (auto [ns, nc] : std::vector<std::pair<int, int>>{
             {3, 4}, {10, 12}, {30, 120}, {120, 800}, {300, 8192}, {300, 32768}}) {
        for (int sorted = 0; sorted <= 1; ++sorted) {
            std::mt19937_64 rng(73 + ns);
            std::vector<Event> events;
            Time step = (w.end - w.start - 2000) / ns;
            if (step <= gap)
                return 3;
            for (int i = 0; i < ns; ++i)
                events.push_back({w.start + 1000 + i * step, uint32_t(i % 3 == 0 ? 11 : 0), 1});
            std::vector<Click> clicks;
            auto id = [&](int i) { return ids.encode("k" + std::to_string(i)); };
            unsigned campaigns = unsigned(std::min<size_t>(nc > 500 ? 64 : 4, d.names.size() - 1));
            for (int i = 0; i < nc; ++i) {
                Time t = w.start - week_ms + Time(rng() % uint64_t(w.end - w.start + week_ms));
                if (i == 0)
                    t = events[0].ts - week_ms;
                if (i == 1 || i == 2)
                    t = events[ns / 2].ts;
                if (i == 3)
                    t = events.back().ts + 1;
                clicks.push_back({t, id(i + 1), uint32_t(1 + rng() % campaigns)});
            }
            if (sorted)
                std::stable_sort(clicks.begin(), clicks.end(),
                                 [](auto a, auto b) { return a.ts < b.ts; });
            Segment seg{events.data(), clicks.data(), uint32_t(ns), uint32_t(nc), 17};
            Shopper shopper(w, d, ids);
            std::vector<UserDay> users;
            CampaignDays setup(w, d.names.size());
            shopper.run(17, &seg, users, setup);
            AttributionTrial trial(shopper);
            std::map<std::pair<uint32_t, int32_t>, Totals> ref;
            for (const auto &event : events) {
                Click best{invalid_time, 0, 0};
                std::vector<bool> present(campaigns + 1);
                for (const auto &c : clicks)
                    if (c.ts >= event.ts - week_ms && c.ts <= event.ts) {
                        present[c.campaign] = true;
                        if (c.ts > best.ts ||
                            (c.ts == best.ts && ids.decode(c.id) < ids.decode(best.id)))
                            best = c;
                    }
                int day = utc_day(event.ts);
                auto &tot = ref[{best.campaign, day}];
                ++tot.sessions;
                tot.purchasing += (event.price > 0);
                tot.revenue += event.price;
                for (uint32_t i = 1; i <= campaigns; ++i)
                    if (present[i])
                        ++ref[{i, day}].assists;
            }
            for (auto method : methods) {
                CampaignDays checked(w, d.names.size());
                trial.replay(method, &seg, checked);
                auto remaining = ref;
                bool match = true;
                checked.each([&](uint32_t campaign, int32_t day, const Totals &row) {
                    auto it = remaining.find({campaign, day});
                    if (it == remaining.end()) {
                        match = false;
                        return;
                    }
                    const auto &x = it->second;
                    if (std::tie(x.sessions, x.purchasing, x.revenue, x.assists) !=
                        std::tie(row.sessions, row.purchasing, row.revenue, row.assists))
                        match = false;
                    remaining.erase(it);
                });
                if (!match || !remaining.empty()) {
                    std::cerr << "mismatch " << ns << " " << nc << " " << sorted << "\n";
                    return 4;
                }
            }
            int repeats = std::max(10, 300000 / (nc + ns));
            for (unsigned round = 0; round < 6; ++round) {
                for (unsigned slot = 0; slot < methods.size(); ++slot) {
                    auto method = methods[(round + slot) % methods.size()];
                    CampaignDays sums(w, d.names.size());
                    auto start = std::chrono::steady_clock::now();
                    for (int i = 0; i < repeats; ++i)
                        trial.replay(method, &seg, sums);
                    auto end = std::chrono::steady_clock::now();
                    uint64_t served = 0;
                    sums.each([&](uint32_t, int32_t, const Totals &t) { served += t.sessions; });
                    if (served != uint64_t(repeats) * ns)
                        return 5;
                    if (round) {
                        auto elapsed =
                            std::chrono::duration<double, std::nano>(end - start).count() / repeats;
                        std::cout << ns << ',' << nc << ',' << sorted << ',' << method_name(method)
                                  << ',' << round << ',' << elapsed << '\n';
                    }
                }
            }
        }
    }
}
