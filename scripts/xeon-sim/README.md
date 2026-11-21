# Local AVX-512 simulation

The runner boots an x86-64 Debian Linux guest and executes the same AVX-512 program built by the score image. It works on Apple silicon. It checks eight logical CPUs, the required instruction sets and Linux ZMM state before each run. It captures simulated instruction counts and both output tables. Bochs uses its Skylake-X CPU model. Four cores with two hardware threads each is an assumption about the scorer's eight assigned vCPUs. Google offers Cascade Lake and Ice Lake platforms for N2. [Bochs CPU models](https://bochs.sourceforge.io/doc/docbook/user/cpu-models.html) · [Google N2 platforms](https://docs.cloud.google.com/compute/docs/general-purpose-machines#n2_series)

## Prepare from a fresh checkout

Use Python 3.11 or newer, a C/C++ compiler for the host, Make, patch, tar, bsdtar and ar. Docker can provide the score-image libraries, Linux executable and static guest helper. Run these commands at the repository root.

```sh
assets="$PWD/.bench/xeon-sim"
mkdir -p "$assets/rootfs"
docker run --platform linux/amd64 --rm -v "$PWD/scripts/xeon-sim:/src:ro" \
    -v "$assets:/out" gcc:12-bookworm gcc -O2 -static /src/measure.c -o /out/measure-linux
python3 scripts/xeon-sim/prepare.py --measure "$assets/measure-linux"
docker build --platform linux/amd64 -t shopping-sessions-sim .
container=$(docker create --platform linux/amd64 shopping-sessions-sim)
docker export "$container" | tar -xf - -C "$assets/rootfs"
docker rm "$container"
```

Alternatively, pass a Linux x86-64 cross compiler to `prepare.py --cc` and supply an existing exported score image. The prepare step verifies SHA-256 checksums for Bochs, the Debian kernel and modules, BusyBox, syslinux and the Python ISO writer.

## Compare runs

Point `--data` at the directory containing the organizer's manifest and CSV folders. Each output directory must be new. By default the runner uses the AVX-512 executable in the exported score image. For a comparison, both programs receive the same data, UID, libraries and guest settings. The runner reverses their order on every second repetition. Each run uses a fresh process within the same guest.

```sh
python3 scripts/xeon-sim/run.py --data dev=/path/to/dev-input \
    --binary baseline=/path/to/previous/pipeline-avx512 \
    --binary candidate="$assets/rootfs/usr/local/bin/pipeline-avx512" \
    --repeat 2 --output .bench/xeon-dev
python3 /path/to/starter-duckdb/scripts/compare.py \
    .bench/xeon-dev/dev/candidate /path/to/dev-answer
```

The default 1024 MiB guest works for the million-event reference. Guest input and Parquet files reside in RAM. For the 20M-event practice input, allow 6144 MiB of guest memory, a 256 MiB output disk, and a temporary volume with several GiB free.

```sh
python3 scripts/xeon-sim/run.py --data practice=/path/to/practice \
    --binary candidate="$assets/rootfs/usr/local/bin/pipeline-avx512" \
    --memory-mib 6144 --output-mib 256 --timeout 5400 --work-dir /path/to/tmp \
    --output .bench/xeon-practice
```

`run.json` identifies the binaries, kernel, emulator and staged input disk by SHA-256 and records the guest configuration. `counts.json` records the instructions executed by all guest CPUs from immediately before fork to immediately after wait, including Linux kernel work inside that interval. `virtual_ticks` records the Bochs scheduler's time units. `guest.log` contains feature checks, exit markers and optional pipeline stage readings from `--profile`. The output directory retains both final Parquet tables per binary and dataset. A failed child, missing marker or missing table makes the command fail.

The official measurement includes fresh-container startup and exit. Bochs executes instructions functionally and uses an instruction-based clock. Its figures omit native cache and memory stalls, AVX frequency changes and container startup. Changing `ips` changes the guest clock. Only the scorer measures the contest time. [Bochs clock configuration](https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html) · [Builders scoring](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

## Calibration against scored revisions

Both Linux executables were built with Debian GCC 12.2 and Arrow 21 using the flags from their respective Dockerfiles. `ef41f89` scored 1.52 s and `1018e64` scored 1.12 s. The last column scales the older score by the ratio of simulated instructions. The newer official score provides a check on that estimate.

| Generator input | Runs per build | `ef41f89` instructions | `1018e64` instructions | Estimate for `1018e64` |
| --- | ---: | ---: | ---: | ---: |
| 100k events + 25k clicks, seed 719 | 2 | 0.788 billion | 0.742 billion | 1.43 s |
| 1M events + 250k clicks, seed 1 | 2 | 2.650 billion | 2.141 billion | 1.23 s |
| 20M events + 5M clicks, seed 719 | 1 | 37.104 billion | 28.853 billion | 1.18 s |

The first two rows use medians from alternating run orders. The single practice-size comparison put the estimate 0.062 s above the official result. One comparison gives no error bound for another implementation or input. Both million-event outputs matched the organizer's 117,038 user-day rows and 27,400 campaign-day rows. At practice size the two builds matched on all 2,251,704 user-day rows and 28,014 campaign-day rows. The accounting check agreed across all 14 days and 3,420,024 sessions.

On a separate profiled million-event run of `1018e64`, parsing and linking used 58.2% of the guest time measured inside main, setup 15.4%, sessions and attribution 12.4%, Parquet output 8.3%, and deduplication and partitioning 5.7%. These shares depend on the simulated machine and input size.

## Model an individual loop

`loop.py` disassembles a linked ELF and feeds one straight-line loop, including its back edge, to LLVM's CPU scheduling model. Look up the start and exclusive end addresses with `llvm-objdump -d` for each build. The example addresses below apply to the Debian GCC 12.2 builds used here.

```sh
python3 scripts/xeon-sim/loop.py /path/to/ef41f89/pipeline-avx512 0x92d0 0x931a
python3 scripts/xeon-sim/loop.py /path/to/1018e64/pipeline-avx512 0xa0c0 0xa0ff
```

The first deduplication pass models at 24 micro-ops and 4.0 cycles per record for `ef41f89`, and 19 micro-ops and 3.2 cycles for `1018e64` with LLVM 20.1.8's Cascade Lake model. This estimates steady-state core throughput with ready data. Cold memory and interactions between workers can change the native result. Pass `--cpu icelake-server` to inspect the other N2 platform. Use `--objdump` and `--mca` for non-default tool locations. [LLVM machine code analyzer](https://llvm.org/docs/CommandGuide/llvm-mca.html)
