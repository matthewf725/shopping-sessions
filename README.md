# Shopping Sessions

A C++20 pipeline for the [Builders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The Docker image runs offline and writes both daily tables as Parquet. Build it with `docker build --platform linux/amd64 -t shopping-sessions .`. Mount the input at `/data` and an empty writable directory at `/output`.

## Algorithm

1. One worker builds the identity graph from the links while the other workers parse CSV chunks. The reader validates separators and quotes 64 bytes at a time. It locates the fields needed for each event and defers the product value and price until a purchase passes the checks on its other fields. Event records usually take 24 bytes. Purchases and timestamps outside the compact range have an exact side record.
2. Partition events by exact event ID and retain the earliest ingestion time, including invalid copies. A bitmap sends potential duplicates to an exact hash table. Group the winners and the valid clicks by raw user ID. Each distinct raw ID then needs one identity lookup. A linked shopper's event and click ranges stay in place. The session builder reads them through a list of ranges.
3. After deduplication and identity linking, each two-hour slot for a shopper starts with one connected interval. An event inside it or within 30 minutes of either endpoint extends that interval. A farther event triggers refinement of just that slot into at most four half-hour cells. The previous interval retains its counts and purchases, and later events can bridge a gap. A bitmap visits the resulting intervals in time order and compares their endpoints across slot boundaries. Very wide windows use four radix passes over occupied half-hour cell numbers.
4. Session starts are more than 30 minutes apart. A bitmap and prefix counts find the first start eligible for each click. Keep the most recent click at that start, breaking ties by ID, then carry the winner forward until expiration. For assists, group a shopper's clicks by campaign and seven-day bucket. All windows in a bucket overlap, so the earliest eligible session and the latest click determine their exact union. Only adjacent buckets can overlap. Clip each range at the following bucket's first eligible session and count each UTC day's share by subtracting session positions. Identical clicks preserve both endpoints.
5. Workers write independent user Parquet files and reduce campaign totals. Apache Arrow's C++ Parquet writer (21.0.0) is the output library. The CSV scanner, deduplication, session construction and attribution code were written for this track.

For the 14-day input window, each event takes expected constant work. The allowed click dates occupy three seven-day buckets including the pre-window lookback. Given `c` clicks, `b` occupied shopper/campaign/week groups and `d` covered group/day intersections after clipping, assist aggregation takes `O(c + b + d)` work. Shuffling the rows preserves the result.

## Verification

`scripts/bench.sh 20000000 19 3` builds the image, runs the organizers' 36 fixtures and exact million-event comparison, then times three fresh containers on a generated practice-size input. Pass `60000000` for final size. The script uses the organizers' public Rust generator and checker, and requires Linux, Docker, Cargo, Python with Polars, curl and unzip. The default limits are eight vCPUs and 24 GB. Set `CPUSET` to the assigned logical CPU list on an N2 host to pin the run. Reported durations use Docker’s container start and exit timestamps.

For a native build, run `make ARROW_PREFIX=/path/to/arrow`. Run `./pipeline INPUT EMPTY_OUTPUT` with `NCPU` set to the desired thread count. Set `PROFILE=1` for stage timings on stderr.

`python tests/differential.py ./pipeline --generator /path/to/generate` compares the pipeline with an independent chronological reference on six generated seeds and four crafted cases. They cover overlapping, touching and disjoint click windows, unusual IDs, both dense and sparse week storage, and records spanning multiple file chunks.

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

### Field selection on fresh input

The scored reader builds a comma bitmap and prefix counts as it reads each 8 KiB tile. Two nine-byte IDs and their separators fit in the first 24 bytes even when both IDs are quoted. Exactly two commas in those bytes let the reader locate the ID and timestamp fields with two independent bitmap loads. For wider fields it can binary-search the prefix counts to locate a separator. A UTC event timestamp with the same date prefix as its ingestion time reuses the already known midnight.

A view or cart needs a nonempty product. Its first one or two bytes settle that check. Only an otherwise eligible purchase needs the final comma, product value and price. Every physical line still passes the structural check, including quote errors in unused fields. On both generated datasets every syntactically valid event row used the bitmap reader.

| 20M-event seed | Valid CSV event rows | Rows skipping product value and price | Parse and link, before → after | Start to exit, before → after |
| --- | ---: | ---: | ---: | ---: |
| 211 | 21,425,622 | 20,794,189 (97.05%) | 185.57 → 165.60 ms | 386.52 → 372.11 ms |
| 719 | 21,463,092 | 20,830,116 (97.05%) | 189.52 → 168.57 ms | 405.13 → 381.35 ms |

Times are medians of 14 interleaved fresh processes per build and seed after warm-up, using eight M4 Max workers and RAM-backed files with 20M generated events and 5M clicks. Both output tables matched the baseline `d4a5099` on both seeds. The measured change includes an ARM NEON mask reduction. The Xeon build uses AVX-512BW for masks and BMI2 for bit selection.

On seed 719, a separate eight-worker measurement that only validated CSV structure and counted valid lines took a median of 65.42 ms over 14 warm runs. It covered 21,463,092 valid event rows, 5,100,253 click rows and 888,405 link rows. No field values entered that count.

Under Rosetta, an eight-run comparison on seed 719 using x86-64-v2 with PCLMUL took a median of 738.18 ms before and 743.56 ms after, start to exit. Both builds produced identical output. The native Xeon AVX-512 time for this revision is unmeasured.

### Global structure across shuffled parts

The generator assigns sessions to two-hour slots before shuffling the files. The pipeline checks connectivity using the timestamps actually received for each linked shopper. If the earliest and latest event in a slot are at most 30 minutes apart, all events in it are connected for every input order. Comparing the previous occupied slot's last time with the next one's first time always gives the exact gap between slots. Any missed session start must lie inside a slot whose span exceeds 30 minutes. [Organizer's generator](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/generator.zip)

| 20M-event seed | Nonempty shopper–slot groups | Groups certified by the two endpoints | Groups requiring a closer look by this test | Additional starts inside those groups |
| --- | ---: | ---: | ---: | ---: |
| 211 | 3,465,787 | 3,449,752 | 16,035 | 7,955 |
| 307 | 3,462,225 | 3,446,092 | 16,133 | 8,055 |

Twelve interleaved warm runs per build on the eight-worker M4 Max gave median process times of 390.09 ms before and 390.08 ms after these changes on seed 211, and 385.17 ms before and 375.41 ms after on seed 307. The baseline was `04ec7ca`. An earlier nine-run comparison on seed 307 measured 392.09 ms before and 394.52 ms after. Both practice-size datasets produced exactly the same output rows as the baseline.

The CSV reader also retains the complete lines preceding a quote error. On seed 211 the previous tile fallback sent 383,568 event rows to the strict reader after a vector quote check failed. The new check isolated 3,623 malformed physical lines and resumed at the next newline. Both readers accepted the same 21,425,622 syntactically valid event rows.

### Campaign assists

These are pooled medians from two interleaved batches, 24 warm fresh processes per build per seed, on the M4 Max with eight workers and RAM-backed input and output. Both tables matched the baseline `131243f` row for row on both seeds. The session and attribution stage improved on each seed. Whole-process medians moved in opposite directions.

| 20M-event seed | Individual assists | Day-total updates | Baseline sessions and attribution | Grouped sessions and attribution | Baseline start to exit | Grouped start to exit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 211 | 36,122,537 | 11,312,487 | 92.82 ms | 89.45 ms | 391.64 ms | 394.11 ms |
| 307 | 37,093,335 | 11,667,852 | 94.44 ms | 91.11 ms | 389.51 ms | 387.49 ms |

### Queries on CSV row patterns

The standalone `scripts/shape_index.cpp` indexes the ID-width and UTC-date conditions above. One pass in filename order numbers the physical data rows from zero. For each 64 rows it stores a 64-bit exception mask and the number of exceptions before that word. Two prefix counts give the exact number of exceptions in any half-open range in `O(1)`. Count and selection queries store 16 bytes of directory entries per 64 rows plus one sentinel.

To select the zero-based `k`th exception, the range's exact density predicts a row. The word's prefix counts tell the search which way to move. It doubles the jump distance until the counts bracket the target, binary-searches those words and selects the bit in the final word. For a prediction off by `d` words, the query takes `O(log(2 + d))` work under any arrangement of the input. A poor prediction changes the number of probes while preserving the answer.

Seed 211 had 21,434,677 physical rows and 3,244,877 exceptions. The index returned identical answers to the explicitly stored exception list for 20,006 counts and 20,010 selections. The millionth exception was at row 6,604,305, 21 words from its predicted location. Across the sampled nonempty ranges the median error was 21 words and the 99th percentile was 100 words.

Build with `c++ -O3 -march=native -std=c++20 -Isrc scripts/shape_index.cpp -o /tmp/shape-index`. Run `/tmp/shape-index DATA` and enter `first stop` to count exceptions. With `--select`, enter `first stop k` to get the physical row number or `none`. With `--list`, enter `first stop` to print the original row, filename and byte-offset listing. Run `python3 tests/shape_index.py /tmp/shape-index` to check shuffled and clustered layouts. The timed pipeline reads its own fresh CSV input directly.
