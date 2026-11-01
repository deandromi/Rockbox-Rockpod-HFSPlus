#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only validation and conflict checks for a Rockpod .rockpod-rw file.

This mirrors the adapter's persistent overlay validation.  It is used by the
Mac installers so a shadowing/tombstoned target is detected before any live
file is replaced.
"""
from __future__ import annotations

import struct
from pathlib import Path

BLOCK = 4096
ENTRY_SIZE = 320
MAX_ENTRIES = 2048
ID_BASE = 0x40000000
FLAG_USED = 1
FLAG_DELETED = 2
FLAG_BASE = 4
FLAG_TOMBSTONE = 8


class OverlayError(ValueError):
    pass


def _header_sum(raw: bytes) -> int:
    value = 0
    for byte in raw[:40]:
        value = ((value << 8) ^ (value + byte)) & 0xffffffff
    return (~value) & 0xffffffff


def parse(path: Path):
    file_size = path.stat().st_size
    with path.open("rb") as f:
        header = f.read(BLOCK)
        if len(header) < BLOCK:
            raise OverlayError("overlay is smaller than one block")
        magic = header[:8]
        if magic == b"RPOVL12\0":
            version = 2
        elif magic == b"RPOVL11\0":
            version = 1
        else:
            raise OverlayError(f"unknown overlay magic {magic!r}")
        block, count, total, bitmap_bytes, data_start, next_id, generation = \
            struct.unpack_from("<7I", header, 12)
        if block != BLOCK or count != MAX_ENTRIES:
            raise OverlayError("unsupported overlay layout")
        stored = struct.unpack_from("<I", header, 40)[0]
        if (version == 2 and not stored) or (stored and stored != _header_sum(header)):
            raise OverlayError("overlay header checksum mismatch")
        if total * BLOCK > file_size:
            raise OverlayError("overlay header exceeds file size")
        bitmap_start = 1 if version == 1 else 2
        bitmap_blocks = (bitmap_bytes + BLOCK - 1) // BLOCK
        entry_blocks = (MAX_ENTRIES * ENTRY_SIZE + BLOCK - 1) // BLOCK
        expected = bitmap_start + bitmap_blocks + entry_blocks
        if data_start != expected or data_start >= total:
            raise OverlayError("overlay data start is inconsistent")
        f.seek(bitmap_start * BLOCK)
        bitmap = f.read(bitmap_bytes)
        if len(bitmap) != bitmap_bytes:
            raise OverlayError("truncated overlay bitmap")
        entry_at = (bitmap_start + bitmap_blocks) * BLOCK
        f.seek(entry_at)
        table = f.read(MAX_ENTRIES * ENTRY_SIZE)
        if len(table) != MAX_ENTRIES * ENTRY_SIZE:
            raise OverlayError("truncated overlay entry table")
    entries = []
    ids = set()
    owned = []
    for slot in range(MAX_ENTRIES):
        raw = table[slot * ENTRY_SIZE:(slot + 1) * ENTRY_SIZE]
        ident, parent, flags, base_id = struct.unpack_from("<4I", raw, 0)
        if not flags & FLAG_USED:
            continue
        size, ds, db = struct.unpack_from("<QII", raw, 16)
        kind, name_len = struct.unpack_from("<HH", raw, 36)
        name_raw = raw[40:40 + name_len]
        if (ident < ID_BASE or ident >= 0x80000000 or ident in ids or
                name_len == 0 or name_len > 255 or len(name_raw) != name_len or
                b"\0" in name_raw or flags & ~(FLAG_USED | FLAG_DELETED |
                                               FLAG_BASE | FLAG_TOMBSTONE)):
            raise OverlayError(f"invalid overlay entry in slot {slot}")
        ids.add(ident)
        name = name_raw.decode("utf-8")
        if flags & FLAG_TOMBSTONE:
            if db or ds or size or not base_id or base_id >= ID_BASE:
                raise OverlayError(f"invalid tombstone in slot {slot}")
        else:
            if kind not in (1, 2) or base_id >= ID_BASE or (
                    flags & FLAG_BASE and not base_id):
                raise OverlayError(f"invalid object in slot {slot}")
            if db:
                if (ds < data_start or ds >= total or
                        db > total - ds or
                        size > db * BLOCK):
                    raise OverlayError(f"invalid range in slot {slot}")
                for block_no in range(ds, ds + db):
                    byte = bitmap[block_no >> 3]
                    if not byte & (1 << (block_no & 7)):
                        raise OverlayError(f"unallocated range in slot {slot}")
                for start, count in owned:
                    if ds < start + count and start < ds + db:
                        raise OverlayError(f"overlapping range in slot {slot}")
                owned.append((ds, db))
            elif size and not flags & FLAG_BASE:
                raise OverlayError(f"size without blocks in slot {slot}")
        entries.append(dict(slot=slot, id=ident, parent=parent, flags=flags,
                            base_id=base_id, size=size, data_start=ds,
                            data_blocks=db, kind=kind, name=name))
    by_id = {e["id"]: e for e in entries}
    for entry in entries:
        parent = entry["parent"]
        if parent < 2 or parent == entry["id"]:
            raise OverlayError(f"invalid parent in slot {entry['slot']}")
        while parent >= ID_BASE:
            parent_entry = by_id.get(parent)
            if (not parent_entry or parent_entry["flags"] &
                    (FLAG_DELETED | FLAG_TOMBSTONE) or
                    parent_entry["kind"] != 1):
                raise OverlayError(f"orphan/cyclic parent in slot {entry['slot']}")
            parent = parent_entry["parent"]
    return entries

def conflicts(path: Path, names, native_ids=None):
    """Return entries that could hide a target name or native CNID."""
    if not path.exists():
        return []
    entries = parse(path)
    target_names = {str(n) for n in names}
    native_ids = {int(v) for v in (native_ids or {}).values() if v}
    live_ids = {e["base_id"] for e in entries
                if e["flags"] & FLAG_BASE and e["name"] in target_names}
    found = []
    for entry in entries:
        flags = entry["flags"]
        if entry["name"] in target_names:
            found.append(entry)
        elif (flags & FLAG_TOMBSTONE and
              (entry["name"] in target_names or
               entry["base_id"] in native_ids or
               entry["base_id"] in live_ids)):
            found.append(entry)
    return found
