#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace shop {
// Blocks are allocated in arrival order. Different hash partitions can share a
// huge page, and pointers to earlier records remain valid while a partition grows.
template<class T, size_t Shards>
class ShardedBlocks {
    static_assert(std::is_trivially_destructible_v<T>);
    static constexpr size_t records = 128;
    struct alignas(64) Block {
        Block* next;
        alignas(64) T items[records];
    };
    struct Cursor {
        Block* first = nullptr;
        Block* last = nullptr;
        T* next = nullptr;
        T* limit = nullptr;
        size_t previous = 0;
    };
    struct Free { void operator()(void* p) const { std::free(p); } };
    std::array<Cursor, Shards> shards_;
    std::vector<std::unique_ptr<void, Free>> pages_;
    std::byte* pool_ = nullptr;
    size_t blocks_left_ = 0;

    [[gnu::noinline]] void extend(Cursor& c) {
        if (!blocks_left_) {
            constexpr size_t huge_page = 2 * 1024 * 1024, bytes = 4 * huge_page;
            void* p = nullptr;
            if (posix_memalign(&p, huge_page, bytes)) throw std::bad_alloc();
            std::unique_ptr<void, Free> page(p);
            pages_.push_back(std::move(page));
#if defined(__linux__) && defined(MADV_HUGEPAGE)
            madvise(p, bytes, MADV_HUGEPAGE);
#endif
            pool_ = static_cast<std::byte*>(p);
            blocks_left_ = bytes / sizeof(Block);
        }
        Block* b = ::new (pool_) Block;
        pool_ += sizeof(Block);
        --blocks_left_;
        b->next = nullptr;
        if (c.last) {
            c.last->next = b;
            c.previous += records;
        } else {
            c.first = b;
        }
        c.last = b;
        c.next = b->items;
        c.limit = b->items + records;
    }
public:
    void append(size_t shard, const T& row) {
        auto& c = shards_[shard];
        if (c.next == c.limit) extend(c);
        *c.next++ = row;
    }
    size_t size(size_t shard) const {
        const auto& c = shards_[shard];
        return c.last ? c.previous + size_t(c.next - c.last->items) : 0;
    }
    template<class F> void visit(size_t shard, F fn) {
        const auto& c = shards_[shard];
        for (Block* b = c.first; b; b = b->next) {
            size_t n = b == c.last ? size_t(c.next - b->items) : records;
            fn(std::span<T>(b->items, n));
        }
    }
};
}
