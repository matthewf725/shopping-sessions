#!/usr/bin/env bash
# Run on an x86-64 Linux host with Docker, Rust, Python (polars), curl and unzip.
set -euo pipefail
size=${1:-1000000}
seed=${2:-19}
repeats=${3:-3}
cpus=${NCPU:-8}
memory=${MEM_GB:-24}
work=$(mktemp -d)
container=''
cleanup() {
    if [[ -n $container ]]; then docker rm -f "$container" >/dev/null; fi
    if [[ -d "$work/scratch" ]] && [[ -n $(find "$work/scratch" -mindepth 1 ! -uid "$(id -u)" -print -quit 2>/dev/null) ]]; then
        docker run --rm --platform linux/amd64 --network none --read-only --user 1000:1000 \
            -v "$work/scratch:/scratch" --entrypoint find "$image" /scratch -uid 1000 -exec chmod a+rwX '{}' +
    fi
    rm -r "$work"
}
trap cleanup EXIT
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
    local data=$1 out=$2 run_image=${3:-$image} started finished code
    local -a pin=()
    if [[ -n ${CPUSET:-} ]]; then pin=(--cpuset-cpus "$CPUSET"); fi
    mkdir -p "$out/user_daily" "$out/campaign_daily" "$work/scratch"
    chmod a+rwx "$out" "$out/user_daily" "$out/campaign_daily" "$work/scratch"
    container=$(docker create --platform linux/amd64 --read-only --network none --cap-drop ALL \
        --security-opt no-new-privileges --tmpfs /tmp:rw,size=64m,mode=1777 \
        --user 1000:1000 --pids-limit 4096 --cpus "$cpus" "${pin[@]}" --memory "${memory}g" --memory-swap "${memory}g" \
        -v "$(realpath "$data"):/data:ro" -v "$(realpath "$out"):/output" -v "$work/scratch:/scratch" \
        -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch \
        -e NCPU="$cpus" -e MEM_GB="$memory" "$run_image")
    docker start -a "$container"
    read -r started finished code < <(docker inspect -f '{{.State.StartedAt}} {{.State.FinishedAt}} {{.State.ExitCode}}' "$container")
    if [[ $code != 0 ]]; then return 1; fi
    elapsed_ns=$(( $(date -d "$finished" +%s%N) - $(date -d "$started" +%s%N) ))
    docker rm "$container" >/dev/null
    container=''
}
# Exercise the shipped executable on every fixture. Mount each case's input/output via the scorer wrapper.
export IMAGE="$image" CPUS="$cpus" MEM="${memory}g" PLATFORM=linux/amd64
python3 starter-duckdb/scripts/run_fixtures.py starter-duckdb/fixtures -- bash starter-duckdb/scripts/docker_run.sh
gen=generator/target/release/generate
"$gen" --seed 1 --events 1000000 --clicks 250000 --threads "$cpus" --output dev
run dev dev-out
python3 starter-duckdb/scripts/compare.py dev-out dev-answer
if [[ -n ${COMPARE_IMAGE:-} ]]; then
    run dev dev-reference "$COMPARE_IMAGE"
    python3 starter-duckdb/scripts/compare.py dev-reference dev-answer
fi
if [ "$size" = 1000000 ]; then exit 0; fi
"$gen" --seed "$seed" --events "$size" --clicks "$((size/4))" --threads "$cpus" --output scale
if [[ -n ${COMPARE_IMAGE:-} ]]; then
    # Warm the input once outside the timers. The first image alternates across pairs.
    find scale -type f -exec cat {} + >/dev/null
    for ((i=1; i<=repeats; ++i)); do
        methods=(candidate reference)
        if ((i % 2 == 0)); then methods=(reference candidate); fi
        for method in "${methods[@]}"; do
            run_image=$image
            if [[ $method == reference ]]; then run_image=$COMPARE_IMAGE; fi
            run scale "out-$method" "$run_image"
            echo "events=$size seed=$seed vcpus=$cpus repeat=$i method=$method container_ns=$elapsed_ns"
        done
        python3 starter-duckdb/scripts/compare.py out-candidate out-reference
        rm -r out-candidate out-reference
    done
    exit 0
fi
for ((i=1; i<=repeats; ++i)); do
    run scale "out-$i"
    echo "events=$size seed=$seed vcpus=$cpus repeat=$i container_ms=$((elapsed_ns/1000000))"
    python3 "$root/scripts/check_totals.py" "out-$i"
done
