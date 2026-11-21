#pragma once
#include "input.hpp"
#include <deque>
#include <mutex>
#include <unordered_map>

namespace shop {
using Key = uint64_t;
constexpr Key external_bit = Key(1) << 63;

inline uint64_t hash_key(Key k) {
    k ^= k >> 32;
    k *= 0xd6e8feb86659fd93ULL;
    return k ^ (k >> 32);
}

inline uint64_t table_hash(Key key) {
#if defined(__x86_64__) && defined(__SSE4_2__)
    return _mm_crc32_u64(0, key);
#else
    return hash_key(key);
#endif
}

// Direct keys are the exact suffix bytes in lexical order, padded with zeroes.
// Spellings that cannot be represented this way get disjoint interned keys.
class Keys {
    char prefix_;
    mutable std::mutex mutex_;
    std::deque<std::string> other_;
    size_t longest_ = 9;
    std::unordered_map<Text, Key> index_;
    Key intern(Text s) {
        std::lock_guard lock(mutex_);
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        Key k = external_bit | other_.size();
        other_.emplace_back(s);
        longest_ = std::max(longest_, s.size());
        index_.emplace(other_.back(), k);
        return k;
    }
public:
    explicit Keys(char prefix) : prefix_(prefix) {}
    Key encode(Text s) {
        if (s.empty()) return 0;
        if (s.size() >= 2 && s.size() <= 9 && s.front() == prefix_ &&
            static_cast<unsigned char>(s[1]) < 128 && s.back() != '\0') {
            // The leading bit is reserved for interned strings. A nonzero last
            // byte separates the suffix from its zero padding, including when
            // earlier bytes contain NUL or non-ASCII values.
            size_t n = s.size() - 1;
            uint64_t word = 0;
            if (n >= 7) memcpy(&word, s.data() + s.size() - 8, 8);
            else memcpy(&word, s.data() + 1, n);
            if constexpr (std::endian::native == std::endian::little) word = __builtin_bswap64(word);
            return n >= 7 ? word << (8 * (8 - n)) : word;
        }
        return intern(s);
    }
    size_t longest() const {
        std::lock_guard lock(mutex_);
        return longest_;
    }
    std::string decode(Key k) const {
        if (k & external_bit) {
            std::lock_guard lock(mutex_);
            return other_[k ^ external_bit];
        }
        char bytes[9];
        bytes[0] = prefix_;
        int n = 1;
        while (k) { bytes[n++] = char(k >> 56); k <<= 8; }
        return std::string(bytes, n);
    }
    bool less(Key a, Key b) const {
        if (!((a | b) & external_bit)) return a < b;
        return decode(a) < decode(b);
    }
};

template<class T, auto bucket_hash = table_hash>
class FlatMap {
public:
    struct Slot { Key key{}; T value{}; };
private:
    std::vector<Slot> slots_;
    size_t used_ = 0;
    size_t index(Key key) const {
        size_t mask = slots_.size() - 1;
        size_t pos = bucket_hash(key) & mask;
        while (slots_[pos].key && slots_[pos].key != key) pos = (pos + 1) & mask;
        return pos;
    }
public:
    explicit FlatMap(size_t expected = 0) { reserve(expected); }
    void reserve(size_t n) {
        size_t capacity = std::bit_ceil(std::max<size_t>(8, n + n / 2 + 1));
        if (capacity <= slots_.size()) return;
        std::vector<Slot> old = std::move(slots_);
        slots_.resize(capacity);
        for (auto& slot : old) if (slot.key) slots_[index(slot.key)] = slot;
    }
    T* get(Key key) {
        auto& slot = slots_[index(key)];
        return slot.key ? &slot.value : nullptr;
    }
    const T* get(Key key) const {
        const auto& slot = slots_[index(key)];
        return slot.key ? &slot.value : nullptr;
    }
    std::pair<T&, bool> add(Key key, T initial) {
        if ((used_ + 1) * 3 > slots_.size() * 2) reserve(used_ + 1);
        auto& slot = slots_[index(key)];
        if (slot.key) return {slot.value, false};
        ++used_;
        slot = {key, initial};
        return {slot.value, true};
    }
    auto& slots() { return slots_; }
};

class People {
    struct Tree {
        int32_t parent_or_size;
        uint32_t smallest;
    };
    struct Slot {
        Key key{};
        union { Tree tree; Key canonical; };
        Slot() : tree{-1, 0} {}
    };
    static_assert(sizeof(Slot) == 16);
    std::vector<Slot> slots_;
    size_t used_ = 0;

    static uint32_t position(const std::vector<Slot>& table, Key key, uint64_t hash) {
        size_t mask = table.size() - 1;
        uint32_t i = uint32_t(hash & mask);
        while (table[i].key && table[i].key != key)
            i = uint32_t((i + 1) & mask);
        return i;
    }
    uint32_t root(uint32_t i) {
        while (slots_[i].tree.parent_or_size >= 0) {
            auto parent = uint32_t(slots_[i].tree.parent_or_size);
            auto grandparent = slots_[parent].tree.parent_or_size;
            if (grandparent < 0) return parent;
            slots_[i].tree.parent_or_size = grandparent;
            i = uint32_t(grandparent);
        }
        return i;
    }
    void grow(size_t count) {
        size_t capacity = std::bit_ceil(std::max<size_t>(16, count + count / 2 + 2));
        if (capacity <= slots_.size()) return;
        if (capacity > size_t{1} << 31) throw std::bad_alloc();
        std::vector<Slot> larger(capacity);
        std::vector<uint32_t> moved(slots_.size());
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].key) continue;
            auto j = position(larger, slots_[i].key, table_hash(slots_[i].key));
            larger[j].key = slots_[i].key;
            moved[i] = j;
        }
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].key) continue;
            auto state = slots_[i].tree;
            if (state.parent_or_size >= 0)
                state.parent_or_size = int32_t(moved[uint32_t(state.parent_or_size)]);
            state.smallest = moved[state.smallest];
            larger[moved[i]].tree = state;
        }
        slots_.swap(larger);
    }
    uint32_t node(Key key, uint64_t hash) {
        uint32_t i = position(slots_, key, hash);
        if (slots_[i].key) return i;
        slots_[i].key = key;
        slots_[i].tree = {-1, i};
        ++used_;
        return i;
    }
