"""Build the local emulator and assemble the matching Debian boot files."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from urllib.request import urlopen
import zipfile

HERE = Path(__file__).resolve().parent
DEBIAN = "https://deb.debian.org/debian/"
INSTALLER = DEBIAN + "dists/bookworm/main/installer-amd64/20230607+deb12u15/images/"
BOCHS = "https://codeload.github.com/bochs-emu/Bochs/tar.gz/refs/tags/REL_3_1_FINAL"
WHEEL = ("https://files.pythonhosted.org/packages/0c/dd/95557c0e3bad0e8ba10c0f17d9deffb139b106eda"
         "528a744e544799cbbfb/pycdlib-1.14.0-py2.py3-none-any.whl")


def fetch(cache, name, url, digest):
    path = cache / name
    if path.exists():
        with path.open("rb") as file:
            if hashlib.file_digest(file, "sha256").hexdigest() == digest:
                return path
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as tmp:
        candidate = Path(tmp.name)
        try:
            with urlopen(url, timeout=90) as response:
                shutil.copyfileobj(response, tmp)
            tmp.flush()
            with candidate.open("rb") as file:
                if hashlib.file_digest(file, "sha256").hexdigest() != digest:
                    raise ValueError(f"checksum mismatch for {url}")
            os.replace(candidate, path)
        finally:
            candidate.unlink(missing_ok=True)
    return path


def copy_modules(extracted, destination):
    def key(name):
        return name.replace("-", "_")

    available = {key(f.stem): f for f in extracted.rglob("*.ko")}
    builtin = {key(Path(line).stem) for line in
               next(extracted.rglob("modules.builtin")).read_text().splitlines()}
    selected = []

    def add(name):
        name = key(name)
        if name in builtin or name in selected:
            return
        module = available[name]
        deps = re.search(rb"(?:^|\0)depends=([^\0]*)\0", module.read_bytes())
        for dep in deps[1].decode().split(","):
            if dep:
                add(dep)
        selected.append(name)

    for name in ("ata_piix", "sd_mod"):
        add(name)
    destination.mkdir(exist_ok=True)
    for index, name in enumerate(selected):
        file = available[name]
        shutil.copyfile(file, destination / f"{index:02d}-{file.name}")


def prepare(args, work):
    dest = args.assets.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    cache = dest / "downloads"
    cache.mkdir(exist_ok=True)
    archive = fetch(cache, "bochs.tgz", BOCHS,
                    "0d3766784bbcaa4d6fe57dac50f83c6d7095d0ea212fd7904cbf7a775347159a")
    bochs = work / "bochs"
    bochs.mkdir()
    subprocess.run(["tar", "-xzf", archive, "-C", bochs, "--strip-components=2",
                    "Bochs-REL_3_1_FINAL/bochs"], check=True)
    for name in ("counters.patch", "vsib.patch"):
        with (HERE / name).open("rb") as patch:
            subprocess.run(["patch", "-p1"], cwd=bochs, stdin=patch, check=True)
    subprocess.run(["./configure", "--enable-x86-64", "--enable-smp", "--enable-avx",
                    "--enable-evex", "--with-nogui", "--without-x11", "--without-sdl2",
                    "--disable-plugins"], cwd=bochs, check=True,
                   env={**os.environ, "CFLAGS": "-O3 -DNDEBUG", "CXXFLAGS": "-O3 -DNDEBUG"})
    subprocess.run(["make", f"-j{min(8, os.cpu_count() or 1)}", "bochs"], cwd=bochs, check=True)
    for source, target in (("bochs", "bochs"), ("bios/BIOS-bochs-latest", "bios.bin"),
                           ("bios/VGABIOS-lgpl/VGABIOS-lgpl-latest.bin", "vgabios.bin"),
                           ("msrs.def", "msrs.def")):
        shutil.copy2(bochs / source, dest / target)
    kernel = fetch(cache, "linux", INSTALLER + "netboot/debian-installer/amd64/linux",
                   "d8808aa4ca188560da1e6d749dcb930c87a5fd8b11ebff1f3fa6d728af35203d")
    shutil.copy2(kernel, dest / "linux")
    initrd = fetch(cache, "cdrom-initrd.gz", INSTALLER + "cdrom/initrd.gz",
                   "032102f9f735fa8589172af616303cde2570275496ca6a9424c2763cb46e5675")
    subprocess.run(["bsdtar", "-xf", initrd, "-C", work, "lib/modules/*"], check=True)
    copy_modules(work / "lib/modules", dest / "modules")
    packages = (
        ("pool/main/b/busybox/busybox-static_1.35.0-4+deb12u1+b1_amd64.deb",
         "3d3fdbe91d4660c873e14b092c213fe81c1da6362daa236eb25d0171eb108744",
         "bin/busybox", "busybox"),
        ("pool/main/s/syslinux/isolinux_6.04~git20190206.bf6db5b4+dfsg1-3_all.deb",
         "cb14af88adc2c3844976cd2de61571d2af94d2fb65c277fe4acf3dc8a5d25aae",
         "usr/lib/ISOLINUX/isolinux.bin", "isolinux.bin"),
        ("pool/main/s/syslinux/syslinux-common_6.04~git20190206.bf6db5b4+dfsg1-3_all.deb",
         "f7e4ead7fdfe5cb6117d8d2485d75e53570a8cf680c81a432f92c1774f878875",
         "usr/lib/syslinux/modules/bios/ldlinux.c32", "ldlinux.c32"),
    )
    for path, digest, member, name in packages:
        deb = fetch(cache, name + ".deb", DEBIAN + path, digest)
        contents = subprocess.check_output(["ar", "t", deb], text=True).splitlines()
        part = next(n.strip() for n in contents if n.startswith("data.tar"))
        with (work / part).open("wb") as out:
            subprocess.run(["ar", "p", deb, part], stdout=out, check=True)
        with (dest / name).open("wb") as out:
            subprocess.run(["bsdtar", "-xOf", work / part, "./" + member], stdout=out, check=True)
    wheel = fetch(cache, "pycdlib.whl", WHEEL,
                  "a905827335f0066af3fd416c5cf9b1f29dffaf4d0914b714555213d1809f38d4")
    with zipfile.ZipFile(wheel) as z:
        z.extractall(dest / "python", [n for n in z.namelist() if n.startswith("pycdlib/")])
    if args.measure:
        if args.measure.resolve() != dest / "measure":
            shutil.copy2(args.measure, dest / "measure")
    else:
        subprocess.run([args.cc, "-static", "-O2", HERE / "measure.c", "-o", dest / "measure"],
                       check=True)
    print(f"Simulator assets ready at {dest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=HERE.parents[1] / ".bench/xeon-sim")
    parser.add_argument("--cc", default="x86_64-linux-gnu-gcc", help="Linux x86-64 C compiler")
    parser.add_argument("--measure", type=Path, help="prebuilt static Linux x86-64 measure.c")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="build-shop-sim-") as tmp:
        prepare(args, Path(tmp))
