"""Run the Linux AVX-512 pipeline inside a headless Bochs guest."""

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
DEFAULT_ASSETS = HERE.parents[1] / ".bench/xeon-sim"
CPU_MODEL = "corei7_skylake_x"
TRACK = 16 * 63 * 512


def named_path(arg):
    name, sep, raw = arg.partition("=")
    if not sep or not raw or not re.fullmatch(r"[a-zA-Z0-9_-]+", name):
        raise argparse.ArgumentTypeError("expected NAME=PATH")
    try:
        path = Path(raw).resolve(strict=True)
    except OSError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return name, path


def boot_iso(work, assets):
    sys.path.insert(0, str(assets / "python"))
    import pycdlib

    # Linux newc initramfs. The console node is needed before devtmpfs mounts.
    with gzip.open(work / "initrd.gz", "wb", compresslevel=1) as out:
        ino = 0

        def entry(name, mode, data=b"", device=(0, 0)):
            nonlocal ino
            ino += 1
            name = name.encode() + b"\0"
            fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, *device, len(name), 0)
            out.write(b"070701" + "".join(f"{x:08x}" for x in fields).encode() + name)
            out.write(b"\0" * (-out.tell() % 4))
            out.write(data)
            out.write(b"\0" * (-out.tell() % 4))

        for folder in ("dev", "proc", "sys", "bin", "tmp", "modules"):
            entry(folder, stat.S_IFDIR | 0o755)
        entry("dev/console", stat.S_IFCHR | 0o600, device=(5, 1))
        entry("dev/null", stat.S_IFCHR | 0o666, device=(1, 3))
        entry("bin/busybox", stat.S_IFREG | 0o755, (assets / "busybox").read_bytes())
        entry("init", stat.S_IFREG | 0o755, (HERE / "guest.sh").read_bytes())
        for module in sorted((assets / "modules").glob("*.ko")):
            entry("modules/" + module.name, stat.S_IFREG | 0o644, module.read_bytes())
        entry("TRAILER!!!", 0)

    cfg = (b"DEFAULT run\nPROMPT 0\nTIMEOUT 1\nLABEL run\n KERNEL /linux\n"
           b" APPEND initrd=/initrd.gz console=ttyS0,115200n8 rdinit=/init nokaslr "
           b"quiet loglevel=3 panic=-1\n")
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3)
    for name in ("linux", "isolinux.bin", "ldlinux.c32"):
        iso.add_file(str(assets / name), "/" + name.upper() + ";1")
    iso.add_file(str(work / "initrd.gz"), "/INITRD.GZ;1")
    iso.add_fp(io.BytesIO(cfg), len(cfg), "/ISOLINUX.CFG;1")
    iso.add_eltorito("/ISOLINUX.BIN;1", boot_load_size=4, boot_info_table=True)
    iso.write(str(work / "boot.iso"))
    iso.close()


def disk_geometry(path, size):
    cylinders = (size + TRACK - 1) // TRACK
    with path.open("ab") as disk:
        disk.truncate(cylinders * TRACK)
    return f'type=disk, path="{path}", mode=flat, cylinders={cylinders}, heads=16, spt=63'


