#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import os
import struct
import subprocess
import unittest
import zlib
import test_images as fixtures
image = fixtures.image
BLOCK = fixtures.BLOCK


def wrapped(kind, unit=512, payload=None):
    payload = image() if payload is None else payload
    start = 64
    blocks = len(payload) // unit
    total = start + blocks + 64
    b = bytearray(total * unit)
    b[start * unit:start * unit + len(payload)] = payload
    if kind == "apm":
        struct.pack_into(">HHI", b, 0, 0x4552, unit, total)
        for i, (begin, length, name) in enumerate([
                (1, 2, b"Apple_partition_map"), (start, blocks, b"Apple_HFS")], 1):
            off = unit * i
            struct.pack_into(">HHIII", b, off, 0x504d, 0, 2, begin, length)
            b[off + 48:off + 48 + len(name)] = name
    else:
        b[510:512] = b"\x55\xaa"
        b[450] = 0xee if kind == "gpt" else 0xaf
        struct.pack_into("<II", b, 454, 1 if kind == "gpt" else start,
                         total - 1 if kind == "gpt" else blocks)
        if kind == "gpt":
            table = bytearray(128 * 128)
            table[:16] = bytes.fromhex("005346480000aa11aa1100306543ecac")
            table[16:32] = bytes(range(16))
            struct.pack_into("<QQQ", table, 32, start, start + blocks - 1, 0)
            b[unit * 2:unit * 2 + len(table)] = table
            h = bytearray(92)
            h[:8] = b"EFI PART"
            struct.pack_into("<IIIIQQQQ", h, 8, 0x10000, 92, 0, 0, 1,
                             total - 1, 2 + len(table) // unit, total - 2 - len(table) // unit)
            h[56:72] = bytes(range(16, 32))
            struct.pack_into("<QIII", h, 72, 2, 128, 128, zlib.crc32(table))
            struct.pack_into("<I", h, 16, zlib.crc32(h))
            b[unit:unit + len(h)] = h
    return b


class PartitionTests(unittest.TestCase):
    setUp = fixtures.ImageTests.setUp
    tearDown = fixtures.ImageTests.tearDown
    run_image = fixtures.ImageTests.run_image
    def test_partition_tables_and_sector_units(self):
        for kind in ["apm", "gpt", "mbr"]:
            for unit in [512, 4096]:
                with self.subTest(kind=kind, unit=unit):
                    b = wrapped(kind, unit)
                    p = self.run_image(b, "cat", "/.rockbox/rockbox.ipod", options=("--scan",))
                    self.assertEqual(p.stdout, b"TEST ONLY!!!")
                    if unit == 4096:
                        p = self.run_image(b, "cat", "/.rockbox/rockbox.ipod",
                                           options=("--scan", "--sector", "4096"))
                        self.assertEqual(p.stdout, b"TEST ONLY!!!")


    def test_mbr_prefers_4096_unit_over_false_512_hfs_signature(self):
        b = wrapped("mbr", 4096)
        # MBR start is 64.  Interpreting it as 512-byte units points at
        # 32 KiB instead of the real 256 KiB partition.  Plant a fake H+
        # signature at +1024 from that wrong candidate.  The scanner must
        # still choose the real 4096-byte-unit partition.
        wrong = 64 * 512 + 1024
        b[wrong:wrong + 2] = b"H+"
        p = self.run_image(b, "cat", "/.rockbox/rockbox.ipod", options=("--scan",))
        self.assertEqual(p.stdout, b"TEST ONLY!!!")

    def test_partition_checksums_and_bounds(self):
        b = wrapped("gpt")
        b[512 + 16] ^= 1
        self.run_image(b, options=("--scan",), ok=False)
        b = wrapped("gpt")
        b[1024 + 42] ^= 1
        self.run_image(b, options=("--scan",), ok=False)
        b = wrapped("apm")
        struct.pack_into(">I", b, 1024 + 12, 0xffffffff)
        self.run_image(b, options=("--scan",), ok=False)
        b = wrapped("mbr")
        struct.pack_into("<I", b, 458, 0xffffffff)
        self.run_image(b, options=("--scan",), ok=False)

    def test_native_adapter(self):
        binaries = os.environ.get("HFS_RB_TESTS", "").split(os.pathsep)
        if not binaries[0]:
            self.skipTest("Native adapter executables not supplied")
        cases = [(image(), 512), (image(journal=True), 512), (image(), 4096)]
        for kind in ["apm", "gpt", "mbr"]:
            cases.extend([(wrapped(kind), 512), (wrapped(kind, 4096), 512),
                          (wrapped(kind, 4096), 4096)])
        for binary in binaries:
            for i, (data, sector) in enumerate(cases):
                with self.subTest(binary=binary, case=i, sector=sector):
                    self.path.write_bytes(data)
                    p = subprocess.run([binary, str(self.path), str(sector)],
                                       capture_output=True, timeout=10)
                    self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
                    self.assertEqual(self.path.read_bytes(), data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
