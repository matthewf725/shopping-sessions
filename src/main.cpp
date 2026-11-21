#include "output.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace shop {
constexpr size_t partitions = 256;
inline size_t partition(Key key) { return hash_key(key) >> 56; }
// Views and carts need only two words beyond their IDs. Purchases and unusual
// date ranges point into their parser worker's exact, immutable detail array.
constexpr uint32_t detailed = 0x80000000u;
constexpr int64_t seconds_bias = int64_t(1) << 30;
struct PackedEvent { uint32_t when, tag; };
struct Detail { Time ingest; Event event; };
struct RawEvent { Key id, person; PackedEvent event; };
static_assert(sizeof(RawEvent) == 24);
template<class T> struct Owned { Key person; T value; };
using OwnedEvent = Owned<PackedEvent>;
using OwnedClick = Owned<Click>;
struct Worker {
    uint32_t number;
    std::vector<Detail> details;
    std::array<std::vector<RawEvent>, partitions> parsed;
    std::array<std::vector<Segment>, partitions> segments;
    std::array<std::vector<OwnedEvent>, partitions> events;
    std::array<std::vector<OwnedClick>, partitions> clicks;
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

struct Pipeline {
    Window window;
    Dimensions dim;
    Keys events{'e'}, users{'u'}, click_keys{'k'};
    People people;
    struct RawStore { std::vector<Event> events; std::vector<Click> clicks; };
    std::array<RawStore, partitions> stores;
    std::vector<Worker> workers;
    using Timer = std::chrono::steady_clock;
    Timer::time_point last = Timer::now();
    void mark(const char* stage) {
        auto now = Timer::now();
        if (getenv("PROFILE")) std::cerr << stage << ' ' << std::chrono::duration<double>(now-last).count() << " s\n";
        last = now;
    }
    Pipeline(const fs::path& data, size_t n) : window(data / "manifest.json"), dim(data) {
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) workers.emplace_back(uint32_t(i), window, dim, click_keys);
    }
    void parse_events(Chunk chunk, Worker& w) {
        read_rows<7>(chunk, [&](const auto& f) {
            Key id = events.encode(f[0]);
            if (!id) return;
            // Ingest is guaranteed F1. Retain a tombstone for any invalid earliest copy.
            if (f[3].size() != 20) return;
            Time ingest = window.clock.iso<true>(f[3]);
            Time ts = window.clock.parse(f[2]);
            uint32_t price = 0, category = 0;
            bool valid = !f[1].empty() && !f[5].empty() && ts >= window.start && ts < window.end && ingest - ts <= day_ms;
            if (valid && f[4] == "purchase") {
                int64_t p = decimal(f[6], 10'000'000);
                valid = p > 0;
                if (valid) {
                    price = uint32_t(p);
                    category = dim.products.get(f[5]);
                    if (!category) category = dim.unknown;
                }
            } else if (valid) valid = f[4] == "view" || f[4] == "cart";
            uint64_t when = valid ? uint64_t(ts - window.start) : 0;
            int64_t seconds = (ingest - window.start) / 1000 + seconds_bias;
            PackedEvent event;
            if (!price && when <= UINT32_MAX && uint64_t(seconds) < detailed) {
                event = {uint32_t(when), uint32_t(seconds)};
            } else {
                event = {uint32_t(w.details.size()), detailed | w.number};
                w.details.push_back({ingest, {ts, price, category}});
            }
            w.parsed[partition(id)].push_back({id, valid ? users.encode(f[1]) : 0, event});
        });
    }
    void parse_clicks(Chunk chunk, Worker& w) {
        read_rows<4>(chunk, [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            uint32_t campaign = dim.campaigns.get(f[2]);
            if (!campaign) return;
            Time ts = window.clock.parse(f[3]);
            if (ts < window.start - week_ms || ts >= window.end) return;
            Key raw_user = users.encode(f[1]);
            w.clicks[partition(raw_user)].push_back({raw_user, {ts, click_keys.encode(f[0]), campaign}});
        });
    }
    void read(const fs::path& data) {
        CsvFiles ev(data / "events"), ck(data / "clicks"), links(data / "links");
        for (auto& w : workers) {
            size_t hint = ev.bytes / (74 * workers.size() * partitions);
            for (auto& v : w.parsed) v.reserve(hint);
            for (auto& v : w.clicks) v.reserve(ck.bytes / (48 * workers.size() * partitions));
        }
        size_t click_start = 1 + ev.chunks.size();
        parallel(workers.size(), click_start + ck.chunks.size(), [&](size_t wi, size_t job) {
            if (job == 0) people.build(links, users);
            else if (job < click_start) parse_events(ev.chunks[job - 1], workers[wi]);
            else parse_clicks(ck.chunks[job - click_start], workers[wi]);
        });
    }
    Time ingestion(PackedEvent e) const {
        if (e.tag & detailed) return workers[e.tag ^ detailed].details[e.when].ingest;
        return window.start + (int64_t(e.tag) - seconds_bias) * 1000;
    }
    Event unpack(PackedEvent e) const {
        if (e.tag & detailed) return workers[e.tag ^ detailed].details[e.when].event;
        return {window.start + e.when, 0, 0};
    }
    void dedup(size_t p, Worker& dst) {
        size_t size = 0;
        for (auto& w : workers) size += w.parsed[p].size();
        if (!size) return;
        // A hash collision only selects a key for an exact comparison. A lone bit
        // can bypass the full table. Both passes read the same contiguous shard.
        struct Word { uint64_t seen = 0, repeated = 0; };
        size_t words = std::bit_ceil(std::max<size_t>(1, (size + 3) / 4));
        std::vector<Word> filter(words);
        const size_t mask = words * 64 - 1;
        for (auto& w : workers) for (const auto& row : w.parsed[p]) {
            size_t h = hash_key(row.id) & mask;
            uint64_t bit = uint64_t(1) << (h & 63);
            auto& word = filter[h >> 6];
            word.repeated |= word.seen & bit;
            word.seen |= bit;
        }
        FlatMap<RawEvent*> best(size / 10);
        auto emit = [&](const RawEvent& row) {
            if (row.person) {
                dst.events[partition(row.person)].push_back({row.person, row.event});
            }
        };
        for (auto& w : workers) for (auto& row : w.parsed[p]) {
            size_t h = hash_key(row.id) & mask;
            if (!(filter[h >> 6].repeated & (uint64_t(1) << (h & 63)))) {
                emit(row);
                continue;
            }
            auto [prev, fresh] = best.add(row.id, &row);
            if (!fresh && ingestion(row.event) < ingestion(prev->event)) prev = &row;
        }
        for (const auto& slot : best.slots()) if (slot.key) emit(*slot.value);
        for (auto& w : workers) std::vector<RawEvent>().swap(w.parsed[p]);
    }
    void group_aliases(size_t p, Worker& dst) {
        struct Group { Key key; size_t events = 0, clicks = 0; };
        size_t ne = 0;
        for (auto& w : workers) ne += w.events[p].size();
        FlatMap<uint32_t> index(ne / 20);
        std::vector<Group> groups;
        for (auto& w : workers) for (auto& e : w.events[p]) {
            auto [i, fresh] = index.add(e.person, uint32_t(groups.size()));
            if (fresh) groups.push_back({e.person});
            ++groups[i].events;
            e.person = i;
        }
        for (auto& w : workers) for (auto& c : w.clicks[p]) {
            auto [i, fresh] = index.add(c.person, uint32_t(groups.size()));
            if (fresh) groups.push_back({c.person});
            ++groups[i].clicks;
            c.person = i;
        }
        size_t en = 0, cn = 0;
        for (auto& g : groups) {
            size_t next_e = en + g.events, next_c = cn + g.clicks;
            g.events = en; g.clicks = cn;
            en = next_e; cn = next_c;
        }
        // Scatter once by raw ID. These arrays stay in place through the canonical
        // shopper pass, so joining aliases moves only range descriptors.
        auto& ev = stores[p].events;
        auto& ck = stores[p].clicks;
        ev.resize(en);
        ck.resize(cn);
        for (auto& w : workers) {
            for (auto& e : w.events[p]) ev[groups[e.person].events++] = unpack(e.value);
            for (auto& c : w.clicks[p]) ck[groups[c.person].clicks++] = c.value;
            std::vector<OwnedEvent>().swap(w.events[p]);
            std::vector<OwnedClick>().swap(w.clicks[p]);
        }
        size_t e0 = 0, c0 = 0;
        for (auto& g : groups) {
            // A click-only alias may share a person with an event in another shard.
            Key canonical = people.linked_name(g.key);
            Segment seg{g.events != e0 ? ev.data() + e0 : nullptr, g.clicks != c0 ? ck.data() + c0 : nullptr,
                        uint32_t(g.events-e0), uint32_t(g.clicks-c0), canonical};
            if (canonical) dst.segments[partition(canonical)].push_back(seg);
            else dst.shopper.run(g.key, &seg, dst.days, dst.campaigns);
            e0 = g.events; c0 = g.clicks;
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
    void run(const fs::path& data, const fs::path& output) {
        read(data); mark("parse and link");
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { dedup(p, workers[wi]); });
        mark("deduplicate and partition");
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { group_aliases(p, workers[wi]); });
        parallel(workers.size(), partitions, [&](size_t wi, size_t p) { join_aliases(p, workers[wi]); });
        mark("sessions and attribution");
        fs::create_directories(output / "user_daily");
        fs::create_directories(output / "campaign_daily");
        parallel(workers.size(), workers.size() + 1, [&](size_t, size_t part) {
            if (part < workers.size()) {
                if (part == 0 || !workers[part].days.empty())
                    write_users(output / "user_daily" / ("part-" + std::to_string(part) + ".parquet"), workers[part].days, users, dim);
            } else {
                CampaignDays total(window, dim.names.size());
                for (auto& w : workers) w.campaigns.each([&](uint32_t c, int32_t day, const Totals& from) {
                    auto& t = total.at(c, day);
                    t.sessions += from.sessions; t.purchasing += from.purchasing; t.revenue += from.revenue; t.assists += from.assists;
                });
                write_campaigns(output / "campaign_daily" / "part-0.parquet", total, dim);
            }
        });
        mark("write parquet");
    }
};
} // namespace shop

int main(int argc, char** argv) {
    try {
        auto env = [](const char* key, const char* fallback) { auto value = getenv(key); return value ? value : fallback; };
        shop::fs::path data = argc > 1 ? argv[1] : env("DATA_DIR", "/data");
        shop::fs::path output = argc > 2 ? argv[2] : env("OUTPUT_DIR", "/output");
        size_t threads = std::clamp(std::stoi(env("NCPU", "8")), 1, 64);
        shop::Pipeline pipeline(data, threads);
        pipeline.run(data, output);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
