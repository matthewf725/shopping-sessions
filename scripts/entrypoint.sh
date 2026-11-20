#!/bin/sh
set -eu
if [ -x /usr/local/bin/pipeline-avx512 ] && grep -qw -m1 avx512bw /proc/cpuinfo; then
    exec /usr/local/bin/pipeline-avx512 "$@"
fi
exec /usr/local/bin/pipeline "$@"
