# Shopping Sessions

A C++20 pipeline for the [Builders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The Docker image runs offline and writes both daily tables as Parquet. Build it with `docker build --platform linux/amd64 -t shopping-sessions .`. Mount the input at `/data` and an empty writable directory at `/output`.

## Algorithm

1. One worker builds the identity graph from the links while the other workers parse CSV chunks. The AVX-512BW build for Cascade Lake scans 64 bytes at a time, checks quote parity at separators and decodes the four date and time pairs in parallel. Event records usually take 24 bytes. Purchases and timestamps outside the compact range have an exact side record.
2. Partition events by exact event ID and retain the earliest ingestion time, including invalid copies. A bitmap sends potential duplicates to an exact hash table. Group the winners and the valid clicks by raw user ID. Each distinct raw ID then needs one identity lookup. A linked shopper's event and click ranges stay in place. The session builder reads them through a list of ranges.
3. After deduplication and identity linking, each two-hour slot for a shopper starts with one connected interval. An event inside it or within 30 minutes of either endpoint extends that interval. A farther event triggers refinement of just that slot into at most four half-hour cells. The previous interval retains its counts and purchases, and later events can bridge a gap. A bitmap visits the resulting intervals in time order and compares their endpoints across slot boundaries. Very wide windows use four radix passes over occupied half-hour cell numbers.
4. Session starts are more than 30 minutes apart. A bitmap and prefix counts map a click's seven-day interval to a range of sessions. Keep the most recent click first eligible at each session, breaking ties by ID, then carry the winner forward until expiration. OR the eligible range into a campaign bitset to count each assist once. Identical click copies give identical winners and set the same bits.
5. Workers write independent user Parquet files and reduce campaign totals. Apache Arrow's C++ Parquet writer (21.0.0) is the output library. The CSV scanner, deduplication, session construction and attribution code were written for this track.

For the 14-day input window, each event takes expected constant work. A seven-day click range spans at most 337 session starts and seven 64-bit words. Input row order has no effect on the result. No event or click comparison sort is used.

## Verification

`scripts/bench.sh 20000000 19 3` builds the image, runs the organizers' 36 fixtures and exact million-event comparison, then times three fresh containers on a generated practice-size input. Pass `60000000` for final size. The script uses the organizers' public Rust generator and checker, and requires Linux, Docker, Cargo, Python with Polars, curl and unzip. The default limits are eight vCPUs and 24 GB. Set `CPUSET` to the assigned logical CPU list on an N2 host to pin the run. Reported durations use Docker’s container start and exit timestamps.

For a native build, run `make ARROW_PREFIX=/path/to/arrow`. Run `./pipeline INPUT EMPTY_OUTPUT` with `NCPU` set to the desired thread count. Set `PROFILE=1` for stage timings on stderr.

`python tests/differential.py ./pipeline --generator /path/to/generate` compares the pipeline with an independent chronological reference on six generated seeds and three crafted cases covering multiword click ranges, unusual IDs, a wide calendar window and records spanning multiple file chunks.

## Measurements

The official Xeon practice score for commit `ef41f89` was **1.52 s** for team Uhhh. [Builders leaderboard](https://www.joinbuilders.org/buildathon/leaderboard)

On an M4 Max with eight native worker threads and RAM-backed input and output, the raw-user grouping reduced median end-to-end time from 0.494 s to 0.454 s over five interleaved warm runs per variant on seed 19 (20M events, 5M clicks). The deduplication and repartitioning stage fell from 107 ms to 31 ms. The additional work to join alias ranges raised the following stage from 78 ms to 95 ms. Both variants produced identical rows.

On a second practice-size dataset (seed 71), the `xeon-tuning` build took 0.575 s on first access and 0.424–0.437 s on the following seven fresh-process runs. On a final-size dataset (seed 47, 60M events and 15M clicks), its three runs took 2.379 s, 1.248 s and 1.307 s. All 6,684,549 user rows and 28,014 campaign rows matched the scored implementation exactly.

### CSV structure

Across two datasets generated with 20M events and 5M clicks, 84.32% of physical event rows on seed 83 and 84.20% on seed 157 had standard nine-byte event and raw user IDs and an in-window event timestamp on the same UTC date as ingestion. Their average consecutive runs were 6.38 and 6.33 rows. Counting complete, non-overlapping 64-row blocks within files, just 8 of 326,344 blocks on seed 83 and 9 of 330,237 on seed 157 matched throughout.

The reader uses a single eight-byte load for standard eight- and nine-byte IDs. It caches UTC midnights and packs the six clock digits of valid ingestion timestamps in chronological order for exact deduplication. Same-day arrivals immediately satisfy the lateness rule. For next-day arrivals the reader checks the precise 24-hour boundary, including milliseconds in the event timestamp.

These are medians from nine interleaved warm, fresh-process runs of each build on an M4 Max with eight workers and RAM-backed input and output. The baseline is `4f7d3e7`. Both builds produced identical tables on both seeds.

| Seed | Baseline parse and link | Tuned parse and link | Baseline start to exit | Tuned start to exit |
| --- | ---: | ---: | ---: | ---: |
| 83 | 210.7 ms | 199.3 ms | 422.2 ms | 421.7 ms |
| 157 | 194.7 ms | 193.0 ms | 410.3 ms | 409.0 ms |

### Global structure across shuffled parts

The generator assigns sessions to two-hour slots before shuffling the files. The pipeline checks connectivity using the timestamps actually received for each linked shopper. If the earliest and latest event in a slot are at most 30 minutes apart, all events in it are connected for every input order. Comparing the previous occupied slot's last time with the next one's first time always gives the exact gap between slots. Any missed session start must lie inside a slot whose span exceeds 30 minutes. [Organizer's generator](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/generator.zip)

| 20M-event seed | Nonempty shopper–slot groups | Groups certified by the two endpoints | Groups requiring a closer look by this test | Additional starts inside those groups |
| --- | ---: | ---: | ---: | ---: |
| 211 | 3,465,787 | 3,449,752 | 16,035 | 7,955 |
| 307 | 3,462,225 | 3,446,092 | 16,133 | 8,055 |

Twelve interleaved warm runs per build on the eight-worker M4 Max gave median process times of 390.09 ms before and 390.08 ms after these changes on seed 211, and 385.17 ms before and 375.41 ms after on seed 307. The baseline was `04ec7ca`. An earlier nine-run comparison on seed 307 measured 392.09 ms before and 394.52 ms after. Both practice-size datasets produced exactly the same output rows as the baseline.

The CSV reader also retains the complete lines preceding a quote error. On seed 211 the previous tile fallback sent 383,568 event rows to the strict reader after a vector quote check failed. The new check isolated 3,623 malformed physical lines and resumed at the next newline. Both readers accepted the same 21,425,622 syntactically valid event rows.

The standalone `scripts/shape_index.cpp` indexes the two predicted row properties described above. It scans the event files once in filename order, counting physical data rows from zero and recording a file and byte offset for each exception. A query binary-searches the sorted list of exceptions and visits only the entries in the requested half-open range. With `H` exceptions in the index and `h` in the range, the query takes `O(log(1 + H) + h)` work. Building the index reads all the event bytes. On seed 211, 20,000 range queries checked against a row-by-row oracle needed at most 22 comparisons to locate their first candidate among 3,244,877 exceptions.

Build the diagnostic with `c++ -O3 -std=c++20 -Isrc scripts/shape_index.cpp -o /tmp/shape-index`. Run `/tmp/shape-index /path/to/data` and enter pairs such as `1000 1250` on standard input. Each result gives the physical row number, part filename and byte offset. The timed pipeline processes all events directly.
