#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the read-only contents of an HFSPlus Rockboxed iPod overlay (`.rockpod-rw`).

macOS/Finder sees only the native HFS+ tree.  Files created by HFSPlus Rockboxed iPod, settings
and databases live inside .rockpod-rw.  This tool reconstructs their paths
from HFS+ CNIDs (st_ino) and overlay parent IDs and copies them to a normal
Mac folder for inspection or backup.  It never modifies the iPod.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import os
import stat
import sys
from pathlib import Path

import overlay_check

SKIP_NAMES = {
    ".rockpod-rw", ".Trashes", ".fseventsd", ".Spotlight-V100",
    ".TemporaryItems",
}
FLAG_USED = 1
FLAG_DELETED = 2
FLAG_BASE = 4
FLAG_TOMBSTONE = 8


def safe_name(name: str) -> str:
    name = name.replace("/", "_").replace("\0", "_")
    if name in ("", ".", ".."):
        name = "_" + name
    return name


def native_cnids(root: Path):
    """Map HFS+ catalog CNIDs (macOS st_ino) to paths relative to root."""
    mapping = {2: Path(".")}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES]
        for name in list(dirnames) + list(filenames):
            if name in SKIP_NAMES:
                continue
            path = Path(dirpath) / name
            try:
                st = path.lstat()
            except OSError:
                continue
            if stat.S_ISLNK(st.st_mode):
                continue
            try:
                rel = path.relative_to(root)
            except ValueError:
                rel = Path(name)
            mapping.setdefault(st.st_ino, rel)
    return mapping


def resolve_path(entry, by_id, native_map, memo, depth=0):
    ident = entry["id"]
    if ident in memo:
        return memo[ident]
    if depth > 2048:
        raise ValueError("overlay parent cycle")
    parent = entry["parent"]
    parent_entry = by_id.get(parent)
    if parent_entry is not None:
        if (parent_entry["flags"] & (FLAG_DELETED | FLAG_TOMBSTONE) or
                parent_entry["kind"] != 1):
            base = Path(f"by-cnid-{parent}")
        else:
            base = resolve_path(parent_entry, by_id, native_map, memo, depth + 1)
    else:
        base = native_map.get(parent, Path(f"by-cnid-{parent}"))
    path = base / safe_name(entry["name"])
    memo[ident] = path
    return path


def copy_range(src: Path, offset: int, length: int, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open("rb") as fin, dst.open("wb") as fout:
        fin.seek(offset)
        left = length
        while left:
            chunk = fin.read(min(left, 1024 * 1024))
            if not chunk:
                raise OSError(f"short read at offset {offset}")
            fout.write(chunk)
            left -= len(chunk)


def main() -> int:
    default_out = (Path.home() / "Documents" /
                   ("rockpod-overlay-export-" +
                    _dt.datetime.now().strftime("%Y%m%d-%H%M%S")))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ipod", type=Path, default=Path("/Volumes/IPOD"),
                        help="mounted iPod root in Apple Disk Mode")
    parser.add_argument("--output", type=Path, default=default_out,
                        help="destination folder for exported files")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and print paths without writing files")
    args = parser.parse_args()

    ipod = args.ipod.expanduser().resolve()
    container = ipod / ".rockpod-rw"
    if not ipod.is_dir() or not container.is_file():
        print(f"STOP: {container} was not found. Mount the iPod in Apple Disk Mode.",
              file=sys.stderr)
        return 2
    try:
        entries = overlay_check.parse(container)
    except overlay_check.OverlayError as exc:
        if "unknown overlay magic" in str(exc):
            print("STOP: .rockpod-rw is empty or not initialized. "
                  "Boot HFSPlus Rockboxed iPod once so it can create the overlay.", file=sys.stderr)
        else:
            print(f"STOP: cannot read overlay: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"STOP: cannot read overlay: {exc}", file=sys.stderr)
        return 2

    by_id = {e["id"]: e for e in entries}
    natives = native_cnids(ipod)
    memo = {}
    manifest = []
    exported = dirs = native = skipped = 0
    for entry in sorted(entries, key=lambda e: (e["parent"], e["name"])):
        rel = resolve_path(entry, by_id, natives, memo)
        flags = entry["flags"]
        if flags & (FLAG_DELETED | FLAG_TOMBSTONE):
            status = "skipped"
            skipped += 1
        elif entry["kind"] == 1:
            status = "directory"
            dirs += 1
            if not args.dry_run:
                (args.output / rel).mkdir(parents=True, exist_ok=True)
        elif entry["data_blocks"]:
            status = "exported"
            exported += 1
            if not args.dry_run:
                copy_range(container, entry["data_start"] * overlay_check.BLOCK,
                           entry["size"], args.output / rel)
        elif flags & FLAG_BASE:
            # A BASE entry still points at the native data fork.  The native
            # file is already visible on the Mac and is not duplicated here.
            status = "native"
            native += 1
        else:
            # Overlay-only empty file (for example an empty playlist/config).
            status = "exported"
            exported += 1
            if not args.dry_run:
                target = args.output / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"")
        manifest.append((status, rel.as_posix(), entry["id"], entry["parent"],
                         entry["kind"], entry["size"], flags, entry["base_id"]))
        print(f"{status:9} {rel}")

    if not args.dry_run:
        args.output.mkdir(parents=True, exist_ok=True)
        with (args.output / "manifest.tsv").open("w", encoding="utf-8") as f:
            f.write("status\tpath\tid\tparent\tkind\tsize\tflags\tbase_id\n")
            for row in manifest:
                f.write("\t".join(str(v) for v in row) + "\n")
        (args.output / "README.txt").write_text(
            "Read-only export of the HFSPlus Rockboxed iPod .rockpod-rw overlay.\n"
            "Files marked 'native' are already visible in the native HFS+ tree.\n"
            "This export is not merged back into the iPod by this tool.\n",
            encoding="utf-8")
    print(f"Exported {exported} files, {dirs} directories; "
          f"{native} native, {skipped} skipped.")
    if args.dry_run:
        print("Dry run: no files were written.")
    else:
        print(f"Output: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        raise SystemExit(f"STOP: {exc}")
