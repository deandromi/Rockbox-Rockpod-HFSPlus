#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description="Host checks only; never produces ARM firmware")
parser.add_argument("--sanitize", action="store_true", help="Linux/GCC ASan and UBSan")
args = parser.parse_args()
src = Path(__file__).resolve().parents[3]
tests = src / "tools/hfsplus/tests"
cc = shlex.split(os.environ.get("CC", "cc"))
env = os.environ.copy()
if args.sanitize:
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")


def run(command, cwd=src):
    p = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    if p.returncode:
        print(p.stdout + p.stderr)
        raise SystemExit(f"Failed: {shlex.join(map(str, command))}")
    return p.stdout + p.stderr


with tempfile.TemporaryDirectory(prefix="rockpod-hfs-host-") as tmp:
    build = Path(tmp)
    shutil.copyfile(tests / "host-autoconf.h", build / "autoconf.h")
    include_dirs = [build, "firmware/target/arm/s5l8702/ipod6g",
                    "firmware/target/arm/s5l8702", "firmware/target/arm", "firmware",
                    "firmware/export", "firmware/drivers", "firmware/include",
                    "firmware/kernel/include", "firmware/libc/include", "bootloader",
                    "lib/rbcodec", "lib/rbcodec/metadata"]
    flags = ["-std=gnu99", "-ffreestanding", "-DROCKBOX", "-DIPOD_6G",
             "-DMEMORYSIZE=64", "-DTARGET_ID=71", "-Wno-format"]
    flags += ["-I" + str(d) for d in include_dirs]
    common = ["firmware/hfsplus/hfsplus_ro.c", "firmware/hfsplus/hfsplus_partition.c",
              "firmware/hfsplus/hfsplus_rb.c", "firmware/common/fat.c",
              "firmware/common/disk.c", "firmware/common/file.c",
              "firmware/common/file_internal.c", "firmware/common/fileobj_mgr.c",
              "firmware/common/rb_namespace.c", "firmware/common/disk_cache.c"]
    run(cc + ["tools/convbdf.c", "-o", str(build / "convbdf")])
    run([str(build / "convbdf"), "-l", "127", "-h", "-o", str(build / "sysfont.h"),
         "fonts/08-Schumacher-Clean.bdf"])
    san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"] if args.sanitize else []
    link = ["-fsanitize=address,undefined", "-no-pie"] if args.sanitize else []
    binaries = []
    for mode in ["firmware", "bootloader"]:
        defs = ["-DBOOTLOADER"] if mode == "bootloader" else []
        sources = common + (["bootloader/ipod-s5l87xx.c"] if defs else [])
        run(cc + flags + defs + ["-fsyntax-only"] + sources)
        objects = []
        for i, source in enumerate(["firmware/hfsplus/hfsplus_rb.c", "tools/hfsplus/tests/rb_adapter.c"]):
            obj = build / f"{mode}-{i}.o"
            run(cc + flags + defs + san + ["-O1", "-g", "-c", source, "-o", str(obj)])
            objects.append(str(obj))
        binary = build / ("rb-test-" + mode)
        run(cc + ["-std=c99", "-O1", "-g"] + san + objects +
            [str(tests / "rb_runner.c")] + common[:2] + link + ["-o", str(binary)])
        binaries.append(str(binary))
        print(f"PASS: {mode} host syntax and adapter compilation", flush=True)
    inspect = build / "hfsplus-inspect"
    run(cc + ["-std=c99", "-O1", "-g", "-Ifirmware/hfsplus"] + san +
        ["tools/hfsplus/inspect.c"] + common[:2] + link + ["-o", str(inspect)])
    env["HFS_RB_TESTS"] = os.pathsep.join(binaries)
    print(run(["python3", str(tests / "test_partitions.py"), str(inspect)]))
print("Host checks passed; no ARM link, emulator, full filesystem runtime, or iPod boot was tested.")
