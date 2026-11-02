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
ID_BASE = 0x40000000
FLAG_USED = 1
FLAG_TOMBSTONE = 8


def overlay_layout(b, base):
    magic = bytes(b[base:base + 8])
    if magic == b"RPOVL12\0":
        version = 2
    elif magic == b"RPOVL11\0":
        version = 1
    elif magic == b"RPOVL13\0":
        version = 3
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


def overlay_checksum40(hdr):
    value = 0
    for byte in hdr[:40]:
        value = ((value << 8) ^ (value + byte)) & 0xffffffff
    return (~value) & 0xffffffff


def overlay_checksum_v3(hdr):
    value = 0
    for i, byte in enumerate(hdr[:64]):
        if 40 <= i < 44:
            continue
        value = ((value << 8) ^ (value + byte)) & 0xffffffff
    return (~value) & 0xffffffff


def make_v3_header(total_blocks, bitmap_bytes, state, high_water=0):
    hdr = bytearray(4096)
    hdr[:8] = b"RPOVL13\0"
    bitmap_blocks = (bitmap_bytes + 4095) // 4096
    entry_blocks = (2048 * 320 + 4095) // 4096
    cache_start = 2 + bitmap_blocks + entry_blocks
    cache_blocks = 64
    data_start = cache_start + cache_blocks
    struct.pack_into("<7I", hdr, 8, 3, 4096, 2048, total_blocks,
                     bitmap_bytes, data_start, ID_BASE)
    struct.pack_into("<I", hdr, 36, 0)
    struct.pack_into("<4I", hdr, 44, high_water, cache_start,
                     cache_blocks, state)
    struct.pack_into("<I", hdr, 40, overlay_checksum_v3(hdr))
    return hdr


