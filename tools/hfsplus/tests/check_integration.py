#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
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


if sys.platform == "darwin":
    # Apple Clang does not accept the Linux PIE flags, and the full ARM
    # freestanding firmware headers cannot be compiled against Mach-O.  Run a
    # native host build of the real adapter instead.  Linux/GCC keeps the
    # stricter freestanding syntax check below.
    with tempfile.TemporaryDirectory(prefix="rockpod-hfs-host-") as tmp:
        build = Path(tmp)
        adapter = build / "adapter_host"
        run(cc + ["-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
                  "-Itools/hfsplus/tests/adapter/host/firmware/export",
                  "-Ifirmware/export", "-Ifirmware/hfsplus", "-Ifirmware/include",
                  "tools/hfsplus/tests/adapter_host.c",
                  "firmware/hfsplus/hfsplus_state.c",
                  "firmware/hfsplus/hfsplus_diag.c",
                  "firmware/hfsplus/hfsplus_io.c",
                  "firmware/hfsplus/hfsplus_overlay_format.c",
                  "firmware/hfsplus/hfsplus_overlay_alloc.c",
                  "firmware/hfsplus/hfsplus_overlay_store.c",
                  "firmware/hfsplus/hfsplus_time.c",
                  "firmware/hfsplus/hfsplus_namespace.c",
                  "firmware/hfsplus/hfsplus_overlay_validate.c",
                  "firmware/hfsplus/hfsplus_overlay_load.c",
                  "firmware/hfsplus/hfsplus_fat.c",
                  "firmware/hfsplus/hfsplus_index_cache.c",
                  "firmware/hfsplus/hfsplus_mount.c",
                  "firmware/hfsplus/hfsplus_ro.c",
                  "firmware/hfsplus/hfsplus_partition.c",
                  "-o", str(adapter)])
        run(["python3", str(tests / "test_adapter.py"), str(adapter)])
        print("PASS: macOS host adapter (real HFS+ modules/ro/partition)", flush=True)
    print("Host checks passed; no ARM link, emulator, or iPod boot was tested.")
    raise SystemExit(0)

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
              "firmware/hfsplus/hfsplus_state.c", "firmware/hfsplus/hfsplus_diag.c",
              "firmware/hfsplus/hfsplus_io.c",
              "firmware/hfsplus/hfsplus_overlay_format.c",
              "firmware/hfsplus/hfsplus_overlay_alloc.c",
              "firmware/hfsplus/hfsplus_overlay_store.c",
              "firmware/hfsplus/hfsplus_time.c",
              "firmware/hfsplus/hfsplus_namespace.c",
              "firmware/hfsplus/hfsplus_overlay_validate.c",
              "firmware/hfsplus/hfsplus_overlay_load.c",
              "firmware/hfsplus/hfsplus_fat.c",
              "firmware/hfsplus/hfsplus_index_cache.c",
              "firmware/hfsplus/hfsplus_mount.c",
              "firmware/common/fat.c",
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
        adapter_obj = build / f"{mode}-adapter.o"
        run(cc + flags + defs + san + ["-O1", "-g", "-c",
            "tools/hfsplus/tests/rb_adapter.c", "-o", str(adapter_obj)])
        hfsplus_sources = [s for s in common
                           if s.startswith("firmware/hfsplus/")]
        binary = build / ("rb-test-" + mode)
        run(cc + ["-std=c99", "-O1", "-g"] + san + [str(adapter_obj)] +
            [str(tests / "rb_runner.c")] + hfsplus_sources + link +
            ["-o", str(binary)])
        binaries.append(str(binary))
        print(f"PASS: {mode} host syntax and adapter compilation", flush=True)
    inspect = build / "hfsplus-inspect"
    run(cc + ["-std=c99", "-O1", "-g", "-Ifirmware/hfsplus"] + san +
        ["tools/hfsplus/inspect.c"] + common[:2] + link + ["-o", str(inspect)])
    env["HFS_RB_TESTS"] = os.pathsep.join(binaries)
    print(run(["python3", str(tests / "test_partitions.py"), str(inspect)]))
print("Host checks passed; no ARM link, emulator, full filesystem runtime, or iPod boot was tested.")
