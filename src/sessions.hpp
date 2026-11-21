#pragma once
#include "keys.hpp"

namespace shop {
constexpr Time gap = 30 * 60 * 1000;
struct Event { Time ts; uint32_t price, category; };
struct Click { Time ts; Key id; uint32_t campaign; };
// A range of observations for one raw user. Linked aliases share a chain.
struct Segment {
    const Event* events;
    const Click* clicks;
    uint32_t n, m;
    Key name;
    Segment* next = nullptr;
};
struct UserDay { Key person; int32_t day, sessions, events; uint32_t top; int64_t revenue; };
struct Credits { int64_t sessions{}, purchasing{}, revenue{}; };
struct Totals : Credits { int64_t assists{}; };
struct SessionDay { int32_t day; uint32_t stop; }; // Exclusive session rank for this UTC day.

class CampaignDays {
    int32_t first_;
    size_t campaigns_, days_;
    std::vector<Credits> credits_;   // All campaigns for a given day.
    std::vector<int64_t> assists_;   // All days for a given campaign.
    std::unordered_map<uint64_t, Totals> sparse_;

    uint64_t cell(uint32_t campaign, int32_t day) const {
        return uint64_t(day - first_) * campaigns_ + campaign;
    }
public:
    CampaignDays(const Window& w, size_t campaigns)
        : first_(utc_day(w.start)), campaigns_(campaigns), days_(utc_day(w.end - 1) - first_ + 1) {
        if (days_ <= 1'000'000 / campaigns_) {
            credits_.resize(days_ * campaigns_);
            assists_.resize(days_ * campaigns_);
        }
    }
    Credits& credited(uint32_t campaign, int32_t day) {
        auto i = cell(campaign, day);
        if (credits_.empty()) return sparse_[i];
        return credits_[i];
    }
    void count_assists(uint32_t campaign, int32_t day, unsigned count) {
        if (assists_.empty())
            sparse_[cell(campaign, day)].assists += count;
        else
            assists_[size_t(campaign) * days_ + size_t(day - first_)] += count;
    }
    void add_assists(uint32_t campaign, const SessionDay* day, size_t first, size_t stop) {
        int64_t* counts = assists_.empty() ? nullptr : assists_.data() + campaign * days_;
        for (; first < stop; ++day) {
            size_t end = std::min(stop, size_t(day->stop));
            auto& count = counts ? counts[day->day - first_] : sparse_[cell(campaign, day->day)].assists;
            count += int64_t(end - first);
            first = end;
        }
    }
    void add(uint32_t campaign, int32_t day, const Totals& from) {
        auto& to = credited(campaign, day);
        to.sessions += from.sessions;
        to.purchasing += from.purchasing;
        to.revenue += from.revenue;
        if (assists_.empty()) sparse_[cell(campaign, day)].assists += from.assists;
        else assists_[campaign * days_ + size_t(day - first_)] += from.assists;
    }
    template<class F> void each(F fn) const {
        if (!credits_.empty()) {
            for (size_t d = 0; d < days_; ++d) for (size_t c = 0; c < campaigns_; ++c) {
                const auto& credit = credits_[d * campaigns_ + c];
                auto assists = assists_[c * days_ + d];
                if (credit.sessions || assists) fn(uint32_t(c), first_ + int32_t(d), Totals{credit, assists});
            }
        }
        for (const auto& [i, t] : sparse_) {
            if (t.sessions || t.assists) fn(uint32_t(i % campaigns_), int32_t(i / campaigns_) + first_, t);
        }
    }
};

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
    struct Session { Time start; int64_t revenue; Click credit; int32_t day; uint32_t day_index; };
    struct AssistRange { size_t first_session = SIZE_MAX; Time latest_click = invalid_time; };
    static constexpr uint32_t none = UINT32_MAX;
    Time origin_;
    bool vector_window_ = false;
    const Keys& click_keys_;
    // These workspaces are reused across people. Clear only touched cells, categories and campaigns.
    std::vector<Bucket> bins_;
    std::vector<uint64_t> occupied_, start_bits_;
    std::vector<uint32_t> start_rank_;
    std::unordered_map<uint32_t, Bucket> sparse_bins_;
    std::vector<uint32_t> order_, radix_scratch_;
    std::vector<Purchase> purchases_;
    std::vector<Session> sessions_;
    std::vector<SessionDay> session_days_;
    std::vector<int64_t> category_sums_;
    std::vector<uint32_t> categories_used_;
    std::vector<AssistRange> coverage_;
    std::vector<size_t> touched_;
    std::unordered_map<size_t, AssistRange> sparse_coverage_;
    enum class AssistMethod { week, mask, positions };
    AssistMethod method_ = AssistMethod::positions;
    std::vector<uint64_t> campaign_masks_;
    std::vector<size_t> covered_until_;
    std::vector<uint32_t> active_campaigns_;
    std::vector<Click> ordered_clicks_, radix_clicks_;
    size_t campaign_count_;
    size_t first_word_ = 0, last_word_ = 0;
    bool indexed_ = false;

