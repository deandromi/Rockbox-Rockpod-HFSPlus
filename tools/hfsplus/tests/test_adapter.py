#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic host adapter tests for overlay ownership and crash paths."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ADAPTER_ARG = sys.argv[1] if len(sys.argv) == 2 else ""
sys.argv = [sys.argv[0], "/bin/true"]
import test_images as fixtures

HARNESS = ""


def overlay_layout(b, base):
    magic = bytes(b[base:base + 8])
    if magic == b"RPOVL12\0":
        version = 2
    elif magic == b"RPOVL11\0":
        version = 1
    else:
        raise AssertionError(f"unknown overlay magic {magic!r}")
    block, count, total, bitmap_bytes, data_start = struct.unpack_from(
        "<5I", b, base + 12)
    assert block == 4096 and count == 2048
    bitmap_start = 1 if version == 1 else 2
    bitmap_blocks = (bitmap_bytes + block - 1) // block
    entry_start = (bitmap_start + bitmap_blocks) * block
    return version, bitmap_start * block, entry_start, data_start


def entries(b, base):
    version, bitmap, entry_start, data_start = overlay_layout(b, base)
    out = []
    for i in range(2048):
        raw = bytes(b[base + entry_start + i * 320:
                      base + entry_start + (i + 1) * 320])
        ident, parent, flags, base_id = struct.unpack_from("<4I", raw, 0)
        if not flags & 1:
            continue
        ds, db = struct.unpack_from("<2I", raw, 24)
        name_len = struct.unpack_from("<H", raw, 38)[0]
        out.append(dict(slot=i, id=ident, parent=parent, flags=flags,
                        base_id=base_id, data_start=ds, data_blocks=db,
                        name=raw[40:40 + name_len]))
    return out, bitmap, entry_start, data_start


def container_fork_offset(b):
    cat = 5 * fixtures.BLOCK
    for leaf in (1, 2):
        start = cat + leaf * fixtures.BLOCK
        count = struct.unpack_from(">H", b, start + 10)[0]
        for i in range(count):
            off = struct.unpack_from(">H", b, start + fixtures.BLOCK - 2 * (i + 1))[0]
            key_size = struct.unpack_from(">H", b, start + off)[0]
            parent = struct.unpack_from(">I", b, start + off + 2)[0]
            units = struct.unpack_from(">H", b, start + off + 6)[0]
            name = b[start + off + 8:start + off + 8 + units * 2].decode(
                "utf-16-be")
            if parent == 2 and name == ".rockpod-rw":
                return start + off + key_size + 2 + 88
    raise AssertionError("container record not found")


class AdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.image = fixtures.image(container=True, blocks=16384)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def path(self, name):
        return self.dir / name

    def run_harness(self, image, test, fail_after=None, expect=0):
        src = self.path("in.hfs")
        dst = self.path("out.hfs")
        src.write_bytes(image)
        command = [HARNESS, str(src), str(dst), test]
        if fail_after is not None:
            command.append(str(fail_after))
        p = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, expect, p.stderr)
        return dst.read_bytes() if dst.exists() else image

    def test_readwrite_and_remount(self):
        self.run_harness(self.image, "rw")

    def test_container_extent_in_metadata_is_rejected(self):
        b = bytearray(self.image)
        fork = container_fork_offset(b)
        struct.pack_into(">I", b, fork + 16, 1)  # allocation-file block
        self.run_harness(bytes(b), "readonly")

    def test_container_extent_unallocated_is_rejected(self):
        b = bytearray(self.image)
        fork = container_fork_offset(b)
        struct.pack_into(">I", b, fork + 16, 6000)  # free block
        self.run_harness(bytes(b), "readonly")

    def test_legacy_duplicate_live_name_is_shadowed(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, _, entry_start, _ = entries(b, base)
        a = next(e for e in es if e["name"] == b"a.txt")
        b_entry = next(e for e in es if e["name"] == b"b.txt")
        name = b"a.txt"
        raw = b[base + entry_start + b_entry["slot"] * 320:
                base + entry_start + (b_entry["slot"] + 1) * 320]
        raw = bytearray(raw)
        raw[38:40] = struct.pack("<H", len(name))
        raw[40:40 + len(name)] = name
        raw[40 + len(name):] = b"\0" * (320 - 40 - len(name))
        b[base + entry_start + b_entry["slot"] * 320:
          base + entry_start + (b_entry["slot"] + 1) * 320] = raw
        self.run_harness(bytes(b), "legacy-duplicate")

    def test_legacy_backing_id_without_base_flag_is_readable(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, _, entry_start, _ = entries(b, base)
        a = next(e for e in es if e["name"] == b"a.txt")
        struct.pack_into("<I", b, base + entry_start + a["slot"] * 320 + 12,
                         16)  # native .rockbox CNID
        self.run_harness(bytes(b), "legacy-baseid")

    def test_timestamps_are_exposed(self):
        self.run_harness(self.image, "timestamps")

    def test_duplicate_live_name_is_rejected(self):
        out = self.run_harness(self.image, "rw")
        self.run_harness(out, "rw", expect=1)

    def test_reserved_container_name(self):
        out = self.run_harness(self.image, "rw")
        self.run_harness(out, "reserved")

    def test_entry_table_full_maps_to_enospc(self):
        self.run_harness(self.image, "enospc")

    def test_duplicate_shadows_share_one_entry(self):
        out = self.run_harness(self.image, "rw")
        self.run_harness(out, "duplicate")

    def test_directory_rename_namespace_survives_remount(self):
        self.run_harness(self.image, "rename-parent")

    def test_bitmap_corruption_fails_closed(self):
        out = self.run_harness(self.image, "recovery")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, bitmap, _, _ = entries(b, base)
        victim = next(e for e in es if e["data_blocks"] > 0)
        for x in range(victim["data_start"],
                       victim["data_start"] + victim["data_blocks"]):
            b[base + bitmap + (x >> 3)] &= ~(1 << (x & 7))
        self.run_harness(bytes(b), "readonly")

    def test_overlapping_ranges_fail_closed(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, _, entry_start, _ = entries(b, base)
        victim = next(e for e in es if e["data_blocks"] > 0)
        other = next(e for e in es if e is not victim)
        struct.pack_into("<I", b, base + entry_start + other["slot"] * 320 + 24,
                         victim["data_start"])
        self.run_harness(bytes(b), "readonly")

    def test_duplicate_entry_id_fails_closed(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, _, entry_start, _ = entries(b, base)
        victim = next(e for e in es if e["data_blocks"] > 0)
        other = next(e for e in es if e is not victim)
        struct.pack_into("<I", b, base + entry_start + other["slot"] * 320,
                         victim["id"])
        self.run_harness(bytes(b), "readonly")

    def test_v2_backup_header_recovers(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        b[base + 40] ^= 0xff       # primary checksum, backup at block 1 intact
        self.run_harness(bytes(b), "recovery")

    def test_both_headers_bad_fails_closed(self):
        out = self.run_harness(self.image, "twofiles")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        b[base + 40] ^= 0xff
        b[base + 4096 + 40] ^= 0xff
        self.run_harness(bytes(b), "readonly")

    def test_failed_entry_write_keeps_existing_data(self):
        out = self.run_harness(self.image, "recovery")
        faulted = self.run_harness(out, "fault", fail_after=2)
        self.run_harness(faulted, "verify")


if __name__ == "__main__":
    if not ADAPTER_ARG:
        raise SystemExit("usage: test_adapter.py ./adapter_host")
    HARNESS = str(Path(ADAPTER_ARG).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
