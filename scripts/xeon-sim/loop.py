"""Estimate the throughput of a single compiled x86 loop on an N2 CPU."""

import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("start", type=lambda s: int(s, 0), help="address of the first instruction")
    parser.add_argument("end", type=lambda s: int(s, 0), help="exclusive end address")
    parser.add_argument("--cpu", choices=("cascadelake", "icelake-server"), default="cascadelake")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument("--mca", default="llvm-mca")
    args = parser.parse_args()
    dump = subprocess.check_output(
        [args.objdump, "-d", "--no-show-raw-insn", f"--start-address={args.start}",
         f"--stop-address={args.end}", args.elf], text=True)
    lines = []
    for line in dump.splitlines():
        match = re.fullmatch(r"\s*([0-9a-fA-F]+):\s+(.+)", line)
        if match:
            lines.append((int(match[1], 16), match[2].split("#")[0].strip()))
    if not lines or lines[0][0] != args.start:
        parser.error("range must start at an instruction")
    branch = re.fullmatch(r"(j\w+)\s+0x([0-9a-fA-F]+)(?:\s+<.*>)?", lines[-1][1])
    if not branch or int(branch[2], 16) != args.start:
        parser.error("range must end with a direct branch back to the first instruction")
    if any(re.match(r"(?:j\w+|call\w*|ret\w*|loop\w*)\b", op) for _, op in lines[:-1]):
        parser.error("loop body must have a single straight-line path")
    body = [" .text", ".Lloop:", *(op for _, op in lines[:-1]), f"{branch[1]} .Lloop"]
    subprocess.run([args.mca, "--mtriple=x86_64-pc-linux-gnu", f"--mcpu={args.cpu}",
                    "--iterations=100"], input="\n".join(body) + "\n", text=True, check=True)


if __name__ == "__main__":
    main()
