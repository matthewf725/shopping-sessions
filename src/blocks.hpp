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
        T *next = nullptr;
        T *limit = nullptr;
    };
    struct Chain {
        Block *first = nullptr;
        Block *last = nullptr;
        size_t previous = 0;
    };
    std::array<Chain, Shards> chains_{};
    struct Free { void operator()(void* p) const { std::free(p); } };
    std::array<Cursor, Shards> shards_;
    std::vector<std::unique_ptr<void, Free>> pages_;
    std::byte* pool_ = nullptr;
    size_t blocks_left_ = 0;
    static constexpr size_t huge_page = 2 * 1024 * 1024;
    static constexpr size_t page_bytes = 4 * huge_page;
    struct Remaining {
        std::byte* cursor;
        size_t available;
    };
    std::vector<Remaining> recycled_;
    void* new_page(size_t size) {
        void* p = nullptr;
        if (posix_memalign(&p, huge_page, size))
            throw std::bad_alloc();
        std::unique_ptr<void, Free> page(p);
        pages_.push_back(std::move(page));
#if defined(__linux__) && defined(MADV_HUGEPAGE)
        madvise(p, size, MADV_HUGEPAGE);
#endif
        return p;
    }

    [[gnu::noinline]] void extend(size_t shard) {
        auto &c = shards_[shard];
        auto &chain = chains_[shard];
        if (!blocks_left_) {
            pool_ = static_cast<std::byte*>(new_page(page_bytes));
            blocks_left_ = page_bytes / sizeof(Block);
        }
        Block* b = ::new (pool_) Block;
        pool_ += sizeof(Block);
        --blocks_left_;
        b->next = nullptr;
        if (chain.last) {
            chain.last->next = b;
            chain.previous += records;
        } else {
            chain.first = b;
        }
        chain.last = b;
        c.next = b->items;
        c.limit = b->items + records;
    }

public:
  // Every deduplication worker has finished reading the parsed records.
  // Their storage can now hold the contiguous event and click ranges.
  void recycle_pages() {
      pool_ = nullptr;
      blocks_left_ = 0;
      shards_ = {};
      chains_ = {};
      recycled_.reserve(pages_.size());
      for (auto& page : pages_)
          recycled_.push_back({static_cast<std::byte*>(page.get()), page_bytes});
  }
  template <class U> U* allocate_range(size_t n) {
      static_assert(alignof(U) <= 64);
      static_assert(std::is_trivially_default_constructible_v<U> && std::is_trivially_destructible_v<U>);
      if (!n)
          return nullptr;
      if (n > (SIZE_MAX - 63) / sizeof(U))
          throw std::bad_array_new_length();
      size_t bytes = (n * sizeof(U) + 63) & ~size_t(63);
      // Best fit keeps larger unused portions available for a later shard.
      Remaining* best = nullptr;
      for (auto& p : recycled_)
          if (p.available >= bytes && (!best || p.available < best->available))
              best = &p;
      if (!best) {
          size_t size = std::max(page_bytes, bytes);
          recycled_.push_back({static_cast<std::byte*>(new_page(size)), size});
          best = &recycled_.back();
      }
      auto* ptr = best->cursor;
      best->cursor += bytes;
      best->available -= bytes;
      return ::new (ptr) U[n];
  }
    void append(size_t shard, const T& row) {
        auto& c = shards_[shard];
        if (c.next == c.limit)
            extend(shard);
        *c.next++ = row;
    }
#if defined(__AVX512CD__)
    // Reserve the first ready lane in each shard. Apply conflicting lanes in input order.
    template <size_t W, class Slow>
    void append_lanes(__m512i shard, unsigned active, const __m512i (&cols)[W], Slow slow) {
        static_assert(sizeof(T) == 8 * W && sizeof(Cursor) == 16);
        auto indices = _mm512_slli_epi64(shard, 4);
        auto next = _mm512_i64gather_epi64(indices, &shards_[0].next, 1);
        auto end = _mm512_i64gather_epi64(indices, &shards_[0].limit, 1);
        unsigned ready = active &
                         _mm512_testn_epi64_mask(_mm512_conflict_epi64(shard), _mm512_set1_epi64(active)) &
                         _mm512_cmpneq_epi64_mask(next, end);
        _mm512_mask_i64scatter_epi64(&shards_[0].next, __mmask8(ready), indices,
                                     _mm512_add_epi64(next, _mm512_set1_epi64(sizeof(T))), 1);
        auto *base = reinterpret_cast<char *>(shards_.data());
        auto address = _mm512_sub_epi64(next, _mm512_set1_epi64(reinterpret_cast<uintptr_t>(base)));
        for (size_t k = 0; k < W; ++k)
            _mm512_mask_i64scatter_epi64(base + 8 * k, __mmask8(ready), address, cols[k], 1);
        if (unsigned pending = active ^ ready) {
            alignas(64) uint64_t data[W][8];
            for (size_t k = 0; k < W; ++k)
                _mm512_store_si512(data[k], cols[k]);
            do {
                unsigned i = std::countr_zero(pending);
                slow(i, data);
                pending &= pending - 1;
            } while (pending);
        }
    }
#endif
    size_t size(size_t shard) const {
        const auto &c = shards_[shard];
        const auto &chain = chains_[shard];
        return chain.last ? chain.previous + size_t(c.next - chain.last->items) : 0;
    }
    template<class F> void visit(size_t shard, F fn) {
        const auto &c = shards_[shard];
        const auto &chain = chains_[shard];
        for (Block *b = chain.first; b; b = b->next) {
            size_t n = b == chain.last ? size_t(c.next - b->items) : records;
            fn(std::span<T>(b->items, n));
        }
    }
};
}
