# Shopping Sessions

A C++20 pipeline for the [Builders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The Docker image runs offline and writes both daily tables as Parquet. Build it with `docker build --platform linux/amd64 -t shopping-sessions .`. Mount the input at `/data` and an empty writable directory at `/output`.

## Algorithm

1. One worker builds the identity graph from the links while the other workers parse CSV chunks. The AVX-512 reader checks quoting across eight 64-byte words at a time and locates the first malformed row when a block fails validation. On AVX-512 it indexes field boundaries and checks IDs, event types, UTC dates and epoch timestamps for eight events together. The Xeon reader indexes 32 KiB at a time and masks inactive lanes in a partial batch. Rows with exact IDs and a recognized ingestion day retain those values while checking a purchase or another timestamp format. The reader can apply a signed offset to the known date after checking the sign, punctuation and every clock digit. The click reader checks eight campaign IDs against the input catalog and decodes their timestamps together, including the seven-day lookback. Product lookup and price conversion happen only for purchases that pass the earlier checks. Event records usually take 24 bytes. Purchases and timestamps outside the compact range have an exact side record.
2. Partition events into 256 shards by exact event ID and retain the earliest ingestion time, including invalid copies. Each worker appends records to 128-record blocks in 8 MiB slabs. A bitmap checks eight IDs together and routes potential duplicates to an exact hash table. Updates to the same bitmap word run in input order. Group the winners and valid clicks by raw user ID. A row keeps a pointer to its group. Eight independent rows can reserve slots and write directly into their groups’ output slices. When several lanes name the same group, later lanes use the updated write position. Once deduplication has finished, those contiguous slices reuse the 8 MiB slabs that held the parsed events. Each distinct raw ID then needs one identity lookup. A linked shopper's event and click ranges stay in place. The session builder reads them through a list of ranges.
3. Gather each shopper’s events into occupied half-hour cells. Events in one cell always belong to the same session, so its minimum timestamp, maximum timestamp and count represent all its views and carts. Keep purchases by category. Visit occupied cells in chronological order and start a session when the next minimum exceeds the previous maximum by more than 30 minutes. Very wide windows use four radix passes over occupied cell numbers.
4. Session starts are more than 30 minutes apart. A bitmap and prefix counts find the first start eligible for each click. Keep the most recent click at that start, breaking ties by ID, then carry the winner forward until expiration. For assists, group a shopper's clicks by campaign and seven-day bucket. All windows in a bucket overlap, so the earliest eligible session and the latest click determine their exact union. Only adjacent buckets can overlap. Clip each range at the following bucket's first eligible session and count each UTC day's share by subtracting session positions. Identical clicks preserve both endpoints.
5. Workers write independent user Parquet files and reduce campaign totals. The C++ writer emits Parquet data pages with string and date annotations, dictionary-encodes repeated shopper IDs and delta-encodes the four numeric shopper columns. It packs the optional category's definition levels and bounds row groups by the longest string in the input. [Parquet encodings](https://parquet.apache.org/docs/file-format/data-pages/encodings/)

For the 14-day input window, each event takes expected constant work. The allowed click dates occupy three seven-day buckets including the pre-window lookback. Given `c` clicks, `b` occupied shopper/campaign/week groups and `d` covered group/day intersections after clipping, assist aggregation takes `O(c + b + d)` work. Shuffling the rows preserves the result.

## Verification

`scripts/bench.sh 20000000 19 3` builds the image, runs the organizers' 36 fixtures and exact million-event comparison, then times three fresh containers on a generated practice-size input. Pass `60000000` for final size. The script uses the organizers' public Rust generator and checker, and requires Linux, Docker, Cargo, Python with Polars, curl and unzip. The default limits are eight vCPUs and 24 GB. Set `CPUSET` to the assigned logical CPU list on an N2 host to pin the run. Reported durations use Docker’s container start and exit timestamps.

The Xeon image uses GCC 12 profile-guided optimization. The two files in `profiles/` contain compiler counters from the public million-event development input. On a Linux AVX-512 host with Debian bookworm GCC 12, `scripts/train-xeon-profile.sh /path/to/dev` regenerates them and verifies that the compiler accepts them. The image entrypoint selects the optimized Linux binary for the available CPU.

For a native build, run `make`. Run `./pipeline INPUT EMPTY_OUTPUT` with `NCPU` set to the desired thread count. Set `PROFILE=1` for stage timings on stderr. Run `python3 scripts/profile.py ./pipeline INPUT EMPTY_OUTPUT` to include process launch and exit. The parent and child use the same monotonic clock.

On Apple silicon, the local Linux simulator executes the AVX-512 build, checks Parquet results and counts guest instructions. A separate LLVM model estimates Cascade Lake or Ice Lake throughput for a compiled loop. [Local simulation and calibration](scripts/xeon-sim/README.md)

`python tests/differential.py ./pipeline --generator /path/to/generate` compares the pipeline with an independent chronological reference on six generated seeds and four crafted cases. They cover overlapping, touching and disjoint click windows, unusual IDs, both dense and sparse week storage, and records spanning multiple file chunks. The chunk-boundary case also forces a file-size error and retries in the same output directory.

## Measurements

Team Uhhh scored **0.87 s** on the official practice scorer for commit `488baf16a48ebc8c708277c6fc898e3ce2286c32`, receipt `BB-38195FEC`. Builders selected it as the team's best entry for the final. Earlier scores were 0.93 s for `d744498`, 1.12 s for `1018e64` and 1.52 s for `ef41f89`. [Builders leaderboard](https://www.joinbuilders.org/buildathon/leaderboard)

The Xeon build that batches shard writes and uses half-hour cells matched both reference tables on two 20M-event, 5M-click inputs. Its three fresh-process runs per seed retired a median of 12.52 billion instructions on seed 719 and 12.41 billion on seed 11971. Median simulated ticks were 1.727 billion and 1.707 billion. Scaling each run against the 1.12 s submission on the same input gives 0.485–0.497 s and 0.469–0.476 s respectively. The official 0.93 s result was about 1.9 times the median estimate. These instruction-based ratios overstated the native speedup.

On two fresh practice-size inputs the Linux AVX-512 simulator ran the scored binary, the new CSV and deduplication paths, and a profile-guided build. The profile-guided binary retired 16.81 billion guest instructions on seed 719 and 16.69 billion on seed 11971. The scored binary retired 28.91 billion and 29.52 billion respectively. Both tables matched on both inputs, including 2,251,704 and 2,237,708 user-day rows. These counts include Linux kernel work. The measured native Xeon score above belongs to the original binary.

Reusing the released event slabs for grouped events and clicks reduced minor page faults on seed 11971 from roughly 247,000 to 70,000 per process. With profile-guided compilation, this version took 1.975 billion simulated ticks on seed 719 and 1.955 billion on seed 11971. Each value is the median of two runs. Both Parquet tables matched the corresponding practice-size reference. The official 1.12 s binary took 3.924 and 4.046 billion simulated ticks on the same inputs.

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

### Process exit and campaign attribution

On seed 719, the baseline took 22.31 ms from launch to entering `main` and 3.07 ms to finish setup. After the last Parquet write, destroying the pipeline took 34.76 ms. The remaining time from leaving `main` until the parent observed exit was 16.50 ms. These are separately measured warm medians. Two first invocations immediately after linking took 183 ms and 569 ms to enter `main` on this Mac.

Each writer now closes the Parquet footer and checks the output stream's close status. Once every writer has returned and every worker has joined, a successful process exits directly so the OS can reclaim its private scratch memory. Errors still unwind and produce a nonzero exit. Attribution walks a list of occupied shopper-days and updates contiguous daily assist counters for each campaign.

A separate diagnostic on seed 719 sampled one in 32 small shoppers and timed every shopper with at least 256 events or 128 clicks. Across all workers it estimated 129 ms collecting events into time buckets, 61 ms forming sessions and 203 ms attributing campaigns. Those worker times overlap. The enclosing stage also groups and scatters records by shopper.

| Generated events / clicks | Seed | Last output close to observed exit, before → after | Sessions and attribution, before → after | Launch to observed exit, before → after |
| --- | ---: | ---: | ---: | ---: |
| 20M / 5M | 211 | 49.01 → 19.58 ms | 92.61 → 88.42 ms | 375.12 → 342.18 ms |
| 20M / 5M | 719 | 51.07 → 19.77 ms | 90.79 → 88.67 ms | 375.34 → 341.82 ms |
| 60M / 15M | 47 | 119.23 → 47.59 ms | 301.07 → 287.95 ms | 1,162.59 → 1,064.18 ms |

The table pools two interleaved batches on an M4 Max with eight workers and RAM-backed files. Each build ran in 32 warm, fresh processes per practice seed and 19 at final size. End-to-end and session times use the unmodified baseline `4c18cb2`. A separately instrumented copy of that baseline records the output-close and destructor boundaries. The optimized build reports output close directly. On seed 719 the combined launch, setup and post-write time fell from 76.77 to 45.83 ms, calculated per run before taking medians. Both daily tables matched across all three datasets, including all 6,684,549 user-day rows at final size.

On the final-size input, the session stage took 8.66 ms longer in the first batch and 17.63 ms less in the second. The end-to-end gain appeared in both. A 512-shopper-partition experiment improved the first final-size comparison but took 5–15 ms longer end to end across the three inputs in the repeat. The build measured in that comparison used 256 partitions.

On seed 211 the translated x86-64-v2 fallback under Rosetta took 720.94 ms before and 720.36 ms after across nine interleaved runs per build. Its output matched the baseline. The native Xeon AVX-512 time for this change is unmeasured.

### Xeon timestamp decoding and deduplication

The Intel decoder evaluates 13-digit epoch timestamps in pairs and then groups of four and eight digits. The AVX-512 build loads exactly 13 bytes, so the field can end at a page boundary. Events with a known five-digit prefix retain the existing eight-digit path.

Event partitioning stores the full 64-bit hash in each record. The hash is a permutation, so distinct event IDs stay distinct. Two bits per slot record the first and subsequent observations in one word. Candidates sharing a slot go to the exact table, which retains the earliest ingestion for each full key.

Against `10198d3`, the x86-64-v2 fallback under Rosetta reduced median deduplication and repartitioning from 45.71 to 40.95 ms on practice seed 719 and from 42.54 to 38.53 ms on seed 211. Median launch-to-exit times moved from 755.11 to 747.25 ms and from 723.99 to 718.98 ms respectively. Each build ran 10 times on seed 719 and eight times on seed 211, with the builds interleaved, eight workers and RAM-backed files.

### Queries on CSV row patterns

The standalone `scripts/shape_index.cpp` indexes the ID-width and UTC-date conditions above. One pass in filename order numbers the physical data rows from zero. For each 64 rows it stores a 64-bit exception mask and the number of exceptions before that word. Two prefix counts give the exact number of exceptions in any half-open range in `O(1)`. Count and selection queries store 16 bytes of directory entries per 64 rows plus one sentinel.

To select the zero-based `k`th exception, the range's exact density predicts a row. The word's prefix counts tell the search which way to move. It doubles the jump distance until the counts bracket the target, binary-searches those words and selects the bit in the final word. For a prediction off by `d` words, the query takes `O(log(2 + d))` work under any arrangement of the input. A poor prediction changes the number of probes while preserving the answer.

Seed 211 had 21,434,677 physical rows and 3,244,877 exceptions. The index returned identical answers to the explicitly stored exception list for 20,006 counts and 20,010 selections. The millionth exception was at row 6,604,305, 21 words from its predicted location. Across the sampled nonempty ranges the median error was 21 words and the 99th percentile was 100 words.

Build with `c++ -O3 -march=native -std=c++20 -Isrc scripts/shape_index.cpp -o /tmp/shape-index`. Run `/tmp/shape-index DATA` and enter `first stop` to count exceptions. With `--select`, enter `first stop k` to get the physical row number or `none`. With `--list`, enter `first stop` to print the original row, filename and byte-offset listing. Run `python3 tests/shape_index.py /tmp/shape-index` to check shuffled and clustered layouts. The timed pipeline reads its own fresh CSV input directly.

## Comparison with the pinned Rust release

The native Linux comparison ran on an Apple M4 Max with eight guest CPUs, Debian Bookworm glibc, GCC 12.2 and Rust 1.91.1. The Rust build used `cargo build --release --locked` on the unmodified revision. Each process ran in a fresh offline OCI container as UID 1000. An external monotonic timer included starting the runtime, reading the inputs, writing both Parquet tables and exiting. Runs alternated on the same warm CSV files. [Pinned Rust source and author measurements](https://github.com/cherylaurelia/builders-hackathon/commit/e185a97fa39841175c75fbcbb1d6ac885289f09d)

| Public seed | Events and clicks | Runs per build | Scored C++ source | Buffered reads and link prefetch | Pinned Rust |
|---|---|---:|---:|---:|---:|
| 11971 | 20M and 5M | 9 | 0.3108 s | 0.2863 s | 0.2742 s |
| 20261121 | 20M and 5M | 9 | 0.2997 s | 0.2835 s | 0.2651 s |
| 47191 | 60M and 15M | 6 | 1.0285 s | 0.9407 s | 0.8681 s |

These are medians from the `warm-input-nine`, `second-seed-nine` and `final-six` series. The guest had 12 GiB for seeds 11971 and 47191 in these series and 8 GiB for 20261121. All timed outputs in the table matched the reference on every key and column, including nulls. The three inputs produced 2,237,708, 2,240,971 and 6,645,833 user-day rows respectively, and each produced 28,014 campaign-day rows. The checker distributes rows by a key hash, then sorts and compares the actual typed values within each partition. It also checks uniqueness of the reference keys. [Individual container times, stage times, faults, peak RSS and row counts](scripts/benchmarks/pinned-rust-comparison.csv)

Reading event and click files into reusable 512 KiB buffers reduced median peak RSS on seed 11971 from 2,589,000 to 1,289,676 KiB and minor page faults from 52,212 to 26,920. This comparison changes only the read path. Records crossing a read boundary extend through their terminating newline or EOF. The identity optimization stores union-find state in open-addressed slots and prefetches both endpoints of 32 links before joining them. The released C++ binary passed all 36 organizer fixtures and the published 1M answer. The Linux AVX-512 build also matched a brute-force oracle on long records, malformed quotes, transitive aliases and wide windows.

A separate implementation placed event payloads directly in shopper partitions and cleared losing copies after comparing their exact event IDs and ingestion order. On 11971 its median deduplication stage fell from 38.8 to 30.7 ms, parsing and linking rose from 136.4 to 148.4 ms, and the whole container moved from 0.3038 to 0.3249 s. On the final-size seed it took 0.9955 s against 1.0285 s for the original layout. [Direct routing control, applicable to the scored C++ revision](scripts/benchmarks/direct-routing.patch)

With eight workers restricted to four guest CPUs on seed 20261121, the Rust median was 0.5151 s and the buffered C++ median was 0.5254 s. The eight-CPU values for the same seed appear in the table above. The four-CPU control measures time-sharing on Arm. The Builders scorer uses pinned Xeon hyperthreads. The Rust author also reports a separate eight-vCPU AMD median of 0.327 s and a four-vCPU Intel VM result of 1.012 s. [Builders track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions) · [Author's measurements](https://github.com/cherylaurelia/builders-hackathon/commit/e185a97fa39841175c75fbcbb1d6ac885289f09d)

On an x86-64 Linux host, set `COMPARE_IMAGE` to a locally built reference image and run `CPUSET=0-7 COMPARE_IMAGE=your-reference-image scripts/bench.sh 20000000 11971 9`. The script checks the 1M answer for both images, warms the generated input, alternates the order of each pair, records Docker's start-to-exit interval and compares every pair of outputs. Set `CPUSET` to the CPUs assigned to the benchmark. The earlier Arm measurements used an external crun timer. The CSV retains slow runs and flags the exploratory final-size series affected by host memory pressure.

## Cache-sized partitions and reuse of output storage

The Xeon build divides event IDs and shoppers into 256 partitions each. Each worker keeps its own append buffers. Global duplicate resolution still selects the copy with the earliest ingestion timestamp for each full event ID before applying payload validation. In the output stage, each worker allocates 65,536-row blocks from its finished deduplication storage. Its Parquet writer takes a page from the completed event partitions. On standard inputs each block holds one row group. Larger strings can use smaller row groups and grow the page as necessary.

| 20M events and 5M clicks | Matched containers per build | Scored C++ source | Buffered reads and links, 64 partitions | 256 partitions | 256 and pooled output | Pinned Rust |
|---|---:|---:|---:|---:|---:|---:|
| Seed 11971, eight CPUs | 9 | 0.2972 s | 0.2795 s | 0.2619 s | 0.2547 s | 0.2741 s |
| Seed 20261121, eight CPUs | 9 | 0.2985 s | 0.2773 s | 0.2602 s | 0.2545 s | 0.2709 s |
| Seed 20261121, four CPUs | 6 | — | 0.5133 s | 0.4811 s | 0.4684 s | 0.5102 s |

Each row reports the median start-to-exit time of interleaved fresh OCI containers on Linux Arm, with eight workers and verified CPU affinity. On eight CPUs the six consecutive three-run medians for the pooled build were 0.2550, 0.2482 and 0.2635 s on seed 11971, and 0.2552, 0.2494 and 0.2573 s on seed 20261121. The six four-CPU runs ranged from 0.4574 to 0.4875 s. The scored C++ source is the source of team Uhhh's earlier 0.93 s Xeon entry, rebuilt with the same native Arm compiler as the candidate.

Independent controls on seed 11971 measured the effect of each partition count. Increasing event-ID partitions from 64 to 256 reduced the median deduplication stage from 40.3 to 26.1 ms with the shopper partition count fixed at 64. Increasing shopper partitions from 64 to 256 reduced the shopper stage from 111.2 to 93.8 ms with the event-ID count fixed at 64. The shopper stage includes grouping, session construction and campaign attribution. Both changes preserve all keys and every output column. [Raw measurements, binary hashes and exact row counts](scripts/benchmarks/pinned-rust-comparison.csv)

## Observed storage reads and smaller Parquet files

Each reader samples its own Linux block input counter across an actual read. A worker that encounters block I/O switches the shared work queue to groups of eight consecutive 512 KiB pieces. Other workers pick up the new scheduling policy on their next claim. The file offsets assigned to workers still cover the input exactly once under either policy. The read buffers extend across record boundaries. [Linux getrusage](https://man7.org/linux/man-pages/man2/getrusage.2.html)

The benchmark host verified all 461,359 input pages before each controlled run on public seed 20261121. It ran the programs as UID 1000 with the data mounted read-only. The new program recorded block I/O in all nine cold runs and in none of the nine warm runs. In this interleaved series, the cold median was 0.3498 s for the new C++ build, 0.3914 s for the previous pooled C++ build and 0.3691 s for the pinned Rust build. One cold C++ run took 0.7885 s. The input filesystem was ext4 over a RAM-backed virtual block device. A separate eight-MiB control showed why the program uses its own I/O counters. With all 2,048 pages evicted, a UID 1000 process saw all 2,048 pages reported resident by mincore on a root-owned file. The file owner's measurement saw zero. [Linux mincore implementation](https://github.com/torvalds/linux/blob/master/mm/mincore.c) · [Individual timings and cache observations](scripts/benchmarks/input-and-output-comparison.csv)

The user-day writer dictionary-encodes adjacent shopper IDs when enough values repeat. It writes all four numeric columns as DELTA_BINARY_PACKED blocks of 128 differences with four 32-value miniblocks. The campaign table retains its original encoding. On six paired runs with ext4 output, median output size fell from 79,713,630 to 28,951,630 bytes. Median container runtime for this isolated encoding change moved from 0.2623 to 0.2593 s. All columns matched the independent implementation. Polars and PyArrow also decoded the writer's minimum and maximum signed integers and the 32-, 128- and 65,536-value boundaries exactly. [Apache Parquet encoding specification](https://parquet.apache.org/docs/file-format/data-pages/encodings/)

| Warm input on Linux Arm | C++ with observed I/O and compact output | Previous pooled C++ | Pinned Rust |
|---|---:|---:|---:|
| 11971, eight pinned CPUs | 0.2597 s | 0.2549 s | 0.2705 s |
| 20261121, eight pinned CPUs | 0.2682 s | 0.2620 s | 0.2824 s |
| 11971, eight workers sharing four CPUs | 0.4865 s | 0.4738 s | 0.5143 s |
| 20261121, eight workers sharing four CPUs | 0.4912 s | 0.4861 s | 0.5293 s |

Each cell is a nine-container median and every output was checked. The four-CPU maximum for the new C++ build was 0.6108 s on seed 11971 and 0.5063 s on seed 20261121. One of its six consecutive three-run medians across these two four-CPU series exceeded half a second. Seed 11971 used tmpfs input, and seed 20261121 used warm ext4 input. [Individual container times](scripts/benchmarks/pinned-rust-comparison.csv)

### Parser selection on an AVX-512 CPU

The 0.87 s submission chooses its event and click readers independently. Each compares 12 completed chunks per reader using the worker's thread CPU time per input byte. When the sample window fills, subsequent chunks use the fastest median. Every sample contributes to the output. Its three readers decode eight rows together, decode individual rows from separator offsets or select fields from a separator bitmap. The native Arm measurements use the scalar reader.

On public seed 719, two x86 Linux simulation runs of the submitted executable chose the eight-row reader for both events and clicks. The output matched all 2,251,704 shopper-day rows and 28,014 campaign-day rows from both the previous submission and the pinned Rust implementation. The two-run medians were 11.75 billion simulated instructions and 1.519 billion simulated ticks for this build, versus 13.33 billion and 1.878 billion for the 0.93 s submission. The official scorer measured a 6.5% improvement. Builders withholds hidden stage timings and hardware counters. [All eight simulated runs](scripts/benchmarks/xeon-seed719-comparison.csv) · [Scoring rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

### Final-size public input

The native Linux Arm benchmark ran 60M events and 15M clicks from seed 47191 in fresh, CPU-pinned containers with eight workers. All 108 input files matched the SHA-256 manifest from the earlier run. All 24 outputs matched an independently checked reference on every column and key, covering 6,645,833 shopper-day rows and 28,014 campaign-day rows.

| Implementation | Runs | Median start to exit | Range | Median peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Submitted C++ source, `488baf1` | 6 | 1.226 s | 0.817–2.217 s | 3.38 GiB |
| C++ with pooled output, `1159fbe` | 6 | 1.243 s | 0.800–2.628 s | 3.37 GiB |
| Pinned Rust, `e185a97` | 6 | 0.963 s | 0.850–1.622 s | 2.07 GiB |
| Earlier official C++ source, `d744498` | 6 | 1.451 s | 0.992–2.321 s | 7.40 GiB |

For the submitted C++ source, the individual stage medians were 475 ms reading and linking, 148 ms selecting duplicates and partitioning, 331 ms grouping shoppers, building sessions and attributing campaigns, and 19 ms writing Parquet. Each median summarizes six runs. The table uses the median of the six whole-container times. [Per-run measurements](scripts/benchmarks/pinned-rust-comparison.csv)

### Reusing narrower records after deduplication

The tested layout stores each grouped event in eight bytes and each grouped click in 16 bytes. Event times are millisecond offsets from the reporting window. Validated purchases fit in 24 bits for the price and eight bits for the category. The code checks the actual window span and catalog size before choosing the representation. Wider windows and catalogs use the full-width layout. An earlier invalid copy still suppresses a later valid event across users.

| Fresh Linux Arm containers | Submitted C++ source | Narrow event and click records after deduplication | Pinned Rust |
| --- | ---: | ---: | ---: |
| 20M events and 5M clicks, 9 runs each | 0.2747 s | 0.2699 s | 0.2830 s |
| 60M events and 15M clicks, 5 runs each | 0.9300 s | 0.7897 s | 0.9041 s |

Every result matched the independent reference on every column and key. The savings were larger at final size. On the 60M input, median time grouping shoppers, forming sessions and attributing campaigns fell from 314.6 ms to 263.7 ms. [Individual measurements and ELF hashes](scripts/benchmarks/pinned-rust-comparison.csv)

In six interleaved 20M runs of translated Linux x86-64-v2 code, the 0.93 s submission took 0.4410 s median and the 0.87 s submission took 0.4179 s. This comparison uses the same portable instruction set for both versions. A second experiment under host memory pressure reduced duplicate selection from 79.6 ms to 37.4 ms with an index derived from actual event IDs. The whole process increased from 0.8735 s to 0.8846 s versus the same one-pass design with ordinary hashing. All five outputs from each design matched. [Run-level data](scripts/benchmarks/pinned-rust-comparison.csv)

### Runtime selection and output checks

This branch adds a fourth Xeon reader. When eight accepted records occupy consecutive positions in the separator table, it loads their column boundaries from consecutive cache lines. The distance between the first and last row indices proves when this is valid. The program samples this reader alongside the other three. It also measures scalar and eight-lane processing separately for the event-ID and raw-user partitions. With `PROFILE=1`, reader numbers 0 through 3 identify the eight-row gather, scalar offsets, scalar bitmap and consecutive-offset readers. The value 4 means sampling was still in progress. The two shard stages report 0 for vector processing, 1 for scalar and 2 while sampling.

After duplicate selection, a grouped event uses eight bytes and a click uses 16 when the event window plus click lookback fits a 32-bit millisecond offset and the category catalog fits eight bits. Other inputs use the full-width representation. Both layouts retain exact event times, purchase amounts and byte-wise click tie breaking.

Two clean Debian GCC 12 release builds produced the same AVX-512 ELF with SHA-256 `96032330869af72f96d430329f8e61b87aa1fe64acda2c40c0ae738340a33468`. That executable passed all 36 organizer fixtures, eight directed cases covering wide windows, catalog sizes and input boundaries, and exact comparisons on 1M and 2M events. The 1M comparison covered 117,038 shopper days and 27,400 campaign days. The 2M comparison covered 234,488 and 27,980. All keys and columns matched.

On seed 11971 with eight pinned CPUs, nine interleaved runs for each translated Linux x86-64-v2 binary gave medians of 0.4242 s for the submitted C++ source, 0.4150 s for narrow C++ records and 0.4044 s for the Rust reference. The Rust build used its original source, locked dependencies and release profile with the same portable CPU target. All 27 pairs of output tables matched. The author's Dockerfile defaults to x86-64-v3. [Pinned Rust source and Dockerfile](https://github.com/cherylaurelia/builders-hackathon/tree/e185a97fa39841175c75fbcbb1d6ac885289f09d) · [Per-process timing, stage boundaries and executable hashes](scripts/benchmarks/pinned-rust-comparison.csv)

On the same seed, changing only the eight-worker C++ process's Arm CPU affinity from eight CPUs to four moved its median from 0.2575 s to 0.4830 s. Four workers on four CPUs took 0.4541 s. Each result includes five fresh containers and full output checks. The official scorer assigns eight Xeon hardware threads. [Official container limits](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

### How input file state changes the local 20M time

On an M4 Max, the same eight-worker macOS executable read the same 1,877,554,817 input bytes and wrote the same two tables on every run. We touched one byte of every page of the original input through a memory mapping. For each trial, we gave the process a directory of links to those files or to fresh copies with identical contents. Copies retained the source files' HFS+ compression. We checked every copied file's SHA-256 after timing and compared every result to an independent reference covering 2,237,708 user-day rows and 28,014 campaign-day rows.

| Input bytes served by fresh copies | First whole process, trial 1 | First whole process, trial 2 |
| --- | ---: | ---: |
| 0% | 0.3368 s | 0.2564 s |
| 55% | 0.5587 s | 0.5227 s |
| 100% | 0.8293 s | 0.8289 s |

With entirely fresh copies, reading and linking took 670 and 679 ms. Kernel CPU time across the workers was 2.72 and 2.69 s. After two runs on the first set of copies, touching every file page through a mapping raised observed residency from 18,960 to 114,689 pages. The next whole process took 0.2856 s, with 150 ms in reading and linking and 0.31 s of kernel CPU time. In the other trial, a separate buffered read followed by the same executable took 0.6337 s for the executable. The measurements exclude the separate preparation step. The scored program performs all its own input reads.

A second control used six fresh compressed copies and six fresh uncompressed copies of the same 2M dataset. The first process on each compressed copy took 0.0730–0.0754 s, against 0.0353–0.0380 s on each uncompressed copy. All 48 outputs matched. Builders publishes whole-container times and withholds hidden stage and page-cache measurements. [All 66 process measurements](scripts/benchmarks/input-cache-state.csv) · [Official scorer](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

Three paired Linux x86 simulation trials on public practice seed 719 compared the exact 0.87 s Xeon release with the compact-record release. Median simulated ticks were 1.524 billion and 1.534 billion respectively. The compact release won the first pair. The scored release won the other two. All six runs matched every output row and column. [Per-run instruction counts, ticks and executable hashes](scripts/benchmarks/xeon-seed719-comparison.csv)

## File scheduling and partial event decoding

Each parser starts with a contiguous range of file pieces and takes eight adjacent pieces per claim. After a worker finishes its range, it can claim remaining pieces from another worker. The AVX-512 reader carries decoded IDs, ingestion order and any verified event timestamp into validation of purchases and unusual rows. An earlier invalid record still suppresses a later copy across shoppers.

Two clean Debian GCC 12 builds produced an AVX-512 executable with SHA-256 `41f6006860310344e20095666f7e7d58e45ccf51a7ec31f38b0e3977319d5767`. It passed all 36 official fixtures, eight directed datasets and the complete 1M and 2M comparisons under x86 Linux instruction execution. A 76,000-row shuffled case with offset timestamps, invalid fields and duplicate IDs matched an independent implementation on 15,025 shopper-day rows and 14 campaign-day rows. Separate checks covered a nine-MiB record crossing input boundaries and signed offsets up to 23 hours and 59 minutes.

On public seed 11971, each fresh Linux Arm container ran as UID 1000 with eight workers, profiling disabled and the input mounted read-only. The C++ runs executed the repository entrypoint. Before each timed launch, the benchmark verified that all 458,487 input pages were resident for a warm run or that zero were resident for a cold run. The ext4 filesystem used a RAM-backed block device. The three implementations ran in rotation, with five fresh containers per table cell. All 60 outputs matched every key and column. [Individual runtimes, executable hashes, page residency and row counts](scripts/benchmarks/file-ranges-and-field-reuse.csv)

| Permitted Arm CPUs | Input pages | 0.87 s submission source | This revision | Pinned Rust |
|---:|---|---:|---:|---:|
| 8 | Warm | 0.2721 s | 0.2638 s | 0.2779 s |
| 8 | Cold | 0.3372 s | 0.3174 s | 0.3299 s |
| 4 | Warm | 0.5012 s | 0.4804 s | 0.5202 s |
| 4 | Cold | 0.5511 s | 0.5307 s | 0.5872 s |

The Arm build uses the scalar reader. The Xeon-specific decoding experiment on 2M events reduced simulated ticks from 252,305,420 to 237,798,304 for a fixed eight-row reader. Those counters describe guest execution. Builders measures elapsed time on its scoring hardware. [Official scoring procedure](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

Three pairs of fresh Linux x86 processes ran the actual 0.87 s release and this release on public seed 719. The launch order alternated between pairs. This release won all three pairs, with 1.461 billion median simulated ticks versus 1.524 billion for the submitted executable. Both builds matched the independent reference on all 2,251,704 shopper-day rows and 28,014 campaign-day rows in every trial. Stage logging was disabled. [Six process measurements and executable hashes](scripts/benchmarks/xeon-seed719-comparison.csv)