public:
    void build(const CsvFiles& links, Keys& keys) {
        grow(links.bytes / 17);
        struct Edge { Key left, right; uint64_t left_hash, right_hash; };
        std::array<Edge, 32> batch;
        size_t count = 0;
        auto flush = [&] {
            // All inserted nodes in this batch keep their slot until both endpoints are joined.
            if ((used_ + 2 * count) * 3 > slots_.size() * 2) grow(used_ + 2 * count);
            for (size_t i = 0; i < count; ++i) {
                const auto& edge = batch[i];
                uint32_t a = root(node(edge.left, edge.left_hash));
                uint32_t b = root(node(edge.right, edge.right_hash));
                if (a == b) continue;
                if (slots_[a].tree.parent_or_size > slots_[b].tree.parent_or_size) std::swap(a, b);
                auto& winner = slots_[a].tree;
                auto& child = slots_[b].tree;
                winner.parent_or_size += child.parent_or_size;
                child.parent_or_size = int32_t(a);
                if (keys.less(slots_[child.smallest].key, slots_[winner.smallest].key))
                    winner.smallest = child.smallest;
            }
            count = 0;
        };
        for (auto chunk : links.chunks) read_rows<2>(chunk, [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            Key a = keys.encode(f[0]), b = keys.encode(f[1]);
            uint64_t ah = table_hash(a), bh = table_hash(b);
            __builtin_prefetch(&slots_[ah & (slots_.size() - 1)], 1, 1);
            __builtin_prefetch(&slots_[bh & (slots_.size() - 1)], 1, 1);
            batch[count++] = {a, b, ah, bh};
            if (count == batch.size()) flush();
        });
        flush();
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].key)
                slots_[i].tree.smallest = slots_[root(i)].tree.smallest;
        for (auto& s : slots_)
            if (s.key)
                s.canonical = slots_[s.tree.smallest].key;
    }
    Key linked_name(Key key) const {
        const auto& slot = slots_[position(slots_, key, table_hash(key))];
        return slot.key ? slot.canonical : 0;
    }
};

// Exact lookup with a dense fast path for p123 and c_123. Verify the spelling
// before indexing so case variants and leading zeroes keep their identities.
class Catalog {
    friend struct ClickLanes;
    std::string prefix_;
    std::vector<uint32_t> dense_;
    std::unordered_map<std::string, uint32_t> other_;
    int64_t number(Text key) const {
        if (!key.starts_with(prefix_)) return -1;
        key.remove_prefix(prefix_.size());
        if (key.empty() || key.size() > 7 || (key.size() > 1 && key[0] == '0')) return -1;
        return decimal(key, 4'000'000);
    }
public:
    explicit Catalog(std::string prefix) : prefix_(std::move(prefix)) {}
    void add(Text key, uint32_t value) {
        int64_t n = number(key);
        if (n < 0) other_.emplace(key, value);
        else {
            if (size_t(n) >= dense_.size()) dense_.resize(n + 1);
            dense_[n] = value;
        }
    }
    uint32_t get(Text key) const {
        int64_t n = number(key);
        if (n >= 0) return size_t(n) < dense_.size() ? dense_[n] : 0;
        if (other_.empty()) return 0;
        auto it = other_.find(std::string(key));
        return it == other_.end() ? 0 : it->second;
    }
};

struct Dimensions {
    Catalog products{"p"}, campaigns{"c_"};
    std::vector<std::string> categories;
    struct Campaign { std::string id, channel; };
    std::vector<Campaign> names{{"organic", "organic"}};
    uint32_t unknown;
    explicit Dimensions(const fs::path& data) {
        CsvFiles prod(data / "products.csv"), camp(data / "campaigns.csv");
        std::vector<std::array<Text, 2>> rows;
        std::unordered_map<Text, uint32_t> category{{"unknown", 0}};
        for (auto chunk : prod.chunks) read_rows<2>(chunk, [&](const auto& f) {
            rows.push_back(f);
            category.try_emplace(f[1], 0);
        });
        for (auto& [name, unused] : category) categories.emplace_back(name);
        std::sort(categories.begin(), categories.end());
        for (size_t i = 0; i < categories.size(); ++i) category[categories[i]] = uint32_t(i + 1);
        unknown = category.at("unknown");
        for (const auto& row : rows) products.add(row[0], category.at(row[1]));
        for (auto chunk : camp.chunks) read_rows<2>(chunk, [&](const auto& f) {
            campaigns.add(f[0], uint32_t(names.size()));
            names.push_back({std::string(f[0]), std::string(f[1])});
        });
    }
};
} // namespace shop
