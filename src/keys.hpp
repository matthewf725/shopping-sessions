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

template<class T, auto bucket_hash = hash_key>
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
    FlatMap<uint64_t> entries_;  // During union-find: node index. Afterwards: canonical key.
public:
    void build(const CsvFiles& links, Keys& keys) {
        entries_.reserve(links.bytes / 17);
        struct Node { uint32_t parent, size; Key name; };
        std::vector<Node> nodes;
        nodes.reserve(links.bytes / 17);
        auto root = [&](uint32_t i) {
            while (nodes[i].parent != i) { i = nodes[i].parent = nodes[nodes[i].parent].parent; }
            return i;
        };
        auto node = [&](Key key) {
            auto [i, fresh] = entries_.add(key, nodes.size());
            if (fresh) nodes.push_back({uint32_t(i), 1, key});
            return root(uint32_t(i));
        };
        for (auto chunk : links.chunks) read_rows<2>(chunk, [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            uint32_t a = node(keys.encode(f[0])), b = node(keys.encode(f[1]));
            if (a == b) return;
            if (nodes[a].size < nodes[b].size) std::swap(a, b);
            nodes[b].parent = a;
            nodes[a].size += nodes[b].size;
            if (keys.less(nodes[b].name, nodes[a].name)) nodes[a].name = nodes[b].name;
        });
        for (auto& slot : entries_.slots()) if (slot.key) slot.value = nodes[root(uint32_t(slot.value))].name;
    }
    Key linked_name(Key key) const {
        const auto* name = entries_.get(key);
        return name ? *name : 0;
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