    uint32_t cell(Time t) const { return uint32_t((t - origin_) / gap); }
    void clear_categories() {
        for (uint32_t c : categories_used_) category_sums_[c] = 0;
        categories_used_.clear();
    }
    // The timestamps in a half-hour cell span at most one session gap.
    Bucket& dense_bucket(Time ts, uint32_t pos) {
        size_t word = pos >> 6;
        occupied_[word] |= uint64_t(1) << (pos & 63);
        first_word_ = std::min(first_word_, word);
        last_word_ = std::max(last_word_, word);
        auto &b = bins_[pos];
        b.first = std::min(b.first, ts);
        b.last = std::max(b.last, ts);
        return b;
    }
    void collect(const Event* events, size_t count) {
#if defined(__AVX512CD__)
        size_t i = 0;
        if (!bins_.empty() && vector_window_) {
            const auto zero = _mm512_setzero_si512(), one = _mm512_set1_epi64(1);
            // For a 32-bit window, x = delta / 64 is below 2^26. Rounding the
            // reciprocal up contributes less than 1/28125, so the quotient is exact.
            constexpr uint64_t magic = ((uint64_t(1) << 41) + 28124) / 28125;
            for (; i + 8 <= count; i += 8) {
                auto first = _mm512_loadu_si512(events + i);
                auto second = _mm512_loadu_si512(events + i + 4);
                auto times =
                    _mm512_permutex2var_epi64(first, _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14), second);
                auto prices =
                    _mm512_permutex2var_epi64(first, _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15), second);
                auto delta = _mm512_sub_epi64(times, _mm512_set1_epi64(origin_));
                auto cell = _mm512_srli_epi64(
                    _mm512_mul_epu32(_mm512_srli_epi64(delta, 6), _mm512_set1_epi64(magic)), 41);
                auto index = _mm512_add_epi64(cell, _mm512_slli_epi64(cell, 1));
                unsigned fast = _mm512_cmpeq_epi64_mask(_mm512_conflict_epi64(cell), zero) &
                                _mm512_testn_epi64_mask(prices, _mm512_set1_epi64(0xffffffffu));
                auto *base = reinterpret_cast<char *>(bins_.data());
                auto low = _mm512_i64gather_epi64(index, base, 8);
                auto high = _mm512_i64gather_epi64(index, base + 8, 8);
                auto counts = _mm512_i64gather_epi64(index, base + 16, 8);
                _mm512_mask_i64scatter_epi64(base, __mmask8(fast), index, _mm512_min_epi64(low, times), 8);
                _mm512_mask_i64scatter_epi64(base + 8, __mmask8(fast), index, _mm512_max_epi64(high, times),
                                             8);
                _mm512_mask_i64scatter_epi64(base + 16, __mmask8(fast), index, _mm512_add_epi64(counts, one),
                                             8);
                alignas(64) uint64_t cells[8];
                _mm512_store_si512(cells, cell);
                for (unsigned j = 0; j < 8; ++j) {
                    unsigned k = cells[j], word = k >> 6;
                    occupied_[word] |= uint64_t(1) << (k & 63);
                    first_word_ = std::min(size_t(word), first_word_);
                    last_word_ = std::max(size_t(word), last_word_);
                    if (!(fast & (1u << j))) {
                        const auto &e = events[i + j];
                        auto &b = bins_[k];
                        ++b.events;
                        b.first = std::min(b.first, e.ts);
                        b.last = std::max(b.last, e.ts);
                        if (e.price) {
                            purchases_.push_back({e.price, e.category, b.purchases});
                            b.purchases = purchases_.size() - 1;
                        }
                    }
                }
            }
        }
#else
        size_t i = 0;
