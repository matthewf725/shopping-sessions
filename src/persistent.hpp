#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

namespace shop {
// Shared purchase lists keep exact prices and categories through every merge.
struct PurchaseBag {
    const PurchaseBag *left;
    const PurchaseBag *right;
    uint32_t price, category;
};
template <class T> class FrozenArena {
    static constexpr unsigned page_size = 4096;
    std::vector<std::unique_ptr<T[]>> pages_;
    size_t used_ = 0;

  public:
    void rewind() { used_ = 0; }
    size_t size() const { return used_; }
    T *put(T value) {
        size_t page = used_ / page_size, index = used_ % page_size;
        if (page == pages_.size())
            pages_.push_back(std::make_unique_for_overwrite<T[]>(page_size));
        ++used_;
        auto *out = pages_[page].get() + index;
        *out = value;
        return out;
    }
};
struct Span {
    uint32_t first, last, count;
    const PurchaseBag *purchases;
};

struct TimeNode {
    const TimeNode *left;
    const TimeNode *right;
    Span span;
    uint64_t priority;
    uint32_t first, last, count, size;
    const PurchaseBag *all_purchases;
};

// Published roots are immutable. Treap splits and joins copy only boundary paths.
class TimePool {
    FrozenArena<TimeNode> nodes_;
    FrozenArena<PurchaseBag> purchases_;
    using Node = const TimeNode *;
    static uint64_t priority(uint32_t key) {
        uint64_t z = uint64_t(key) + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    static bool before(uint32_t last, uint32_t first) { return uint64_t(last) + uint64_t(gap) < first; }
    static bool covers(Span a, Node b) { return !before(b->first, a.first) && !before(a.last, b->last); }
    static Span all(Node n) { return {n->first, n->last, n->count, n->all_purchases}; }
    Node with_children(Node n, Node left, Node right) {
        if (n->left == left && n->right == right)
            return n;
        auto *out = nodes_.put(*n);
        out->left = left;
        out->right = right;
        refresh(out);
        return out;
    }
    Node join(Node a, Node b) {
        if (!a)
            return b;
        if (!b)
            return a;
        if (a->priority > b->priority)
            return with_children(a, a->left, join(a->right, b));
        return with_children(b, join(a, b->left), b->right);
    }
    // Ends and starts are both increasing. Whole subtrees that satisfy the
    // comparison are returned unchanged without reading any descendants.
    template <bool ByEnd> std::pair<Node, Node> split(Node n, uint32_t boundary) {
        if (!n)
            return {nullptr, nullptr};
        if constexpr (ByEnd) {
            if (before(n->last, boundary))
                return {n, nullptr};
            if (!before(n->first, boundary) || n->size == 1)
                return {nullptr, n};
        } else {
            if (!before(boundary, n->last))
                return {n, nullptr};
            if (before(boundary, n->first))
                return {nullptr, n};
        }
        bool left;
        if constexpr (ByEnd)
            left = before(n->span.last, boundary);
        else
            left = !before(boundary, n->span.first);
        if (left) {
            auto [a, b] = split<ByEnd>(n->right, boundary);
            return {with_children(n, n->left, a), b};
        }
        auto [a, b] = split<ByEnd>(n->left, boundary);
        return {a, with_children(n, b, n->right)};
    }
    Node insert(Node root, Span item) {
        if (!root)
            return leaf(item);
        if (covers(item, root))
            return leaf(combine_span(item, all(root)));
        auto [left, rest] = split<true>(root, item.first);
        auto [overlap, right] = split<false>(rest, item.last);
        if (overlap)
            item = combine_span(item, all(overlap));
        return join(join(left, leaf(item)), right);
    }

  public:
    void rewind() {
        nodes_.rewind();
        purchases_.rewind();
    }
    size_t size() const { return nodes_.size(); }
    const PurchaseBag *purchase(const PurchaseBag *previous, const Event &e) {
        return purchases_.put({previous, nullptr, e.price, e.category});
    }
    const PurchaseBag *combine(const PurchaseBag *a, const PurchaseBag *b) {
        if (!a)
            return b;
        if (!b)
            return a;
        return purchases_.put({a, b, 0, 0});
    }
    Span combine_span(const Span &a, const Span &b) {
        return {std::min(a.first, b.first), std::max(a.last, b.last), a.count + b.count,
                combine(a.purchases, b.purchases)};
    }
    // Only the Cartesian builder mutates unpublished nodes.
    TimeNode *leaf(Span item) {
        return nodes_.put({nullptr, nullptr, item, priority(item.first), item.first, item.last, item.count, 1,
                           item.purchases});
    }
    void refresh(TimeNode *n) {
        Node a = n->left, b = n->right;
        n->first = a ? a->first : n->span.first;
        n->last = b ? b->last : n->span.last;
        n->count = n->span.count + (a ? a->count : 0) + (b ? b->count : 0);
        n->size = 1 + (a ? a->size : 0) + (b ? b->size : 0);
        n->all_purchases = combine(combine(a ? a->all_purchases : nullptr, n->span.purchases),
                                   b ? b->all_purchases : nullptr);
    }
    Node meld(Node a, Node b) {
        if (!a)
            return b;
        if (!b)
            return a;
        if (before(a->last, b->first))
            return join(a, b);
        if (before(b->last, a->first))
            return join(b, a);
        if (covers(a->span, b)) {
            if (a->size == 1)
                return leaf(combine_span(a->span, all(b)));
            // The incumbent interval connects every hole inside b. Treating b
            // as one span is exact, even if its endpoints extend the incumbent.
            return insert(a, all(b));
        }
        if (covers(b->span, a)) {
            if (b->size == 1)
                return leaf(combine_span(b->span, all(a)));
            return insert(b, all(a));
        }
        if (a->size < b->size)
            std::swap(a, b);
        a = insert(a, b->span);
        return meld(meld(a, b->left), b->right);
    }
};

class TimeBuilder {
    static constexpr unsigned max_cells = 4096;
    std::vector<Span> cells_;
    std::array<uint64_t, 64> bits_{};
    uint64_t words_ = 0;
    std::vector<TimeNode *> stack_;

