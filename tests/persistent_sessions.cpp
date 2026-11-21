#include "sessions.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <random>

using namespace shop;
struct Answer {
    uint32_t first, last, count;
    std::array<int64_t, 4> revenue{};
    bool operator==(const Answer &) const = default;
};
std::vector<Answer> reference(std::vector<Event> rows) {
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.ts < b.ts; });
    std::vector<Answer> result;
    for (const auto &e : rows) {
        if (result.empty() || e.ts - result.back().last > gap)
            result.push_back({uint32_t(e.ts), uint32_t(e.ts), 0, {}});
        auto &d = result.back();
        d.last = uint32_t(e.ts);
        ++d.count;
        d.revenue[e.category] += e.price;
    }
    return result;
}
void flatten(const TimeNode *n, std::vector<Answer> &result) {
    if (!n)
        return;
    flatten(n->left, result);
    Answer d{n->span.first, n->span.last, n->span.count, {}};
    std::vector<const PurchaseBag *> todo{n->span.purchases};
    while (!todo.empty()) {
        auto *bag = todo.back();
        todo.pop_back();
        if (!bag)
            continue;
        d.revenue[bag->category] += bag->price;
        todo.push_back(bag->left);
        todo.push_back(bag->right);
    }
    result.push_back(d);
    flatten(n->right, result);
}
void check(const TimeNode *n, const std::vector<Event> &rows) {
    std::vector<Answer> observed;
    flatten(n, observed);
    assert(observed == reference(rows));
    if (n)
        assert(n->count == rows.size() && n->size == observed.size());
}
int main(int argc, char **argv) {
    assert(argc == 2);
    Window window(argv[1]);
    window.start = 0;
    window.end = uint64_t(UINT32_MAX) + 1;
    TimeBuilder builder(window);
    TimePool source, merged;
    auto freeze = [&](const std::vector<Event> &rows) {
        return builder.freeze(source, rows.data(), uint32_t(rows.size()), 0);
    };
    std::vector<Event> sparse, bridge;
    for (uint32_t i = 0; i < 1024; ++i) {
        sparse.push_back({2 * i * gap, 30, i % 4});
        bridge.push_back({2 * i * gap, 1, i % 4});
        if (i != 1023)
            bridge.push_back({(2 * i + 1) * gap, 0, 0});
    }
    auto *history = freeze(sparse), *connector = freeze(bridge);
    assert(history->size == 1024 && connector->size == 1);
    size_t before = merged.size();
    auto *joined = merged.meld(history, connector);
    assert(joined->size == 1 && merged.size() == before + 1);
    std::vector<Event> all = sparse;
    all.insert(all.end(), bridge.begin(), bridge.end());
    check(joined, all);
    check(history, sparse);
    check(connector, bridge);
    std::cout << "Bridged 1024 distinct intervals with " << merged.size() - before
              << " new interval node; both prior versions verified.\n";

    std::mt19937_64 rng(43219);
    for (int trial = 0; trial < 128; ++trial) {
        source.rewind();
        merged.rewind();
        std::vector<std::vector<Event>> batches(24);
        std::vector<const TimeNode *> roots;
        for (auto &rows : batches) {
            unsigned n = rng() % 80;
            for (unsigned i = 0; i < n; ++i) {
                uint32_t slot = rng() % 1800;
                Time t = gap * slot;
                if (i % 3)
                    t += int(rng() % 11) - 5;
                rows.push_back({std::max<Time>(0, t), uint32_t(rng() % 901), uint32_t(rng() % 4)});
            }
            auto *root = freeze(rows);
            check(root, rows);
            roots.push_back(root);
        }
        std::vector<Event> union_rows;
        const TimeNode *running = nullptr;
        for (size_t i = 0; i < batches.size(); ++i) {
            const TimeNode *previous = running;
            running = merged.meld(running, roots[i]);
            check(previous, union_rows);
            union_rows.insert(union_rows.end(), batches[i].begin(), batches[i].end());
            check(running, union_rows);
            check(roots[i], batches[i]);
        }
    }
    std::cout
        << "3072 version-preserving randomized merges matched chronological session and revenue results.\n";
}
