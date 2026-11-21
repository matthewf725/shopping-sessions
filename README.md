# Shopping Sessions

A C++20 pipeline for the [Builders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The Docker image runs offline and writes both daily tables as Parquet. Build it with `docker build --platform linux/amd64 -t shopping-sessions .`. Mount the input at `/data` and an empty writable directory at `/output`.

## Algorithm

1. One worker builds the identity graph from the links while the other workers parse CSV chunks. The AVX-512BW build for Cascade Lake scans 64 bytes at a time, checks quote parity at separators and decodes the four date and time pairs in parallel. Event records usually take 24 bytes. Purchases and timestamps outside the compact range have an exact side record.
2. Partition events by exact event ID and retain the earliest ingestion time, including invalid copies. A bitmap sends potential duplicates to an exact hash table. Group the winners and the valid clicks by raw user ID. Each distinct raw ID then needs one identity lookup. A linked shopper's event and click ranges stay in place. The session builder reads them through a list of ranges.
3. Each event updates one half-hour bucket. All events within that bucket belong to one session. Join consecutive occupied buckets when the next bucket's first time is within 30 minutes of the previous bucket's last time. A bitmap visits populated buckets in time order. Very wide windows use four radix passes over occupied bucket numbers.
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
