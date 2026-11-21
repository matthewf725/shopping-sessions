#pragma once
#include "keys.hpp"

namespace shop {
constexpr Time gap = 30 * 60 * 1000;
struct Event { Time ts; uint32_t price, category; };
struct Click { Time ts; Key id; uint32_t campaign; };
} // namespace shop
#include "persistent.hpp"
namespace shop {
// A range of observations for one raw user. Linked aliases share a chain.
struct Segment {
    const Event* events;
    const Click* clicks;
    uint32_t n, m;
    Key name;
    Segment* next = nullptr;
    const TimeNode *root = nullptr;
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
    TimePool merged_versions_;
    TimeBuilder residual_builder_;
    std::vector<const PurchaseBag *> bag_stack_;
    template <class Take> void components(const TimeNode *n, Take &take) {
        if (!n)
            return;
        components(n->left, take);
        const auto &span = n->span;
        take(Bucket{origin_ + span.first, origin_ + span.last, span.count, none}, span.purchases);
        components(n->right, take);
    }
    const Keys& click_keys_;
    // These workspaces are reused across people. Clear only touched cells, categories and campaigns.
    std::vector<Bucket> bins_;
    std::vector<uint64_t> occupied_, refined_, start_bits_;
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
    size_t campaign_count_;
    size_t first_word_ = 0, last_word_ = 0;
    bool indexed_ = false;

    uint32_t cell(Time t) const { return uint32_t((t - origin_) / gap); }
    void clear_categories() {
        for (uint32_t c : categories_used_) category_sums_[c] = 0;
        categories_used_.clear();
    }
    // A two-hour slot starts with one connected component. Any event inside
    // that interval or within one gap of an endpoint belongs to it.
    Bucket& dense_bucket(Time ts, uint32_t pos) {
        size_t word = pos >> 6;
        uint32_t base = pos & ~uint32_t(3);
        uint64_t group_bit = uint64_t(1) << (base & 63);
        uint64_t& used = occupied_[word];
        if (!(refined_[word] & group_bit)) {
            auto& group = bins_[base];
            if (!(used & group_bit)) {
                used |= group_bit;
                first_word_ = std::min(first_word_, word);
                last_word_ = std::max(last_word_, word);
                group = {ts, ts, 0, none};
                return group;
            }
            if (ts >= group.first - gap && ts <= group.last + gap) {
                group.first = std::min(ts, group.first);
                group.last = std::max(ts, group.last);
                return group;
            }
            // Keep the connected interval across its half-hour cells so a later
            // event can bridge the gap. All its counts and purchases stay together.
            Bucket previous = group;
            uint32_t first = cell(previous.first), last = cell(previous.last);
            refined_[word] |= group_bit;
            used &= ~group_bit;
            for (uint32_t cell_id = first; cell_id <= last; ++cell_id) {
                Time edge = origin_ + cell_id * gap;
                bins_[cell_id] = {std::max(edge, previous.first), std::min(edge + gap - 1, previous.last), 0, none};
                used |= uint64_t(1) << (cell_id & 63);
            }
            bins_[first].events = previous.events;
            bins_[first].purchases = previous.purchases;
        }
        uint64_t bit = uint64_t(1) << (pos & 63);
        auto& b = bins_[pos];
        if (used & bit) {
            b.first = std::min(ts, b.first);
            b.last = std::max(ts, b.last);
        } else {
            used |= bit;
            b = {ts, ts, 0, none};
        }
        return b;
    }
    void collect(const Event* events, size_t count) {
        for (size_t i = 0; i < count; ++i) {
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
    void form_sessions(Key person, std::vector<UserDay> &users, const TimeNode *root = nullptr) {
        session_days_.clear();
        Time last = invalid_time;
        auto take = [&](const Bucket &b, const PurchaseBag *bag = nullptr) {
            if (last == invalid_time || b.first - last > gap) {
                int32_t day = utc_day(b.first);
                if (sessions_.empty() || sessions_.back().day != day) {
                    if (!sessions_.empty())
                        session_days_.push_back({sessions_.back().day, uint32_t(sessions_.size())});
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
            auto account = [&](uint32_t price, uint32_t category) {
                auto &sum = category_sums_[category];
                if (!sum)
                    categories_used_.push_back(category);
                sum += price;
                if (sum > category_sums_[daily.top] ||
                    (sum == category_sums_[daily.top] && category < daily.top))
                    daily.top = category;
                daily.revenue += price;
                sessions_.back().revenue += price;
            };
            for (uint32_t i = b.purchases; i != none; i = purchases_[i].next)
                account(purchases_[i].price, purchases_[i].category);
            if (bag) {
                bag_stack_.push_back(bag);
                while (!bag_stack_.empty()) {
                    auto *item = bag_stack_.back();
                    bag_stack_.pop_back();
                    while (item) {
                        if (item->price)
                            account(item->price, item->category);
                        if (item->right)
                            bag_stack_.push_back(item->right);
                        item = item->left;
                    }
                }
            }
        };
        if (root) {
            first_word_ = (root->first / uint32_t(gap)) >> 6;
            last_word_ = (root->last / uint32_t(gap)) >> 6;
            std::fill(start_bits_.begin() + first_word_, start_bits_.begin() + last_word_ + 1, 0);
            components(root, take);
            uint32_t starts = 0;
            for (size_t i = first_word_; i <= last_word_; ++i) {
                start_rank_[i] = starts;
                starts += std::popcount(start_bits_[i]);
            }
        } else if (bins_.empty()) {
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
                }
                starts += std::popcount(start_bits_[w]);
                occupied_[w] = 0;
                refined_[w] = 0;
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
    void attribute(const Segment* chain, CampaignDays& campaigns) {
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
public:
  Shopper(const Window &w, const Dimensions &d, const Keys &click_keys)
      : origin_(w.start), residual_builder_(w), click_keys_(click_keys),
        category_sums_(d.categories.size() + 1), campaign_count_(d.names.size()) {
      size_t weeks = size_t((w.end - w.start - 1) / week_ms) + 2;
      if (weeks <= 1'000'000 / campaign_count_)
          coverage_.resize(weeks * campaign_count_);
      size_t n = size_t((w.end - w.start - 1) / gap + 1);
      if (n <= 65536) {
          bins_.resize(n);
          occupied_.resize((n + 63) / 64);
          refined_.resize(occupied_.size());
          start_bits_.resize(occupied_.size());
          start_rank_.resize(occupied_.size());
      }
  }
    void run(Key person, const Segment* chain, std::vector<UserDay>& users, CampaignDays& campaigns) {
        sessions_.clear();
        first_word_ = occupied_.size();
        last_word_ = 0;
        const TimeNode *root = nullptr;
        merged_versions_.rewind();
        for (auto *seg = chain; seg; seg = seg->next)
            if (seg->root)
                root = merged_versions_.meld(root, seg->root);
        bool has_events = root != nullptr;
        for (auto *seg = chain; seg; seg = seg->next)
            if (seg->n) {
                if (root)
                    root = merged_versions_.meld(
                        root, residual_builder_.freeze(merged_versions_, seg->events, seg->n, origin_));
                else
                    collect(seg->events, seg->n);
                has_events = true;
            }
        if (!has_events)
            return;
        form_sessions(person, users, root);
        attribute(chain, campaigns);
    }
};
} // namespace shop
