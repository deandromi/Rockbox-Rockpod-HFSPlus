# HFSPlus Rockboxed iPod V3.2 Beta 4 — Thermo-Nuclear

Release tag: `v3.2-beta4`
Public version string: `3.2.0-beta4-v13.0`
Status: **experimental beta**. Not a stable release.
Current stable remains **V3.0 // Version V13.0**.
Supersedes: **V3.2 Beta 3** (`v3.2-beta3`).

## Update type

**Both firmware and bootloader.**

- **Firmware (required for this fix):** `rockbox.ipod` and `rockbox.zip`.
- **Bootloader:** rebuilt because the HFS+ reader is shared with the
  bootloader. Reinstalling it is optional if the device already boots Beta 3.

## What changed in Beta 4

### Read-only extents-overflow support

The HFS+ reader previously supported only the **eight inline extent
descriptors** in a file record. A fragmented data file that needed the
extents-overflow B-tree was marked unsupported and disappeared from directory
listings. Large plugin data files (WADs, PAKs, GRPs) are the usual victims:
the plugin reports a missing file even though it exists in Apple Disk Mode.

Beta 4 adds a read-only extents-overflow reader:

- parses the volume's extents B-tree;
- follows leaf records for the requested file ID;
- caches up to **128 overflow extents** for sequential reads;
- keeps the eight inline extents as the first part of the fork.

Files with overflow extents now appear in `readdir`, open normally, and read
correctly. Writing such a file materializes it into `/.rockpod-rw` through the
normal overlay path; the native extents tree is never modified.

Metadata forks, the overlay container, and files requiring more than 128
overflow extents remain unsupported and fail closed.

### Other fixes retained

- Beta 2 freeze fix: the periodic early-boot storage idle-notify path is gone.
- Beta 3 sleep/wake rollback: the V3.1 ATA/backlight power paths remain.
- Tagcache batch-read hardening and the rest of the V3.2 efficiency work are
  unchanged.

## Verification

Host:

- 21 reader tests, including a new synthetic file with one inline extent and
  eight overflow extents; full and unaligned reads match.
- 7 partition tests (1 native-adapter case skipped).
- 39 adapter tests, including a native extents-overflow read through the FAT
  adapter.
- ASan/UBSan passes.
- macOS integration passes.

Device: not re-run for this build. Beta 3 sleep/wake and cache behavior were
confirmed on the affected iFlash SD device; the Beta 4 reader change still
needs a plugin/file test on hardware.

**Still not executed:** P0 physical power-loss matrix, full native HFS+
integrity stress/`diskutil verifyVolume` run, and the 100-cycle SSD/spindown
resume test.

## Rollback

Reinstall **V3.0** or **V3.1 Beta 5** firmware if needed. Do not format, erase,
restore or delete `/.rockpod-rw`. Keep a verified overlay backup.

## Assets and SHA-256

```text
53503e97a38afcc0c14abead8a7d7d9125f8cdb971171f98c3c528109c9282dc  rockbox.ipod
a21e291b1adf60a5848dc3dcaa610624d7915659a2533b34d918523702902b36  rockbox.zip
9039f3b8392600f55395dce4ab73f6118b9dc200d6e55ac490cbf88a22c5abe6  bootloader-ipod6g.ipod
955a2f1f275abdc81daa3ecc9cd6bc8e4374423bb6439f4f8f80f7f0037b7060  bootloader-ipod6g.dfu
```
