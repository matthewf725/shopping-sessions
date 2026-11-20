#!/usr/bin/env bash
# Run on an x86-64 Linux host with Docker, Rust, Python (polars), curl and unzip.
set -euo pipefail
size=${1:-1000000}
seed=${2:-19}
repeats=${3:-3}
cpus=${NCPU:-$(nproc)}
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$work"
base=https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files
for file in generator starter-duckdb dev-answer; do
    curl --retry 3 -fsSL "$base/$file.zip" -o "$file.zip"
    unzip -q "$file.zip"
done
cargo build --release --manifest-path generator/Cargo.toml --locked
image=shopping-sessions-bench:local
docker build --platform linux/amd64 -t "$image" "$root"
run() {
    local data=$1 out=$2
    mkdir -p "$out/user_daily" "$out/campaign_daily" "$work/scratch"
    chmod a+rwx "$out" "$out/user_daily" "$out/campaign_daily" "$work/scratch"
    docker run --rm --platform linux/amd64 --read-only --network none --cap-drop ALL \
        --security-opt no-new-privileges --tmpfs /tmp:rw,size=64m,mode=1777 \
        --user 1000:1000 --cpus "$cpus" --memory "${MEM_GB:-7}g" --memory-swap "${MEM_GB:-7}g" \
        -v "$(realpath "$data"):/data:ro" -v "$(realpath "$out"):/output" -v "$work/scratch:/scratch" \
        -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch \
        -e NCPU="$cpus" -e PROFILE=1 "$image"
}
# Exercise the shipped executable on every fixture. Mount each case's input/output via the scorer wrapper.
export IMAGE="$image" CPUS="$cpus" MEM="${MEM_GB:-7}g" PLATFORM=linux/amd64
python3 starter-duckdb/scripts/run_fixtures.py starter-duckdb/fixtures -- bash starter-duckdb/scripts/docker_run.sh
gen=generator/target/release/generate
"$gen" --seed 1 --events 1000000 --clicks 250000 --threads "$cpus" --output dev
run dev dev-out
python3 starter-duckdb/scripts/compare.py dev-out dev-answer
if [ "$size" = 1000000 ]; then exit 0; fi
"$gen" --seed "$seed" --events "$size" --clicks "$((size/4))" --threads "$cpus" --output scale
for ((i=1; i<=repeats; ++i)); do
    start=$(date +%s%N)
    run scale "out-$i"
    stop=$(date +%s%N)
    echo "events=$size seed=$seed vcpus=$cpus repeat=$i wall_ms=$(((stop-start)/1000000))"
    python3 "$root/scripts/check_totals.py" "out-$i"
done