  public:
    explicit TimeBuilder(const Window &window) {
        size_t n = (window.end - window.start - 1) / gap + 1;
        if (n <= max_cells && uint64_t(window.end - window.start - 1) <= UINT32_MAX)
            cells_.resize(n);
    }
    bool supports() const { return !cells_.empty(); }
    const TimeNode *freeze(TimePool &pool, const Event *rows, uint32_t n, Time start) {
        for (uint32_t i = 0; i < n; ++i) {
            const Event &e = rows[i];
            uint32_t when = uint32_t(e.ts - start), k = when / uint32_t(gap);
            auto &c = cells_[k];
            if (!c.count) {
                c = {when, when, 1, nullptr};
                bits_[k >> 6] |= uint64_t(1) << (k & 63);
                words_ |= uint64_t(1) << (k >> 6);
            } else {
                c.first = std::min(c.first, when);
                c.last = std::max(c.last, when);
                ++c.count;
            }
            if (e.price)
                c.purchases = pool.purchase(c.purchases, e);
        }
        auto append = [&](Span interval) {
            TimeNode *next = pool.leaf(interval);
            while (!stack_.empty() && stack_.back()->priority < next->priority) {
                auto *old = stack_.back();
                stack_.pop_back();
                pool.refresh(old);
                next->left = old;
            }
            if (!stack_.empty())
                stack_.back()->right = next;
            stack_.push_back(next);
        };
        Span interval{};
        for (uint64_t words = words_; words; words &= words - 1) {
            unsigned w = std::countr_zero(words);
            for (uint64_t bits = bits_[w]; bits; bits &= bits - 1) {
                auto &c = cells_[w * 64 + std::countr_zero(bits)];
                if (interval.count && uint64_t(interval.last) + uint64_t(gap) < c.first) {
                    append(interval);
                    interval = {};
                }
                interval = interval.count ? pool.combine_span(interval, c) : c;
                c.count = 0;
            }
            bits_[w] = 0;
        }
        words_ = 0;
        if (interval.count)
            append(interval);
        const TimeNode *root = stack_.empty() ? nullptr : stack_.front();
        while (!stack_.empty()) {
            pool.refresh(stack_.back());
            stack_.pop_back();
        }
        return root;
    }
};
} // namespace shop
