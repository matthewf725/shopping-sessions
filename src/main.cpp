#include "output.hpp"
#include "blocks.hpp"
#include "buffered.hpp"
#include "avx_events.hpp"
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <thread>

namespace shop {
constexpr unsigned partition_bits = 6;
constexpr size_t partitions = size_t{1} << partition_bits;
inline size_t hash_partition(uint64_t hash) { return hash >> (64 - partition_bits); }
inline size_t partition(Key key) { return hash_partition(hash_key(key)); }
// Views and carts need only two words beyond their IDs. Purchases and unusual
// date ranges point into their parser worker's exact, immutable detail array.
constexpr uint32_t detailed = 0x80000000u;
struct PackedEvent { uint32_t when, tag; };
struct Detail { int64_t ingest_order; Event event; };
struct RawEvent { Key id_hash, person; PackedEvent event; };
static_assert(sizeof(RawEvent) == 24);
struct RawGroup {
    Key key = 0;
    size_t events = 0, clicks = 0;
    RawGroup* next = nullptr;
};
template<class T> struct Owned {
    union { Key person; RawGroup* group; };
    T value;
};
using OwnedEvent = Owned<PackedEvent>;
using OwnedClick = Owned<Click>;
struct Worker {
    uint32_t number;
    std::vector<Detail> details;
    ShardedBlocks<RawEvent, partitions> parsed;
    std::array<std::vector<Segment>, partitions> segments;
    ShardedBlocks<OwnedEvent, partitions> events;
    ShardedBlocks<OwnedClick, partitions> clicks;
    std::vector<UserDay> days;
    CampaignDays campaigns;
    Shopper shopper;
    Worker(uint32_t index, const Window& w, const Dimensions& d, const Keys& click_keys)
        : number(index), campaigns(w, d.names.size()), shopper(w, d, click_keys) {}
};

template<class F> void parallel(size_t n, size_t jobs, F fn) {
    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex guard;
    auto work = [&](size_t worker) {
        try {
            for (;;) {
                size_t job = next.fetch_add(1, std::memory_order_relaxed);
                if (job >= jobs) break;
                fn(worker, job);
            }
        } catch (...) {
            std::lock_guard lock(guard);
            failure = std::current_exception();
            next.store(jobs);
        }
    };
    std::vector<std::thread> threads;
    for (size_t i = 1; i < std::min(n, jobs); ++i) threads.emplace_back(work, i);
    work(0);
    for (auto& t : threads) t.join();
    if (failure) std::rethrow_exception(failure);
}

class Profile {
    bool enabled_ = std::getenv("PROFILE") != nullptr;
    int64_t last_ = 0;
    static int64_t now_ns() {
        timespec time;
        clock_gettime(CLOCK_MONOTONIC, &time);
        return int64_t(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
    }
public:
    Profile() {
        if (enabled_) {
            last_ = now_ns();
            std::fprintf(stderr, "main_start_ns %lld\n", static_cast<long long>(last_));
        }
    }
    void mark(const char* stage) {
        if (!enabled_) return;
        auto now = now_ns();
        std::fprintf(stderr, "%s %.9f s\n", stage, double(now - last_) / 1e9);
        last_ = now;
    }
    void finish() const {
        if (enabled_) {
            std::fprintf(stderr, "output_closed_ns %lld\n", static_cast<long long>(last_));
            std::fflush(stderr);
        }
    }
};

struct Pipeline {
    Window window;
    Dimensions dim;
    Keys events{'e'}, users{'u'}, click_keys{'k'};
    People people;
    std::vector<Worker> workers;
    Pipeline(const fs::path& data, size_t n) : window(data / "manifest.json"), dim(data) {
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) workers.emplace_back(uint32_t(i), window, dim, click_keys);
    }
    void store_event(Worker& w, Key id, Key person, Time ts, int64_t order, uint32_t price, uint32_t category) {
        uint64_t when = person ? uint64_t(ts - window.start) : 0;
        PackedEvent event;
        if (!price && when <= UINT32_MAX && uint64_t(order) < detailed) {
            event = {uint32_t(when), uint32_t(order)};
        } else {
            event = {uint32_t(w.details.size()), detailed | w.number};
            w.details.push_back({order, {ts, price, category}});
        }
        // Both xor-shifts and multiplication by an odd number are invertible
        // on 64 bits. This hash is an exact event ID, and zero stays reserved.
        Key id_hash = hash_key(id);
        w.parsed.append(hash_partition(id_hash), {id_hash, person, event});
    }
    void parse_events(Chunk chunk, Worker& w) {
        auto consume = [&](Text eid, Text person, Text time, Text ingest, Text kind, bool has_product, auto purchase) {
            Key id = events.encode(eid);
            if (!id || ingest.size() != 20) return;
            auto arrival = window.arrival(ingest);
            Time ts = window.event_time(time, ingest, arrival.midnight);
            uint32_t price = 0, category = 0;
            bool valid = !person.empty() && has_product && ts >= window.start && ts < window.end &&
                         arrival.on_time(ts, ingest);
            if (valid && kind == "purchase") {
                auto [product, cents] = purchase();
                int64_t p = decimal(cents, 10'000'000);
                valid = p > 0;
                if (valid) {
                    price = uint32_t(p);
                    category = dim.products.get(product);
                    if (!category) category = dim.unknown;
                }
            } else if (valid) valid = kind == "view" || kind == "cart";
            // Keep invalid copies by event ID so the earliest ingestion still wins.
            store_event(w, id, valid ? users.encode(person) : 0, ts, arrival.order, price, category);
        };
#if defined(__AVX512BW__) && defined(__AVX512DQ__) && defined(__AVX512VL__) && defined(__AVX512CD__)
        auto accelerate = [&](const char *data, const uint32_t *offsets, const uint16_t *rows,
                              unsigned active = 255) {
            auto append = [&](Key id, Key person, uint64_t packed) {
                w.parsed.append(hash_partition(id), {id, person, {uint32_t(packed), uint32_t(packed >> 32)}});
            };
            auto eight = [&](__m512i ids, __m512i people, __m512i values, unsigned active) {
                auto shards = _mm512_srli_epi64(ids, 64 - partition_bits);
                const __m512i cols[] = {ids, people, values};
                w.parsed.append_lanes(shards, active, cols,
                                      [&](unsigned i, const auto &v) { append(v[0][i], v[1][i], v[2][i]); });
            };
            return EventLanes::run(window, data, offsets, rows, eight, active);
        };
        auto projection = BatchProjection{consume, accelerate};
#else
        auto& projection = consume;
#endif
        read_rows<7>(chunk, [&](const auto& f) {
            consume(f[0], f[1], f[2], f[3], f[4], !f[5].empty(), [&] { return std::pair{f[5], f[6]}; });
        }, projection);
    }
    void parse_clicks(Chunk chunk, Worker& w) {
        auto consume = [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            uint32_t campaign = dim.campaigns.get(f[2]);
            if (!campaign) return;
            Time ts = window.clock.parse(f[3]);
            if (ts < window.start - week_ms || ts >= window.end) return;
            Key raw_user = users.encode(f[1]);
            w.clicks.append(partition(raw_user), {raw_user, {ts, click_keys.encode(f[0]), campaign}});
        };
#if defined(__AVX512BW__) && defined(__AVX512DQ__) && defined(__AVX512VL__) && defined(__AVX512CD__)
        auto accelerate = [&](const char *data, const uint32_t *offsets, const uint16_t *rows,
                              unsigned active = 255) {
            return ClickLanes::parse(
                window, dim.campaigns, data, offsets, rows,
                [&](uint64_t hash, Key person, Key id, Time time, uint32_t campaign) {
                    w.clicks.append(hash_partition(hash), {person, {time, id, campaign}});
                },
                active);
        };
        read_rows<4>(chunk, consume, BatchProjection{consume, accelerate});
#else
        read_rows<4>(chunk, consume);
#endif
    }
    void read(const fs::path& data) {
        CsvPieces ev(data / "events"), ck(data / "clicks");
        CsvFiles links(data / "links");
        std::vector<PieceReader> inputs(workers.size());
        // File sizes only estimate capacity. Vectors grow normally for any distribution.
        size_t per_worker = ev.bytes / workers.size();
        auto preallocate = [](auto &rows, size_t estimate) {
            if (!estimate)
                return;
            rows.reserve(estimate);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
            uintptr_t start = reinterpret_cast<uintptr_t>(rows.data());
            uintptr_t end =
                start + rows.capacity() * sizeof(typename std::decay_t<decltype(rows)>::value_type);
            auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
            auto page_mask = page_size - 1;
            uintptr_t aligned = (start + page_mask) & ~page_mask;
            end &= ~page_mask;
            if (end > aligned)
                madvise(reinterpret_cast<void *>(aligned), end - aligned, MADV_HUGEPAGE);
#endif
        };
        for (auto &w : workers) {
            preallocate(w.days, per_worker / 600);
            preallocate(w.details, per_worker / 1600);
        }
        size_t click_start = 1 + ev.chunks.size();
        parallel(workers.size(), click_start + ck.chunks.size(), [&](size_t wi, size_t job) {
            if (job == 0) people.build(links, users);
            else if (job < click_start) parse_events(inputs[wi].read(ev.chunks[job - 1]), workers[wi]);
            else parse_clicks(inputs[wi].read(ck.chunks[job - click_start]), workers[wi]);
        });
    }
    int64_t ingest_order(PackedEvent e) const {
        if (e.tag & detailed) return workers[e.tag ^ detailed].details[e.when].ingest_order;
        return e.tag;
    }
    Event unpack(PackedEvent e) const {
        if (e.tag & detailed) return workers[e.tag ^ detailed].details[e.when].event;
        return {window.start + e.when, 0, 0};
    }
    void dedup(size_t p, Worker& dst) {
        size_t size = 0;
        for (auto& w : workers)
            size += w.parsed.size(p);
        if (!size)
            return;
        // A hash collision only selects a key for an exact comparison. A lone bit
        // can bypass the full table. Both passes read the same contiguous shard.
        // A pair of adjacent bits in one machine word records seen/repeated:
        // 00 -> 01 -> 11. One read-modify-write updates both flags.
        size_t words = std::bit_ceil((size + 1) / 2);
        std::vector<uint64_t> filter(words);
        const size_t slot_mask = words * 32 - 1;
        auto record = [&](uint64_t id) {
            size_t slot = id & slot_mask;
            uint64_t seen = uint64_t(1) << (2 * (slot & 31));
            auto& word = filter[slot >> 5];
            word |= seen | ((word & seen) << 1);
        };
#if defined(__AVX512CD__)
        // Three consecutive cache lines hold eight records. Extract the first
        // word of each record without eight separate memory gathers.
        auto ids_from = [](const RawEvent* rows) {
            const char* bytes = reinterpret_cast<const char*>(rows);
            auto a = _mm512_loadu_si512(bytes), b = _mm512_loadu_si512(bytes + 64),
                 c = _mm512_loadu_si512(bytes + 128);
            auto first = _mm512_permutex2var_epi64(a, _mm512_setr_epi64(0, 3, 6, 9, 12, 15, 0, 0), b);
            return _mm512_mask_permutexvar_epi64(first, 0xc0, _mm512_setr_epi64(0, 0, 0, 0, 0, 0, 2, 5), c);
        };
        auto one = _mm512_set1_epi64(1), low = _mm512_set1_epi64(31), mask = _mm512_set1_epi64(slot_mask);
#endif
        for (auto& w : workers)
            w.parsed.visit(p, [&](auto rows) {
                size_t i = 0;
#if defined(__AVX512CD__)
                for (; i + 8 <= rows.size(); i += 8) {
                    auto slot = ids_from(rows.data() + i) & mask, word = _mm512_srli_epi64(slot, 5);
                    auto bit = _mm512_sllv_epi64(one, _mm512_slli_epi64(slot & low, 1));
                    auto old = _mm512_i64gather_epi64(word, filter.data(), 8);
                    auto next = old | bit | _mm512_slli_epi64(old & bit, 1);
                    // Only the first lane for a word may scatter. Apply remaining
                    // updates to that word in order, preserving repeated IDs.
                    unsigned independent =
                        _mm512_cmpeq_epi64_mask(_mm512_conflict_epi64(word), _mm512_setzero_si512());
                    _mm512_mask_i64scatter_epi64(filter.data(), __mmask8(independent), word, next, 8);
                    for (unsigned left = 255 ^ independent; left; left &= left - 1)
                        record(rows[i + std::countr_zero(left)].id_hash);
                }
#endif
                for (; i < rows.size(); ++i)
                    record(rows[i].id_hash);
            });
        FlatMap<RawEvent*, std::identity{}> best(size / 10);
        auto emit = [&](const RawEvent& row) {
            if (row.person) {
                dst.events.append(partition(row.person), {row.person, row.event});
            }
        };
        auto repeat = [&](RawEvent& row) {
            auto [prev, fresh] = best.add(row.id_hash, &row);
            if (!fresh && ingest_order(row.event) < ingest_order(prev->event))
                prev = &row;
        };
        for (auto& w : workers)
            w.parsed.visit(p, [&](auto rows) {
                size_t i = 0;
#if defined(__AVX512CD__)
                for (; i + 8 <= rows.size(); i += 8) {
                    auto slot = ids_from(rows.data() + i) & mask;
                    auto data = _mm512_i64gather_epi64(_mm512_srli_epi64(slot, 5), filter.data(), 8);
                    auto bits = _mm512_sllv_epi64(_mm512_set1_epi64(2), _mm512_slli_epi64(slot & low, 1));
                    unsigned hit = _mm512_test_epi64_mask(data, bits);
                    const auto *mem = reinterpret_cast<const char *>(rows.data() + i);
                    const auto a = _mm512_loadu_si512(mem), b = _mm512_loadu_si512(mem + 64),
                               c = _mm512_loadu_si512(mem + 128);
                    auto names = _mm512_permutex2var_epi64(a, _mm512_setr_epi64(1, 4, 7, 10, 13, 0, 0, 0), b);
                    names = _mm512_mask_permutexvar_epi64(names, 0xe0,
                                                          _mm512_setr_epi64(0, 0, 0, 0, 0, 0, 3, 6), c);
                    auto values =
                        _mm512_permutex2var_epi64(a, _mm512_setr_epi64(2, 5, 8, 11, 14, 0, 0, 0), b);
                    values = _mm512_mask_permutexvar_epi64(values, 0xe0,
                                                           _mm512_setr_epi64(0, 0, 0, 0, 0, 1, 4, 7), c);
                    auto h = names ^ _mm512_srli_epi64(names, 32);
                    h = _mm512_mullo_epi64(h, _mm512_set1_epi64(0xd6e8feb86659fd93ULL));
                    h ^= _mm512_srli_epi64(h, 32);
                    const __m512i cols[] = {names, values};
                    unsigned active = (255 ^ hit) & _mm512_test_epi64_mask(names, names);
                    dst.events.append_lanes(_mm512_srli_epi64(h, 64 - partition_bits), active, cols,
                                            [&](unsigned j, const auto &) { emit(rows[i + j]); });
                    for (; hit; hit &= hit - 1)
                        repeat(rows[i + std::countr_zero(hit)]);
                }
#endif
                for (; i < rows.size(); ++i) {
                    auto& row = rows[i];
                    size_t slot = row.id_hash & slot_mask;
                    uint64_t repeated = uint64_t(2) << (2 * (slot & 31));
                    if (filter[slot >> 5] & repeated)
                        repeat(row);
                    else
                        emit(row);
                }
            });
        for (const auto& slot : best.slots())
            if (slot.key)
                emit(*slot.value);
    }
    void group_aliases(size_t p, Worker& dst) {
        using Group = RawGroup;
        size_t ne = 0, nc = 0;
        for (auto& w : workers) {
            ne += w.events.size(p);
            nc += w.clicks.size(p);
        }
        if (!(ne || nc))
            return;
        const size_t capacity = std::bit_ceil(std::max<size_t>((ne + nc) / 9, 8));
        // Both the primary array and deque preserve group addresses. Once a
        // row has found its group, its next pass can write directly to it.
        std::vector<Group> primary(capacity);
        std::deque<Group> overflow;
        std::vector<Group*> used;
        used.reserve(capacity / 2);
        auto lookup = [&](Key key) {
            Group* g = &primary[table_hash(key) & (capacity - 1)];
            if (g->key != key) {
                if (g->key) {
                    while (g->next) {
                        g = g->next;
                        if (g->key == key)
                            return g;
                    }
                    overflow.emplace_back();
                    g->next = &overflow.back();
                    g = g->next;
                }
                g->key = key;
                used.push_back(g);
            }
            return g;
        };
        for (auto& w : workers) {
            w.events.visit(p, [&](auto rows) {
                for (auto& e : rows) {
                    auto* g = lookup(e.person);
                    ++g->events;
                    e.group = g;
                }
            });
            w.clicks.visit(p, [&](auto rows) {
                for (auto& c : rows) {
                    auto* g = lookup(c.person);
                    ++g->clicks;
                    c.group = g;
                }
            });
        }
        size_t en = 0, cn = 0;
        for (auto* entry : used) {
            auto& g = *entry;
            size_t next_e = en + g.events, next_c = cn + g.clicks;
            g.events = en;
            g.clicks = cn;
            en = next_e;
            cn = next_c;
        }
        // Scatter once by raw ID. These arrays stay in place through the canonical
        // shopper pass, so joining aliases moves only range descriptors.
        auto* ev = dst.parsed.allocate_range<Event>(en);
        auto* ck = dst.parsed.allocate_range<Click>(cn);
#if defined(__AVX512CD__)
        // Each packed event carries a window offset or an index into its worker's detail array.
        // The first lane for each group reserves a slot. Other lanes for that group follow it.
        std::array<const Detail *, 64> details;
        for (size_t wi = 0; wi < workers.size(); ++wi)
            details[wi] = workers[wi].details.data();
        const auto even = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14);
        const auto odd = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);
        const auto zero = _mm512_setzero_si512(), one = _mm512_set1_epi64(1);
#endif
        for (auto& w : workers) {
            w.events.visit(p, [&](auto rows) {
                size_t i = 0;
#if defined(__AVX512CD__)
                auto *group_base = reinterpret_cast<char *>(primary.data());
                auto base = _mm512_set1_epi64(reinterpret_cast<uintptr_t>(group_base));
                for (; i + 8 <= rows.size(); i += 8) {
                    auto a = _mm512_loadu_si512(rows.data() + i), b = _mm512_loadu_si512(rows.data() + i + 4);
                    auto groups = _mm512_permutex2var_epi64(a, even, b);
                    auto value = _mm512_permutex2var_epi64(a, odd, b);
                    auto offsets = _mm512_sub_epi64(groups, base);
                    auto where = _mm512_i64gather_epi64(offsets, group_base + offsetof(Group, events), 1);
                    auto when = value & _mm512_set1_epi64(UINT32_MAX);
                    auto times = _mm512_add_epi64(when, _mm512_set1_epi64(window.start));
                    auto money = zero;
                    unsigned fast = _mm512_cmpeq_epi64_mask(_mm512_conflict_epi64(groups), zero);
                    unsigned tagged = _mm512_cmp_epi64_mask(value, zero, _MM_CMPINT_LT);
                    if (unsigned extra = tagged & fast) {
                        static_assert(sizeof(Detail) == 24 && offsetof(Detail, event) == 8);
                        auto worker = _mm512_srli_epi64(value, 32) & _mm512_set1_epi64(detailed - 1);
                        auto addr =
                            _mm512_mask_i64gather_epi64(zero, __mmask8(extra), worker, details.data(), 8);
                        addr = _mm512_add_epi64(
                            addr, _mm512_slli_epi64(_mm512_add_epi64(when, _mm512_slli_epi64(when, 1)), 3));
                        addr = _mm512_sub_epi64(addr, base);
                        times = _mm512_mask_i64gather_epi64(times, __mmask8(extra), addr, group_base + 8, 1);
                        money = _mm512_mask_i64gather_epi64(money, __mmask8(extra), addr, group_base + 16, 1);
                    }
                    _mm512_mask_i64scatter_epi64(group_base + offsetof(Group, events), __mmask8(fast),
                                                 offsets, _mm512_add_epi64(where, one), 1);
                    auto dest = _mm512_slli_epi64(where, 1);
                    _mm512_mask_i64scatter_epi64(ev, __mmask8(fast), dest, times, 8);
                    _mm512_mask_i64scatter_epi64(reinterpret_cast<char *>(ev) + 8, __mmask8(fast), dest,
                                                 money, 8);
                    for (unsigned pending = 255 ^ fast; pending; pending &= pending - 1) {
                        auto &e = rows[i + std::countr_zero(pending)];
                        ev[e.group->events++] = unpack(e.value);
                    }
                }
#endif
                for (; i < rows.size(); ++i) {
                    auto &e = rows[i];
                    ev[e.group->events++] = unpack(e.value);
                }
            });
            w.clicks.visit(p, [&](auto rows) {
                for (auto& c : rows)
                    ck[c.group->clicks++] = c.value;
            });
        }
        size_t e0 = 0, c0 = 0;
        for (auto* entry : used) {
            auto& g = *entry;
            // A click-only alias may share a person with an event in another shard.
            Key canonical = people.linked_name(g.key);
            Segment seg{g.events != e0 ? &ev[e0] : nullptr, g.clicks != c0 ? &ck[c0] : nullptr,
                        uint32_t(g.events - e0), uint32_t(g.clicks - c0), canonical};
            if (canonical)
                dst.segments[partition(canonical)].push_back(seg);
            else
                dst.shopper.run(g.key, &seg, dst.days, dst.campaigns);
            e0 = g.events;
            c0 = g.clicks;
        }
    }
    void join_aliases(size_t p, Worker& dst) {
        size_t count = 0;
        for (auto& w : workers) count += w.segments[p].size();
        FlatMap<Segment*> groups(count / 2);
        for (auto& w : workers) for (auto& seg : w.segments[p]) {
            auto [head, fresh] = groups.add(seg.name, &seg);
            if (!fresh) { seg.next = head; head = &seg; }
        }
        for (auto& slot : groups.slots()) if (slot.key) dst.shopper.run(slot.key, slot.value, dst.days, dst.campaigns);
        for (auto& w : workers) std::vector<Segment>().swap(w.segments[p]);
    }
    void run(const fs::path& data, const fs::path& output, Profile& profile) {
        read(data); profile.mark("parse and link");
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { dedup(p, workers[wi]); });
        for (auto& w : workers)
            w.parsed.recycle_pages();
        profile.mark("deduplicate and partition");
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { group_aliases(p, workers[wi]); });
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { join_aliases(p, workers[wi]); });
        profile.mark("sessions and attribution");
        fs::create_directories(output / "user_daily");
        fs::create_directories(output / "campaign_daily");
        parallel(workers.size(), workers.size() + 1, [&](size_t, size_t part) {
            if (part < workers.size()) {
                if (part == 0 || !workers[part].days.empty())
                    write_users(output / "user_daily" / ("part-" + std::to_string(part) + ".parquet"), workers[part].days, users, dim);
            } else {
                CampaignDays total(window, dim.names.size());
                for (auto& w : workers) w.campaigns.each([&](uint32_t c, int32_t day, const Totals& from) {
                    total.add(c, day, from);
                });
                write_campaigns(output / "campaign_daily" / "part-0.parquet", total, dim);
            }
        });
        profile.mark("write parquet");
    }
};
} // namespace shop

int main(int argc, char** argv) {
    shop::Profile profile;
    try {
        auto env = [](const char* key, const char* fallback) { auto value = getenv(key); return value ? value : fallback; };
        shop::fs::path data = argc > 1 ? argv[1] : env("DATA_DIR", "/data");
        shop::fs::path output = argc > 2 ? argv[2] : env("OUTPUT_DIR", "/output");
        size_t threads = std::clamp(std::stoi(env("NCPU", "8")), 1, 64);
        shop::Pipeline pipeline(data, threads);
        profile.mark("setup");
        pipeline.run(data, output, profile);
        profile.finish();
        // All Parquet files are closed and every worker has joined. The OS can
        // reclaim the process-private scratch buffers together on successful exit.
        std::_Exit(EXIT_SUCCESS);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