#endif
        for (; i < count; ++i) {
            const Event& e = events[i];
            uint32_t pos = cell(e.ts);
            Bucket* b;
            if (!bins_.empty()) {
                b = &dense_bucket(e.ts, pos);
            } else {
                auto [it, added] = sparse_bins_.try_emplace(pos, Bucket{e.ts, e.ts, 0, none});
                if (added) order_.push_back(pos);
                b = &it->second;
                b->first = std::min(b->first, e.ts);
                b->last = std::max(b->last, e.ts);
            }
            ++b->events;
            if (e.price) {
                purchases_.push_back({e.price, e.category, b->purchases});
                b->purchases = uint32_t(purchases_.size() - 1);
            }
        }
    }
    void form_sessions(Key person, std::vector<UserDay>& users) {
        session_days_.clear();
        Time last = invalid_time;
        auto take = [&](const Bucket& b) {
            if (last == invalid_time || b.first - last > gap) {
                int32_t day = utc_day(b.first);
                if (sessions_.empty() || sessions_.back().day != day) {
                    if (!sessions_.empty()) session_days_.push_back({sessions_.back().day, uint32_t(sessions_.size())});
                    clear_categories();
                    users.push_back({person, day, 0, 0, 0, 0});
                }
                ++users.back().sessions;
                sessions_.push_back({b.first, 0, {invalid_time, 0, 0}, day, uint32_t(session_days_.size())});
                if (!bins_.empty()) {
                    uint32_t start = cell(b.first);
                    start_bits_[start >> 6] |= uint64_t(1) << (start & 63);
                }
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
            for (auto pos : order_) take(sparse_bins_.at(pos));
            order_.clear();
            sparse_bins_.clear();
        } else {
            uint32_t starts = 0;
            for (size_t w = first_word_; w <= last_word_; ++w) {
                start_bits_[w] = 0;
                start_rank_[w] = starts;
                for (uint64_t mask = occupied_[w]; mask; mask &= mask - 1) {
                    uint32_t pos = uint32_t(64 * w + std::countr_zero(mask));
                    take(bins_[pos]);
                    bins_[pos] = {INT64_MAX, INT64_MIN, 0, none};
                }
                starts += std::popcount(start_bits_[w]);
                occupied_[w] = 0;
            }
        }
        indexed_ = !bins_.empty();
        session_days_.push_back({sessions_.back().day, uint32_t(sessions_.size())});
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
    void credit_sessions(CampaignDays& campaigns) {
        Click latest{invalid_time, 0, 0};
        for (const auto& s : sessions_) {
            if (s.credit.id) latest = s.credit;
            uint32_t credited = latest.ts >= s.start - week_ms ? latest.campaign : 0;
            auto& t = campaigns.credited(credited, s.day);
            ++t.sessions;
            t.purchasing += (s.revenue > 0);
            t.revenue += s.revenue;
        }
    }
    void week_assists(const Segment* chain, CampaignDays& campaigns) {
        for (auto seg = chain; seg; seg = seg->next) for (uint32_t i = 0; i < seg->m; ++i) {
            const Click& c = seg->clicks[i];
            size_t first = rank(c.ts, false);
            if (first == sessions_.size() || sessions_[first].start - c.ts > week_ms) continue;
            auto& best = sessions_[first].credit;
            if (c.ts > best.ts || (c.ts == best.ts && click_keys_.less(c.id, best.id))) best = c;
            // Every pair of clicks in a week has overlapping seven-day windows.
            size_t week = size_t((c.ts - origin_ + week_ms) / week_ms);
            size_t at = week * campaign_count_ + c.campaign;
            auto& range = coverage_.empty() ? sparse_coverage_[at] : coverage_[at];
            if (range.first_session == SIZE_MAX && !coverage_.empty()) touched_.push_back(at);
            range.first_session = std::min(range.first_session, first);
            range.latest_click = std::max(range.latest_click, c.ts);
        }
        auto assists = [&](size_t at, const AssistRange& range, size_t next_first) {
            // The only later bucket that can overlap is the next week. It owns the overlap.
            size_t first = range.first_session;
            if (first >= next_first) return;
            size_t stop = std::min(next_first, rank(range.latest_click + week_ms, true));
            uint32_t campaign = uint32_t(at % campaign_count_);
            campaigns.add_assists(campaign, session_days_.data() + sessions_[first].day_index, first, stop);
        };
        for (size_t at : touched_) {
            size_t next = at + campaign_count_;
            assists(at, coverage_[at], next < coverage_.size() ? coverage_[next].first_session : SIZE_MAX);
        }
        for (size_t at : touched_) coverage_[at] = {};
        touched_.clear();
        for (const auto& [at, range] : sparse_coverage_) {
            auto next = sparse_coverage_.find(at + campaign_count_);
            assists(at, range, next == sparse_coverage_.end() ? SIZE_MAX : next->second.first_session);
        }
        sparse_coverage_.clear();
    }
    void mask_assists(const Segment* chain, CampaignDays& campaigns) {
        for (auto seg = chain; seg; seg = seg->next) {
            for (uint32_t i = 0; i < seg->m; ++i) {
                const Click& click = seg->clicks[i];
                size_t first = rank(click.ts, false);
                if (first == sessions_.size() || sessions_[first].start - click.ts > week_ms)
                    continue;
                auto& winner = sessions_[first].credit;
                if (click.ts > winner.ts || (click.ts == winner.ts && click_keys_.less(click.id, winner.id)))
                    winner = click;
                size_t stop = rank(click.ts + week_ms, true);
                uint64_t through_last = stop == 64 ? UINT64_MAX : (uint64_t{1} << stop) - 1;
                auto& covered = campaign_masks_[click.campaign];
                if (!covered)
                    active_campaigns_.push_back(click.campaign);
                covered |= through_last & (UINT64_MAX << first);
            }
        }
        for (uint32_t campaign : active_campaigns_) {
            uint64_t remaining = std::exchange(campaign_masks_[campaign], 0);
            while (remaining) {
                size_t first = std::countr_zero(remaining);
                const auto& day = session_days_[sessions_[first].day_index];
                uint64_t today = remaining;
                if (day.stop < 64)
                    today &= (uint64_t{1} << day.stop) - 1;
                campaigns.count_assists(campaign, day.day, std::popcount(today));
                remaining ^= today;
            }
        }
        active_campaigns_.clear();
    }
    void order_clicks(const Segment *chain) {
        ordered_clicks_.clear();
        uint64_t varying = 0;
        bool ordered = true;
        const Time earliest = sessions_.front().start - week_ms;
        const Time latest = sessions_.back().start;
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

    void position_assists(const Segment* chain, CampaignDays& campaigns) {
        order_clicks(chain);
        size_t first = 0, stop = 0;
        for (const Click& click : ordered_clicks_) {
            while (first < sessions_.size() && sessions_[first].start < click.ts)
                ++first;
            stop = std::max(first, stop);
            while (stop < sessions_.size() && sessions_[stop].start - click.ts <= week_ms)
                ++stop;
            if (first == stop)
                continue;
            auto& winner = sessions_[first].credit;
            if (click.ts > winner.ts || (click.ts == winner.ts && click_keys_.less(click.id, winner.id)))
                winner = click;
            auto& previous_stop = covered_until_[click.campaign];
            size_t next = std::max(first, previous_stop);
            if (next == stop)
                continue;
            if (!previous_stop)
                active_campaigns_.push_back(click.campaign);
            campaigns.add_assists(click.campaign, session_days_.data() + sessions_[next].day_index, next, stop);
            previous_stop = stop;
        }
        for (uint32_t campaign : active_campaigns_)
            covered_until_[campaign] = 0;
        active_campaigns_.clear();
    }
    void attribute(const Segment* chain, CampaignDays& campaigns) {
        if (method_ != AssistMethod::week && sessions_.size() <= 64)
            mask_assists(chain, campaigns);
        else if (method_ == AssistMethod::positions)
            position_assists(chain, campaigns);
        else
            week_assists(chain, campaigns);
        credit_sessions(campaigns);
    }
public:
    Shopper(const Window& w, const Dimensions& d, const Keys& click_keys)
        : origin_(w.start), click_keys_(click_keys), category_sums_(d.categories.size()+1), campaign_count_(d.names.size()) {
        if (const char* value = std::getenv("SHOP_ASSISTS")) {
            std::string_view method(value);
            if (method == "week") method_ = AssistMethod::week;
            else if (method == "mask") method_ = AssistMethod::mask;
            else if (method != "positions") throw std::runtime_error("unknown SHOP_ASSISTS method");
        }
        if (method_ != AssistMethod::week) campaign_masks_.resize(campaign_count_);
        if (method_ == AssistMethod::positions) covered_until_.resize(campaign_count_);
        vector_window_ = uint64_t(w.end - w.start - 1) <= UINT32_MAX;
        size_t weeks = size_t((w.end - w.start - 1) / week_ms) + 2;
        if (weeks <= 1'000'000 / campaign_count_) coverage_.resize(weeks * campaign_count_);
        size_t n = size_t((w.end - w.start - 1) / gap + 1);
        if (n <= 65536) {
            bins_.resize(n, Bucket{INT64_MAX, INT64_MIN, 0, none});
            occupied_.resize((n+63)/64);
            start_bits_.resize(occupied_.size());
            start_rank_.resize(occupied_.size());
        }
    }
    void run(Key person, const Segment* chain, std::vector<UserDay>& users, CampaignDays& campaigns) {
        sessions_.clear();
        first_word_ = occupied_.size();
        last_word_ = 0;
        bool has_events = false;
        for (auto seg = chain; seg; seg = seg->next) if (seg->n) {
            collect(seg->events, seg->n);
            has_events = true;
        }
        if (!has_events) return;
        form_sessions(person, users);
        attribute(chain, campaigns);
    }
};
} // namespace shop
