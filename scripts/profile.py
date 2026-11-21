"""Time one native run, including process launch and exit, using a shared monotonic clock."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pipeline", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    command = [args.pipeline.resolve(), args.input, args.output]
    env = dict(os.environ, PROFILE="1")

    start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    proc = subprocess.run(command, env=env, stderr=subprocess.PIPE, text=True)
    end = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    if proc.returncode:
        sys.stderr.write(proc.stderr)
        return proc.returncode if proc.returncode > 0 else 128 - proc.returncode

    stamps = {}
    stages = []
    for line in proc.stderr.splitlines():
        if match := re.fullmatch(r"(main_start_ns|output_closed_ns) (\d+)", line):
            stamps[match[1]] = int(match[2])
        elif match := re.fullmatch(r"(.+) ([\d.]+) s", line):
            stages.append((match[1], float(match[2]) * 1000))
        else:
            print(line, file=sys.stderr)
    if not {"main_start_ns", "output_closed_ns"} <= stamps.keys():
        sys.exit("This executable did not report its process timing checkpoints.")

    stages.insert(0, ("launch to main", (stamps["main_start_ns"] - start) / 1e6))
    stages.append(("output close to process exit", (end - stamps["output_closed_ns"]) / 1e6))
    stages.append(("total", (end - start) / 1e6))
    for name, milliseconds in stages:
        print(f"{name:30s} {milliseconds:9.2f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
