#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic format fixtures, not a substitute for macOS image/hardware tests."""
import hashlib
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

BLOCK = 4096
BLOCKS = 256
CAT = 5 * BLOCK
UNICODE_NAME = "E\u0301tude 🎵.bin"
PAYLOAD = bytes((i * 37 + 19) % 256 for i in range(5003))
BINARY = str(Path(sys.argv[1]).resolve())
sys.argv = sys.argv[:1]


def put16(b, o, n):
    struct.pack_into(">H", b, o, n)


def put32(b, o, n):
    struct.pack_into(">I", b, o, n)


def fork(size, extents, total=None):
    raw = bytearray(80)
    struct.pack_into(">QII", raw, 0, size, 0,
                     sum(n for _, n in extents) if total is None else total)
    for i, (start, count) in enumerate(extents):
        struct.pack_into(">II", raw, 16 + i * 8, start, count)
    return raw


def key(parent, name):
    encoded = name.encode("utf-16-be")
    return struct.pack(">HIH", len(encoded) + 6, parent, len(encoded) // 2) + encoded


def folder(parent, name, cnid, children):
    data = bytearray(88)
    struct.pack_into(">HHII", data, 0, 1, 0, children, cnid)
    put16(data, 42, 0o40755)
    return key(parent, name) + data


def file_record(parent, name, cnid, size, extents, mode=0o100644, flags=0):
    data = bytearray(248)
    struct.pack_into(">HHII", data, 0, 2, 2, 0, cnid)
    put16(data, 42, mode)
    data[41] = flags
    data[88:168] = fork(size, extents)
    return key(parent, name) + data


def thread(cnid, parent, name, is_folder=False):
    encoded = name.encode("utf-16-be")
    data = struct.pack(">HHIH", 3 if is_folder else 4, 0, parent, len(encoded) // 2)
    return key(cnid, "") + data + encoded


def node(kind, height, records, forward=0, back=0, size=BLOCK):
    b = bytearray(size)
    struct.pack_into(">IIbBHH", b, 0, forward, back, kind, height, len(records), 0)
    off = 14
    for i, record in enumerate(records):
        put16(b, size - 2 * (i + 1), off)
        b[off:off + len(record)] = record
        off += len(record)
    put16(b, size - 2 * (len(records) + 1), off)
    assert off <= size - 2 * (len(records) + 1)
    return b


def header_node(depth, root, count, first, last, total, catalog=True):
    h = bytearray(106)
    struct.pack_into(">HIIIIHHII", h, 0, depth, root, count, first, last,
                     BLOCK, 516 if catalog else 10, total, 0)
    h[37] = 0xcf if catalog else 0
    put32(h, 38, 6 if catalog else 2)
    bitmap = bytearray(BLOCK - 256)
    bitmap[0] = 0xf0 if catalog else 0x80
    return node(1, 0, [h, bytes(128), bitmap])


def checksum(b):
    value = 0
    for byte in b:
        value = ((value << 8) ^ (value + byte)) & 0xffffffff
    return value ^ 0xffffffff


def image(journal=False, little=False):
    b = bytearray(BLOCK * BLOCKS)
    records = [
        folder(1, "Test volume", 2, 2),
        thread(2, 1, "Test volume", True),
        folder(2, ".rockbox", 16, 1),
        folder(2, "Music", 17, 3),
        thread(16, 2, ".rockbox", True),
        file_record(16, "rockbox.ipod", 18, 12, [(26, 1)]),
        thread(17, 2, "Music", True),
        file_record(17, "empty.bin", 21, 0, []),
        file_record(17, UNICODE_NAME, 20, 7, [(24, 1)]),
        file_record(17, "track.bin", 19, len(PAYLOAD), [(20, 1), (22, 1)]),
        thread(18, 16, "rockbox.ipod"),
        thread(19, 17, "track.bin"),
        thread(20, 17, UNICODE_NAME),
        thread(21, 17, "empty.bin"),
    ]
    leaves = [records[:7], records[7:]]
    index = []
    for i, leaf in enumerate(leaves):
        k = leaf[0][:struct.unpack_from(">H", leaf[0])[0] + 2]
        index.append(k + struct.pack(">I", i + 1))
    b[CAT:CAT + BLOCK] = header_node(2, 3, len(records), 1, 2, 4)
    b[CAT + BLOCK:CAT + 2 * BLOCK] = node(-1, 1, leaves[0], forward=2)
    b[CAT + 2 * BLOCK:CAT + 3 * BLOCK] = node(-1, 1, leaves[1], back=1)
    b[CAT + 3 * BLOCK:CAT + 4 * BLOCK] = node(0, 2, index)
    b[4 * BLOCK:5 * BLOCK] = header_node(0, 0, 0, 0, 0, 1, False)
    b[20 * BLOCK:21 * BLOCK] = PAYLOAD[:BLOCK]
    b[22 * BLOCK:22 * BLOCK + len(PAYLOAD) - BLOCK] = PAYLOAD[BLOCK:]
    b[24 * BLOCK:24 * BLOCK + 7] = b"unicode"
    b[26 * BLOCK:26 * BLOCK + 12] = b"TEST ONLY!!!"
    allocated = {0, 3, 4, 5, 6, 7, 8, 20, 22, 24, 26, BLOCKS - 1}
    vh = bytearray(512)
    struct.pack_into(">HHI", vh, 0, 0x482b, 4, (1 << 8) | ((1 << 13) if journal else 0))
    vh[8:12] = b"HFSJ" if journal else b"10.0"
    put32(vh, 32, 4)
    put32(vh, 36, 2)
    put32(vh, 40, BLOCK)
    put32(vh, 44, BLOCKS)
    put32(vh, 64, 22)
    vh[112:192] = fork(BLOCKS // 8, [(3, 1)])
    vh[192:272] = fork(BLOCK, [(4, 1)])
    vh[272:352] = fork(BLOCK * 4, [(5, 4)])
    if journal:
        put32(vh, 12, 2)
        info = bytearray(180)
        put32(info, 0, 1)
        struct.pack_into(">QQ", info, 36, 32 * BLOCK, 16 * BLOCK)
        b[2 * BLOCK:2 * BLOCK + len(info)] = info
        order = "<" if little else ">"
        h = bytearray(44)
        struct.pack_into(order + "IIQQQIII", h, 0,
                         0x4a4e4c78, 0x12345678, 512, 512, 16 * BLOCK,
                         4096, 0, 512)
        struct.pack_into(order + "I", h, 36, checksum(h))
        b[32 * BLOCK:32 * BLOCK + len(h)] = h
        allocated.update({2, *range(32, 48)})
    for n in allocated:
        b[3 * BLOCK + n // 8] |= 1 << (7 - n % 8)
    put32(vh, 48, BLOCKS - len(allocated))
    b[1024:1536] = vh
    b[-1024:-512] = vh
    return b


def record_data_offset(b, leaf, index):
    start = CAT + leaf * BLOCK
    off = struct.unpack_from(">H", b, start + BLOCK - 2 * (index + 1))[0]
    keylen = struct.unpack_from(">H", b, start + off)[0]
    return start + off + keylen + 2


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "fixture.hfs"

    def tearDown(self):
        self.temp.cleanup()

    def run_image(self, b, command="info", path=None, options=(), ok=True):
        self.path.write_bytes(b)
        before = hashlib.sha256(b).digest()
        cmd = [BINARY, *options, str(self.path), command]
        if path is not None:
            cmd.append(path)
        p = subprocess.run(cmd, capture_output=True, timeout=5)
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).digest(), before)
        self.assertNotIn(b"runtime error:", p.stderr)
        self.assertNotIn(b"AddressSanitizer", p.stderr)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
        else:
            self.assertIn(p.returncode, (1, 2), p.stderr.decode(errors="replace"))
        return p

    def test_volume_and_directory_listing(self):
        self.assertIn(b"block_size=4096", self.run_image(image()).stdout)
        listing = self.run_image(image(), "ls", "/").stdout
        self.assertIn(b".rockbox", listing)
        self.assertIn(b"Music", listing)
        self.assertNotIn(b"track.bin", listing)
        listing = self.run_image(image(), "ls", "/Music").stdout
        self.assertIn(UNICODE_NAME.encode(), listing)
        self.assertIn(b"track.bin", listing)

    def test_fragmented_binary_and_boot_file(self):
        self.assertEqual(self.run_image(image(), "cat", "/Music/track.bin").stdout, PAYLOAD)
        self.assertEqual(self.run_image(image(), "cat", "/.rockbox/rockbox.ipod").stdout,
                         b"TEST ONLY!!!")

    def test_unicode_and_empty_file(self):
        self.assertEqual(self.run_image(image(), "cat", "/Music/" + UNICODE_NAME).stdout, b"unicode")
        self.assertEqual(self.run_image(image(), "cat", "/Music/empty.bin").stdout, b"")

    def test_unaligned_partial_reads_and_eof(self):
        for start, size in [(3, 100), (4090, 30), (5000, 40), (9000, 10), (0, 0)]:
            with self.subTest(start=start, size=size):
                p = self.run_image(image(), "cat", "/Music/track.bin",
                                   options=("--skip", str(start), "--count", str(size)))
                self.assertEqual(p.stdout, PAYLOAD[start:start + size])

    def test_explicit_volume_window(self):
        b = bytes(4096) + image() + bytes(512)
        p = self.run_image(b, "cat", "/Music/track.bin",
                           options=("--offset", "4096", "--length", str(BLOCK * BLOCKS)))
        self.assertEqual(p.stdout, PAYLOAD)
        self.run_image(b, options=("--length", str(len(b) + 1)), ok=False)
        self.run_image(b, options=("--offset", str(len(b) + 1)), ok=False)

    def test_missing_and_unsupported_paths(self):
        for path in ["/missing", "/music", "/Music/../Music", "/./Music", "relative"]:
            with self.subTest(path=path):
                self.run_image(image(), "ls", path, ok=False)
        self.run_image(image(), "cat", "/Music", ok=False)
        self.run_image(image(), "ls", "/Music/track.bin", ok=False)
        self.run_image(image(), "cat", "/Music/track.bin/", ok=False)
        self.run_image(image(), "cat", "/Music/track.bin/child", ok=False)

    def test_clean_journals_both_endiannesses(self):
        for little in [False, True]:
            with self.subTest(little=little):
                b = image(journal=True, little=little)
                self.assertIn(b"journaled=1", self.run_image(b).stdout)
                self.assertEqual(self.run_image(b, "cat", "/Music/track.bin").stdout, PAYLOAD)

    def test_dirty_volume_flags(self):
        for attrs in [0, (1 << 8) | (1 << 11), 1 << 13]:
            b = image()
            put32(b, 1028, attrs)
            self.assertIn(b"error -4", self.run_image(b, ok=False).stderr)

    def test_pending_journal(self):
        for little in [False, True]:
            b = image(journal=True, little=little)
            h = bytearray(b[32 * BLOCK:32 * BLOCK + 44])
            order = "<" if little else ">"
            struct.pack_into(order + "Q", h, 16, 1024)
            struct.pack_into(order + "I", h, 36, 0)
            struct.pack_into(order + "I", h, 36, checksum(h))
            b[32 * BLOCK:32 * BLOCK + 44] = h
            self.assertIn(b"error -4", self.run_image(b, ok=False).stderr)

    def test_journal_errors(self):
        for flags in [0, 2, 3, 5]:
            b = image(journal=True)
            put32(b, 2 * BLOCK, flags)
            self.run_image(b, ok=False)
        b = image(journal=True)
        b[32 * BLOCK + 36] ^= 1
        self.run_image(b, ok=False)
        b = image(journal=True)
        b[1032:1036] = b"oops"
        self.run_image(b, ok=False)
        b = image(journal=True)
        struct.pack_into(">Q", b, 2 * BLOCK + 36, len(b) - 100)
        self.run_image(b, ok=False)

    def test_volume_bounds_and_signature(self):
        for off, value in [(1064, 0), (1064, 513), (1068, 0xffffffff), (1072, BLOCKS + 1)]:
            b = image()
            put32(b, off, value)
            self.run_image(b, ok=False)
        b = image()
        b[1024:1026] = b"HX"
        self.run_image(b, ok=False)
        self.run_image(image()[:1500], ok=False)

    def test_catalog_offsets_and_links(self):
        mutations = [
            (CAT + BLOCK + BLOCK - 2, b"\xff\xff"),
            (CAT + BLOCK + 8, b"\x00"),
            (CAT + BLOCK + 10, b"\xff\xff"),
            (CAT + BLOCK, struct.pack(">I", 1)),
            (CAT + BLOCK, struct.pack(">I", 99)),
            (CAT + 2 * BLOCK + 4, struct.pack(">I", 99)),
            (CAT + 2 * BLOCK, struct.pack(">I", 1)),
            (CAT + 14 + 6, struct.pack(">I", 100)),
        ]
        for off, value in mutations:
            with self.subTest(offset=off, value=value):
                b = image()
                b[off:off + len(value)] = value
                self.run_image(b, "ls", "/", ok=False)

    def test_catalog_header_rejects_unsupported_sizes(self):
        for size in [0, 513, 65535]:
            b = image()
            put16(b, CAT + 32, size)
            self.run_image(b, ok=False)

    def test_out_of_range_and_overlapping_extents(self):
        d = record_data_offset(image(), 2, 2)
        for off, value in [(d + 88 + 16, BLOCKS),
                           (d + 88 + 24, 20),
                           (d + 88 + 20, BLOCKS),
                           (d + 88 + 12, 1)]:
            b = image()
            put32(b, off, value)
            self.run_image(b, "cat", "/Music/track.bin", ok=False)

    def test_overflow_extents_fail_explicitly(self):
        b = image()
        d = record_data_offset(b, 2, 2)
        b[d + 88:d + 168] = fork(8 * BLOCK + 1,
                                [(100 + i * 2, 1) for i in range(8)], total=9)
        p = self.run_image(b, "cat", "/Music/track.bin", ok=False)
        self.assertIn(b"error -3", p.stderr)
        self.assertEqual(p.stdout, b"")

    def test_symlinks_compression_hardlinks_are_not_read(self):
        for kind in ["symlink", "compressed", "hardlink"]:
            b = image()
            d = record_data_offset(b, 2, 2)
            if kind == "symlink":
                put16(b, d + 42, 0o120777)
            elif kind == "compressed":
                b[d + 41] |= 0x20
            else:
                b[d + 48:d + 56] = b"hlnkhfs+"
            p = self.run_image(b, "cat", "/Music/track.bin", ok=False)
            self.assertEqual(p.stdout, b"")

    def test_invalid_utf16(self):
        b = image()
        start = CAT + 2 * BLOCK
        off = struct.unpack_from(">H", b, start + BLOCK - 4)[0]
        put16(b, start + off + 8, 0xdc00)
        self.run_image(b, "ls", "/Music", ok=False)

    def test_regular_file_only(self):
        p = subprocess.run([BINARY, "/dev/null", "info"], capture_output=True, timeout=5)
        self.assertEqual(p.returncode, 2)
        self.assertIn(b"regular image file", p.stderr)

    def test_deterministic_malformed_input_smoke(self):
        rng = random.Random(719)
        original = image()
        regions = [(1024, 1536), (CAT, CAT + 3 * BLOCK)]
        for i in range(80):
            b = bytearray(original)
            lo, hi = regions[i % len(regions)]
            for _ in range(1 + i % 7):
                pos = rng.randrange(lo, hi)
                b[pos] ^= rng.randrange(1, 256)
            self.path.write_bytes(b)
            p = subprocess.run([BINARY, str(self.path), "ls", "/"],
                               capture_output=True, timeout=5)
            self.assertIn(p.returncode, (0, 1), (i, p.stderr))
            self.assertNotIn(b"runtime error:", p.stderr)
            self.assertNotIn(b"AddressSanitizer", p.stderr)
            self.assertEqual(self.path.read_bytes(), b)


if __name__ == "__main__":
    unittest.main(verbosity=2)
