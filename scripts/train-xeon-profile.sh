#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
    echo "Usage: $0 PATH_TO_PUBLIC_DEV_INPUT" >&2
    exit 2
fi
input=$(realpath "$1")
repo=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
cd "$build"
cp -R "$repo/src" src
mkdir profiles output

# A successful scored process calls _Exit after closing its Parquet files.
# The training-only wrapper flushes GCC's counters at that same point.
cat > profile-exit.c <<'C'
extern void __gcov_dump(void);
extern void __real__Exit(int);
void __wrap__Exit(int status) {
    __gcov_dump();
    __real__Exit(status);
}
C
gcc -O2 -c profile-exit.c -o profile-exit.o
arch=(-march=skylake-avx512 -mtune=cascadelake)
flags=(-O3 -DNDEBUG "${arch[@]}" -std=c++20 -pthread "-fprofile-prefix-path=$PWD")
g++ "${flags[@]}" -fprofile-generate=profiles -fprofile-update=atomic \
    src/main.cpp src/output.cpp profile-exit.o -Wl,--wrap=_Exit -o pipeline
GCOV_PREFIX="$PWD/profiles" GCOV_PREFIX_STRIP=128 NCPU=8 ./pipeline "$input" "$PWD/output"

# A stale or incompatible profile fails here before either checked-in file is replaced.
g++ "${flags[@]}" -fprofile-use=profiles -Werror=missing-profile -Werror=coverage-mismatch \
    src/main.cpp src/output.cpp -o pipeline
install -m 644 profiles/pipeline-main.gcda profiles/pipeline-output.gcda "$repo/profiles/"