def sha256(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def simulate(args, work):
    assets = args.assets.resolve()
    rootfs = (args.rootfs or assets / "rootfs").resolve()
    programs = args.binary or [("scored", rootfs / "usr/local/bin/pipeline-avx512")]
    if len(dict(programs)) != len(programs) or len(dict(args.data)) != len(args.data):
        raise ValueError("duplicate dataset or binary name")
    runs = []
    for case, _ in args.data:
        for repeat in range(1, args.repeat + 1):
            order = programs if repeat % 2 else reversed(programs)
            runs.extend((case, label, repeat) for label, _ in order)
    boot_iso(work, assets)
    with tarfile.open(work / "input.raw", "w") as disk:
        disk.addfile(tarfile.TarInfo("SHOP_INPUT_V1"))
        if args.profile:
            disk.addfile(tarfile.TarInfo("shop-profile"))
        # The score image ships these Debian and Arrow shared libraries.
        for name in ("libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
                     "libgcc_s.so.1", "libstdc++.so.6", "ld-linux-x86-64.so.2"):
            relative = "lib/x86_64-linux-gnu/" + name
            file = next((d / name for d in (rootfs / "lib/x86_64-linux-gnu",
                                             rootfs / "usr/lib/x86_64-linux-gnu")
                         if (d / name).exists()), None)
            if file is None:
                raise FileNotFoundError(relative)
            disk.add(file.resolve(), relative)
        loader = tarfile.TarInfo("lib64/ld-linux-x86-64.so.2")
        loader.type = tarfile.SYMTYPE
        loader.linkname = "../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
        disk.addfile(loader)
        for name in ("libarrow.so.2100", "libparquet.so.2100"):
            relative = "opt/arrow/lib/" + name
            disk.add((rootfs / relative).resolve(), relative)
        disk.add(assets / "measure", "bin/measure")
        for label, binary in programs:
            disk.add(binary, "programs/" + label)
        for case, path in args.data:
            disk.add(path, "inputs/" + case)
        commands = "".join(f"{label} {case} {n}\n" for case, label, n in runs).encode()
        entry = tarfile.TarInfo("runs")
        entry.size = len(commands)
        disk.addfile(entry, io.BytesIO(commands))
    source = disk_geometry(work / "input.raw", (work / "input.raw").stat().st_size)
    metadata = {
        "cpu_model": CPU_MODEL, "sockets": 1, "cores_per_socket": 4, "threads_per_core": 2,
        "memory_mib": args.memory_mib, "resident_mib": args.resident_mib,
        "profile": args.profile, "repeats": args.repeat,
        "datasets": [name for name, _ in args.data],
        "binary_sha256": {name: sha256(file) for name, file in programs},
        "input_disk_sha256": sha256(work / "input.raw"),
        "emulator_sha256": sha256(assets / "bochs"), "kernel_sha256": sha256(assets / "linux"),
    }
    (args.output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    dest = disk_geometry(work / "result.raw", args.output_mib * 1024**2)
    with (work / "result.raw").open("r+b") as output_disk:
        output_disk.write(b"SHOP_OUTPUT_V1")
    serial = work / "serial.log"
    config = work / "bochsrc"
    config.write_text(f'''memory: guest={args.memory_mib}, host={args.resident_mib}
romimage: file="{assets / 'bios.bin'}"
vgaromimage: file="{assets / 'vgabios.bin'}"
display_library: nogui
cpu: model={CPU_MODEL}, count=1:4:2, quantum=32, ips=20000000, ignore_bad_msrs=1, msrs="{assets / 'msrs.def'}"
clock: sync=none, time0=1700000000
ata0-master: {source}
ata0-slave: {dest}
ata1-master: type=cdrom, path="{work / 'boot.iso'}", status=inserted
boot: cdrom
com1: enabled=1, mode=file, dev="{serial}"
log: {work / 'bochs.log'}
panic: action=fatal
error: action=report
info: action=ignore
debug: action=ignore
''')
    logs = args.output
    try:
        with (logs / "emulator.log").open("wb") as log:
            subprocess.run([assets / "bochs", "-q", "-f", config], stdout=log,
                           stderr=subprocess.STDOUT, timeout=args.timeout, cwd=work)
    finally:
        if serial.exists():
            shutil.copyfile(serial, logs / "guest.log")
        if (work / "bochs.log").exists():
            shutil.copyfile(work / "bochs.log", logs / "bochs.log")
    guest = (logs / "guest.log").read_text(errors="replace") if serial.exists() else ""
    expected = [f"SHOP_SIM_OK {case} {label} {n}" for case, label, n in runs]
    if "SHOP_SIM_COMPLETE" not in guest or any(x not in guest.splitlines() for x in expected):
        raise RuntimeError(f"guest did not complete every run, see {logs}")
    counts = re.findall(r"SHOP_SIM_COUNTER instructions=(\d+) virtual_ticks=(\d+)",
                        (logs / "emulator.log").read_text(errors="replace"))
    if len(counts) != len(runs):
        raise RuntimeError("missing counter markers from the instrumented Bochs build")
    allowed = {(case, label) for case, label, _ in runs}
    tables = set()
    with tarfile.open(work / "result.raw", "r:") as disk:
        for member in disk:
            parts = Path(member.name).parts
            if member.isdir():
                continue
            if (not member.isfile() or len(parts) != 4 or tuple(parts[:2]) not in allowed or
                    parts[2] not in ("user_daily", "campaign_daily") or not parts[3].endswith(".parquet")):
                raise ValueError(f"unexpected guest output {member.name}")
            file = logs.joinpath(*parts)
            file.parent.mkdir(parents=True, exist_ok=True)
            with disk.extractfile(member) as src, file.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            tables.add(tuple(parts[:3]))
    if tables != {(case, label, table) for case, label in allowed
                  for table in ("user_daily", "campaign_daily")}:
        raise RuntimeError("guest output is missing a Parquet table")
    rows = [dict(dataset=c, binary=b, repeat=i, instructions=int(ins), virtual_ticks=int(ticks))
            for (c, b, i), (ins, ticks) in zip(runs, counts)]
    (logs / "counts.json").write_text(json.dumps(rows, indent=2) + "\n")
    for row in rows:
        print("{dataset} {binary} run {repeat}  {instructions:,} simulated instructions".format(**row))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--rootfs", type=Path, help="exported Debian score image filesystem")
    parser.add_argument("--binary", type=named_path, action="append", metavar="NAME=ELF")
    parser.add_argument("--data", type=named_path, action="append", required=True, metavar="NAME=DIR")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--profile", action="store_true",
                        help="enable pipeline stage timings from the guest's virtual clock")
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--resident-mib", type=int,
                        help="host RAM budget for guest pages, up to 8192 MiB in Bochs 3.1")
    parser.add_argument("--output-mib", type=int, default=64)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--work-dir", type=Path, help="parent directory for temporary guest disks")
    parser.add_argument("--output", type=Path, required=True, help="new directory for counts and Parquet")
    args = parser.parse_args()
    if min(args.repeat, args.memory_mib, args.output_mib, args.timeout) < 1:
        parser.error("all sizes, repeat and timeout must be positive")
    max_resident = min(args.memory_mib, 8192)
    if args.resident_mib is None:
        args.resident_mib = max_resident
    elif not 1 <= args.resident_mib <= max_resident:
        parser.error(f"--resident-mib must be between 1 and {max_resident}")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="shop-bochs-", dir=args.work_dir) as tmp:
        simulate(args, Path(tmp).resolve())


if __name__ == "__main__":
    main()
