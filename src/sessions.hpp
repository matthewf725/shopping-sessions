#pragma once
#include "keys.hpp"

namespace shop {
constexpr Time gap = 30 * 60 * 1000;
struct Event { Time ts; uint32_t price, category; };
struct Click { Time ts; Key id; uint32_t campaign; };
struct UserDay { Key person; int32_t day, sessions, events; uint32_t top; int64_t revenue; };
struct Totals { int64_t sessions{}, purchasing{}, revenue{}, assists{}; };

class CampaignDays {
    int32_t first_;
    size_t campaigns_;
    std::vector<Totals> dense_;
    std::unordered_map<uint64_t, Totals> sparse_;
public:
    CampaignDays(const Window& w, size_t campaigns) : first_(utc_day(w.start)), campaigns_(campaigns) {
        size_t days = utc_day(w.end - 1) - first_ + 1;
        if (days <= 1'000'000 / campaigns) dense_.resize(days * campaigns);
    }
    Totals& at(uint32_t campaign, int32_t day) {
        uint64_t cell = uint64_t(day - first_) * campaigns_ + campaign;
        return dense_.empty() ? sparse_[cell] : dense_[cell];
    }
    template<class F> void each(F fn) const {
        auto emit = [&](uint64_t i, const Totals& t) {
            if (t.sessions || t.assists) fn(uint32_t(i % campaigns_), int32_t(i / campaigns_) + first_, t);
        };
        for (size_t i = 0; i < dense_.size(); ++i) emit(i, dense_[i]);
        for (auto& [i, t] : sparse_) emit(i, t);
    }
};

inline uint64_t below_bit(unsigned bit) { return (uint64_t(1) << bit) - 1; }
inline uint64_t through_bit(unsigned bit) { return ~uint64_t(0) >> (63 - bit); }

// For very wide manifests, order only occupied cell numbers with four byte passes.
inline void radix_cells(std::vector<uint32_t>& cells, std::vector<uint32_t>& scratch) {
    if (cells.size() < 2) return;
    scratch.resize(cells.size());
    for (int shift = 0; shift < 32; shift += 8) {
        std::array<size_t, 256> counts{};
        for (auto n : cells) ++counts[(n >> shift) & 255];
        size_t pos = 0;
        for (auto& n : counts) { size_t next = pos + n; n = pos; pos = next; }
        for (auto n : cells) scratch[counts[(n >> shift) & 255]++] = n;
        cells.swap(scratch);
    }
}

class Shopper {
    struct Bucket { Time first, last; uint32_t events, purchases; };
    struct Purchase { uint32_t price, category, next; };
    struct Session { Time start; int64_t revenue; Click credit; int32_t day; };
    static constexpr uint32_t none = UINT32_MAX;
    Time origin_;
    const Keys& click_keys_;
    // These workspaces are reused across people. Clear only touched cells, categories and campaigns.
    std::vector<Bucket> bins_;
    std::vector<uint64_t> occupied_, start_bits_;
    std::vector<uint32_t> start_rank_;
    std::unordered_map<uint32_t, Bucket> sparse_bins_;
    std::vector<uint32_t> order_, radix_scratch_;
    std::vector<Purchase> purchases_;
    std::vector<Session> sessions_;
    std::vector<int64_t> category_sums_;
    std::vector<uint32_t> categories_used_;
    std::vector<uint64_t> coverage_;
    std::vector<size_t> touched_;
    std::unordered_map<uint64_t, uint64_t> sparse_coverage_;
    size_t campaign_count_;
    size_t first_word_ = 0, last_word_ = 0;
    bool indexed_ = false, dense_coverage_ = true;

