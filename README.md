# Shopping Sessions

A C++20 pipeline for the [Builders Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The Docker image runs offline and writes both daily tables as Parquet. Build it with `docker build --platform linux/amd64 -t shopping-sessions .`. Mount the input at `/data` and an empty writable directory at `/output`.

## Algorithm

1. Scan the CSV files in parallel. Validate complete-field quoting and timestamps. Hash-partition events by their exact IDs and keep the copy with the earliest ingestion time, including invalid copies. A union-find over the links supplies each person's byte-wise smallest ID.
2. Use a counting scatter to place each person's observations together. An event updates one half-hour bucket. All times in a bucket are within 30 minutes. Visiting occupied buckets in order, the next bucket continues the current session exactly when its earliest time is within 30 minutes of the previous bucket's latest time. Store individual category contributions only for purchases. A bitmap enumerates occupied buckets for the contest window. For exceptionally wide windows, a four-pass radix traversal orders the occupied bucket numbers.
3. Session starts are more than 30 minutes apart. A bitmap and prefix counts map a click's seven-day interval to a range of sessions. Keep the most recent click first eligible at each session, breaking ties by ID, then carry the winner forward until expiration. OR the eligible range into a campaign bitset to count each assist once. Identical click copies give identical winners and set the same bits.
4. Workers write independent user Parquet files and reduce campaign totals. Apache Arrow's C++ Parquet writer (21.0.0) is the output library. The CSV scanner, deduplication, session construction and attribution code were written for this track.

For the 14-day input window, each event takes expected constant work. A seven-day click range spans at most 337 session starts and seven 64-bit words. Input row order has no effect on the result. No event or click comparison sort is used.

## Verification

`scripts/bench.sh 20000000 19 3` builds the image, runs the organizers' 36 fixtures and exact million-event comparison, then times three fresh containers on a generated practice-size input. Pass `60000000` for final size. The script uses the organizers' public Rust generator and checker, and requires Linux, Docker, Cargo, Python with Polars, curl and unzip. Set `NCPU=8 MEM_GB=24` on a suitable host to reproduce the resource limits. Benchmarks run locally.

For a native build, run `make ARROW_PREFIX=/path/to/arrow`. Run `./pipeline INPUT EMPTY_OUTPUT` with `NCPU` set to the desired thread count. Set `PROFILE=1` for stage timings on stderr.