def v3_to_v2_header(b, base):
    bitmap_bytes = struct.unpack_from("<I", b, base + 24)[0]
    total_blocks = struct.unpack_from("<I", b, base + 20)[0]
    next_id = struct.unpack_from("<I", b, base + 32)[0]
    generation = struct.unpack_from("<I", b, base + 36)[0]
    bitmap_blocks = (bitmap_bytes + 4095) // 4096
    entry_blocks = (2048 * 320 + 4095) // 4096
    data_start = 2 + bitmap_blocks + entry_blocks
    hdr = bytearray(4096)
    hdr[:8] = b"RPOVL12\0"
    struct.pack_into("<7I", hdr, 8, 2, 4096, 2048, total_blocks,
                     bitmap_bytes, data_start, next_id)
    struct.pack_into("<I", hdr, 36, generation)
    struct.pack_into("<I", hdr, 40, overlay_checksum40(hdr))
    b[base:base + 4096] = hdr
    b[base + 4096:base + 8192] = hdr


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

    def run_diag(self, image):
        src = self.path("diag-in.hfs")
        dst = self.path("diag-out.hfs")
        src.write_bytes(image)
        p = subprocess.run([HARNESS, str(src), str(dst), "diag"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        fields = {}
        for token in p.stdout.split():
            key, _, value = token.partition("=")
            fields[key] = value
        return fields

    def two_file_overlay(self):
        b = bytearray(self.run_harness(self.image, "twofiles"))
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        es, bitmap, entry_start, _ = entries(b, base)
        files = [e for e in es if e["data_blocks"] > 0]
        self.assertGreaterEqual(len(files), 2)
        return b, base, bitmap, entry_start, files

    def assert_stage11(self, diag, slot, reason_name):
        self.assertEqual(diag["mount"], "0")
        self.assertEqual(diag["ready"], "0")
        self.assertEqual(diag["stage"], "11")
        self.assertEqual(diag["error"], "-2")
        self.assertEqual(diag["slot"], str(slot))
        self.assertEqual(diag["reason_name"], reason_name)

    def test_healthy_overlay_reports_no_reason(self):
        diag = self.run_diag(self.image)
        self.assertEqual(diag["mount"], "0")
        self.assertEqual(diag["ready"], "1")
        self.assertEqual(diag["stage"], "9")
        self.assertEqual(diag["slot"], "-1")
        self.assertEqual(diag["reason_name"], "none")

    def test_partial_zero_container_never_initializes_fresh(self):
        b, base, bitmap, entry_start, files = self.two_file_overlay()
        victim = files[0]
        raw = bytes(b[base + entry_start + victim["slot"] * 320:
                      base + entry_start + (victim["slot"] + 1) * 320])
        # Move one live entry beyond the first entry-table block, then make the
        # header, backup, bitmap and first entry block look freshly zeroed.
        slot = 100
        off = base + entry_start + slot * 320
        b[off:off + 320] = raw
        start = base + entry_start + victim["slot"] * 320
        b[start:start + 320] = b"\0" * 320
        bitmap_bytes = struct.unpack_from("<I", b, base + 24)[0]
        bitmap_blocks = (bitmap_bytes + fixtures.BLOCK - 1) // fixtures.BLOCK
        b[base:base + 2 * fixtures.BLOCK] = b"\0" * (2 * fixtures.BLOCK)
        b[base + bitmap:base + bitmap + bitmap_blocks * fixtures.BLOCK] = \
            b"\0" * (bitmap_blocks * fixtures.BLOCK)
        b[base + entry_start:base + entry_start + fixtures.BLOCK] = \
            b"\0" * fixtures.BLOCK

        diag = self.run_diag(bytes(b))
        self.assertEqual(diag["mount"], "0")
        self.assertEqual(diag["ready"], "0")
        self.assertEqual(diag["stage"], "3")
        # The loader must not rewrite the non-zero later entry block.
        out = self.path("diag-out.hfs").read_bytes()
        self.assertEqual(out[off:off + 320], raw)

    def test_flush_counters_are_live(self):
        # The first mount of a zero container performs fresh initialization and
        # header/bitmap commits, so the counters must be non-zero.
        diag = self.run_diag(self.image)
        self.assertGreater(int(diag["io_flushes"]), 0)
        self.assertGreater(int(diag["io_reads"]), 0)
        self.assertGreater(int(diag["io_writes"]), 0)

    def test_v3_init_marker_resumes(self):
        b = bytearray(self.image)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        total_blocks = 16 * 1024 * 1024 // fixtures.BLOCK
        hdr = make_v3_header(total_blocks, 512, 1, 0)
        b[base:base + 4096] = hdr
        b[base + 4096:base + 8192] = hdr
        diag = self.run_diag(bytes(b))
        self.assertEqual(diag["mount"], "0")
        self.assertEqual(diag["ready"], "1")
        self.assertEqual(diag["stage"], "9")

    def test_v2_overlay_remains_readable(self):
        b = bytearray(self.run_harness(self.image, "rw"))
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        v3_to_v2_header(b, base)
        diag = self.run_diag(bytes(b))
        self.assertEqual(diag["mount"], "0")
        self.assertEqual(diag["ready"], "1")
        self.assertEqual(diag["stage"], "9")
        self.assertEqual(diag["index_cache"], "0")
        self.assertEqual(diag["overlay_version"], "2")

    def test_v3_index_cache_and_high_water(self):
        out = self.run_harness(self.image, "rw")
        b = bytearray(out)
        base = fixtures.CONTAINER_START * fixtures.BLOCK
        self.assertEqual(b[base:base + 8], b"RPOVL13\0")
        high_water, cache_start, cache_blocks, state = struct.unpack_from(
            "<4I", b, base + 44)
        self.assertGreater(high_water, 0)
        self.assertEqual(cache_blocks, 64)
        self.assertEqual(state, 0)
        self.assertNotEqual(cache_start, 0)
        self.assertEqual(b[base + cache_start * fixtures.BLOCK:
                           base + cache_start * fixtures.BLOCK + 6], b"RPIDX1")
        # A second mount must accept the cache and remain healthy.
        diag = self.run_diag(bytes(b))
        self.assertEqual(diag["ready"], "1")
        self.assertEqual(diag["stage"], "9")
        self.assertEqual(diag["index_cache"], "1")
        self.assertEqual(diag["overlay_version"], "3")

    def test_stage11_unallocated_range_reports_slot_and_reason(self):
        b, base, bitmap, _, files = self.two_file_overlay()
        victim = files[0]
        block = victim["data_start"]
        b[base + bitmap + (block >> 3)] &= ~(1 << (block & 7))
        self.assert_stage11(self.run_diag(bytes(b)), victim["slot"],
                            "data-unallocated")

    def test_stage11_overlapping_range_reports_slot_and_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim, other = files[0], files[1]
        struct.pack_into("<I", b, base + entry_start + other["slot"] * 320 + 24,
                         victim["data_start"])
        expected = max(victim["slot"], other["slot"])
        self.assert_stage11(self.run_diag(bytes(b)), expected, "data-overlap")

    def test_stage11_duplicate_id_reports_slot_and_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim, other = files[0], files[1]
        struct.pack_into("<I", b, base + entry_start + other["slot"] * 320,
                         victim["id"])
        expected = max(victim["slot"], other["slot"])
        self.assert_stage11(self.run_diag(bytes(b)), expected, "duplicate-id")

    def test_stage11_size_beyond_capacity_reports_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim = files[0]
        size = victim["data_blocks"] * fixtures.BLOCK + 1
        struct.pack_into("<Q", b, base + entry_start + victim["slot"] * 320 + 16,
                         size)
        self.assert_stage11(self.run_diag(bytes(b)), victim["slot"],
                            "data-size")

    def test_stage11_bad_parent_reports_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim = files[0]
        struct.pack_into("<I", b, base + entry_start + victim["slot"] * 320 + 4,
                         ID_BASE + 19)
        self.assert_stage11(self.run_diag(bytes(b)), victim["slot"], "parent")

    def test_stage11_bad_name_reports_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim = files[0]
        # Embed a NUL inside the stored name: decode accepts it, the validator
        # rejects strlen(name) != name_len.
        b[base + entry_start + victim["slot"] * 320 + 41] = 0
        self.assert_stage11(self.run_diag(bytes(b)), victim["slot"],
                            "entry-name")

    def test_stage11_tombstone_with_data_reports_reason(self):
        b, base, _, entry_start, files = self.two_file_overlay()
        victim = files[0]
        # USED|TOMBSTONE with a live data run; the tombstone checks reject it.
        struct.pack_into("<I", b, base + entry_start + victim["slot"] * 320 + 8,
                         FLAG_USED | FLAG_TOMBSTONE)
        self.assert_stage11(self.run_diag(bytes(b)), victim["slot"],
                            "tombstone")

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