    uint32_t cell(Time t) const { return uint32_t((t - origin_) / gap); }
    void clear_categories() {
        for (uint32_t c : categories_used_) category_sums_[c] = 0;
        categories_used_.clear();
    }
    void collect(const Event* events, size_t count) {
        first_word_ = occupied_.size();
        last_word_ = 0;
        for (size_t i = 0; i < count; ++i) {
            const Event& e = events[i];
            uint32_t pos = cell(e.ts);
            Bucket* b;
            if (!bins_.empty()) {
                size_t w = pos >> 6;
                uint64_t mask = uint64_t(1) << (pos & 63);
                b = &bins_[pos];
                if (!(occupied_[w] & mask)) {
                    occupied_[w] |= mask;
                    first_word_ = std::min(first_word_, w);
                    last_word_ = std::max(last_word_, w);
                    *b = {e.ts, e.ts, 0, none};
                }
            } else {
                auto [it, added] = sparse_bins_.try_emplace(pos, Bucket{e.ts, e.ts, 0, none});
                if (added) order_.push_back(pos);
                b = &it->second;
            }
            b->first = std::min(b->first, e.ts);
            b->last = std::max(b->last, e.ts);
            ++b->events;
            if (e.price) {
                purchases_.push_back({e.price, e.category, b->purchases});
                b->purchases = uint32_t(purchases_.size() - 1);
            }
        }
    }
    void form_sessions(Key person, std::vector<UserDay>& users) {
        Time last = invalid_time;
        auto take = [&](uint32_t pos, const Bucket& b) {
            if (last == invalid_time || b.first - last > gap) {
                int32_t day = utc_day(b.first);
                if (sessions_.empty() || sessions_.back().day != day) {
                    clear_categories();
                    users.push_back({person, day, 0, 0, 0, 0});
                }
                ++users.back().sessions;
                sessions_.push_back({b.first, 0, {invalid_time, 0, 0}, day});
                if (!bins_.empty()) start_bits_[pos >> 6] |= uint64_t(1) << (pos & 63);
            }
            last = b.last;
            auto& daily = users.back();
            daily.events += b.events;
            for (uint32_t i = b.purchases; i != none; i = purchases_[i].next) {
                const auto& p = purchases_[i];
                auto& sum = category_sums_[p.category];
                if (sum == 0) categories_used_.push_back(p.category);
                sum += p.price;
                // Category numbers are assigned in byte-wise name order.
                if (sum > category_sums_[daily.top] || (sum == category_sums_[daily.top] && p.category < daily.top)) daily.top = p.category;
                daily.revenue += p.price;
                sessions_.back().revenue += p.price;
            }
        };
        if (bins_.empty()) {
            radix_cells(order_, radix_scratch_);
            for (auto pos : order_) take(pos, sparse_bins_.at(pos));
            order_.clear();
            sparse_bins_.clear();
        } else {
            uint32_t starts = 0;
            for (size_t w = first_word_; w <= last_word_; ++w) {
                start_bits_[w] = 0;
                start_rank_[w] = starts;
                for (uint64_t mask = occupied_[w]; mask; mask &= mask - 1) {
                    uint32_t pos = uint32_t(64 * w + std::countr_zero(mask));
                    take(pos, bins_[pos]);
                }
                starts += std::popcount(start_bits_[w]);
                occupied_[w] = 0;
            }
        }
        indexed_ = !bins_.empty();
        purchases_.clear();
    }
    // Number of session starts < t (or <= t). At most one start can occupy any gap-width cell.
    size_t rank(Time t, bool inclusive) const {
        if (t < sessions_.front().start || (!inclusive && t == sessions_.front().start)) return 0;
        if (t > sessions_.back().start || (inclusive && t == sessions_.back().start)) return sessions_.size();
        if (!indexed_) {
            auto it = std::lower_bound(sessions_.begin(), sessions_.end(), t, [inclusive](const Session& s, Time v) {
                return inclusive ? s.start <= v : s.start < v;
            });
            return it - sessions_.begin();
        }
        uint32_t pos = cell(t);
        uint64_t word = start_bits_[pos >> 6];
        uint64_t bit = uint64_t(1) << (pos & 63);
        size_t n = start_rank_[pos >> 6] + std::popcount(word & (bit - 1));
        if ((word & bit) && (inclusive ? sessions_[n].start <= t : sessions_[n].start < t)) ++n;
        return n;
    }
    void cover(uint32_t campaign, size_t first, size_t stop) {
        size_t a = first >> 6, b = (stop - 1) >> 6;
        for (size_t w = a; w <= b; ++w) {
            uint64_t bits = ~uint64_t(0);
            if (w == a) bits &= ~below_bit(first & 63);
            if (w == b) bits &= through_bit((stop - 1) & 63);
            size_t at = w * campaign_count_ + campaign;
            if (dense_coverage_) {
                if (!coverage_[at]) touched_.push_back(at);
                coverage_[at] |= bits;
            } else sparse_coverage_[at] |= bits;
        }
    }
    void attribute(const Click* clicks, size_t count, CampaignDays& campaigns) {
        size_t words = (sessions_.size() + 63) / 64;
        dense_coverage_ = words <= 1'000'000 / campaign_count_;
        if (dense_coverage_ && coverage_.size() < words * campaign_count_) coverage_.resize(words * campaign_count_);
        for (size_t i = 0; i < count; ++i) {
            const Click& c = clicks[i];
            size_t first = rank(c.ts, false);
            if (first == sessions_.size() || sessions_[first].start - c.ts > week_ms) continue;
            auto& best = sessions_[first].credit;
            if (c.ts > best.ts || (c.ts == best.ts && click_keys_.less(c.id, best.id))) best = c;
            cover(c.campaign, first, rank(c.ts + week_ms, true));
        }
        auto assists = [&](size_t at, uint64_t mask) {
            size_t base = at / campaign_count_ * 64;
            uint32_t campaign = at % campaign_count_;
            while (mask) {
                ++campaigns.at(campaign, sessions_[base + std::countr_zero(mask)].day).assists;
                mask &= mask - 1;
            }
        };
        for (size_t at : touched_) { assists(at, coverage_[at]); coverage_[at] = 0; }
        touched_.clear();
        for (auto [at, bits] : sparse_coverage_) assists(at, bits);
        sparse_coverage_.clear();
        Click latest{invalid_time, 0, 0};
        for (const auto& s : sessions_) {
            if (s.credit.id) latest = s.credit;
            uint32_t credited = latest.ts >= s.start - week_ms ? latest.campaign : 0;
            auto& t = campaigns.at(credited, s.day);
            ++t.sessions;
            t.purchasing += (s.revenue > 0);
            t.revenue += s.revenue;
        }
    }
public:
    Shopper(const Window& w, const Dimensions& d, const Keys& click_keys)
        : origin_(w.start), click_keys_(click_keys), category_sums_(d.categories.size()+1), campaign_count_(d.names.size()) {
        size_t n = size_t((w.end - w.start - 1) / gap + 1);
        if (n <= 65536) {
            bins_.resize(n);
            occupied_.resize((n+63)/64);
            start_bits_.resize(occupied_.size());
            start_rank_.resize(occupied_.size());
        }
    }
    void run(Key person, const Event* events, size_t n, const Click* clicks, size_t m,
             std::vector<UserDay>& users, CampaignDays& campaigns) {
        if (!n) return;
        sessions_.clear();
        collect(events, n);
        form_sessions(person, users);
        attribute(clicks, m, campaigns);
    }
};
} // namespace shop
