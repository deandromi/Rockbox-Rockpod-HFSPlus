# Rockpod HFS+ V2.0 — release notes

Release tag: `v2.0`
Base: `main` (V12.1) plus the V2.0 filesystem-hardening work
Public version string: `2.0-v12.2`

## Highlights

- Native HFS+ allocation-file validation: writable file/container forks must be
  allocated in the HFS+ bitmap and must not overlap the volume header,
  alternate header, journal, catalog, extents, attributes, startup or
  allocation metadata.
- Alternate volume-header recovery: a torn primary header mounts read-only from
  a validated alternate copy with an explicit `recovered` diagnostic. Both
  copies corrupt fail closed.
- Overlay v2 (`RPOVL12`): header checksum plus a backup header in block 1.
  Legacy `RPOVL11` containers remain readable; v1 gets a checksum and an
  in-block backup when its header is next synced.
- Overlay ownership checks before freeing blocks; duplicate live names and
  duplicate BASE/CNID shadows are rejected; reserved `.rockpod-rw` names are
  rejected; deleted/tombstoned entries cannot be read, written, renamed or
  chmodded through a stale handle.
- Parent/orphan/cycle validation and a bounded parent chain; the parent hash is
  now used for namespace lookups instead of a 2,048-entry scan per base entry.
- Entry-table exhaustion maps to `ENOSPC`; a full overlay is no longer reported
  as `EIO`.
- Per-volume mount/overlay diagnostics; the bootloader only prints HFS+
  diagnostics when an HFS+ candidate was actually probed.
- CNID hash index and one-time catalog-node offset validation; the directory
  index now covers 16,384 parents, with a validated full-scan fallback.
- HFS+ timestamps are exposed as DOS date/time; base-file `modtime()` creates a
  shadow instead of silently succeeding.
- Non-inline forks are marked unsupported at open, not at a later read/write.
- Partition probes validate the primary or alternate volume header before
  claiming a partition as HFS+; HFSX is rejected as not-HFS so the FAT fallback
  can run; mixed partition units are rejected; the iPod 16510 fallback is now a
  target macro.
- PictureFlow/cache installs write a temporary file and replace atomically;
  old artwork is kept on failure; parent directories are fsynced; the Cover
  Flow conflict checker validates the overlay and matches names, tombstones and
  HFS+ CNIDs (`st_ino`).
- A read-only Mac overlay exporter (`tools/rockpod/export_overlay.py`)
  reconstructs overlay paths through native HFS+ CNIDs so files created
  inside `.rockpod-rw` can be inspected or backed up from Finder-visible
  storage.
- A committed host adapter harness (`make -C tools/hfsplus test`) exercises
  read/write/remount, ENOSPC, duplicate shadows, directory renames, bitmap
  corruption, overlapping runs, duplicate IDs, v2 header recovery, container
  metadata/unallocated extents, timestamps and injected entry-write failure.
- `make sanitize` is portable on macOS (no Linux-only PIE flags).
- `make zip` excludes `.DS_Store`, `._*`, VCS directories and editor backups.

## Audit status

All 109 unique audit IDs are now accounted for:

- **103 FIXED** — code changed and covered by host/build tests.
- **6 DOCUMENTED** — unavoidable format/compatibility limits, with unsafe
  cases rejected:
  - HFS-042/099: non-overlapping logical extent reordering cannot be detected
    without a data checksum; overlap/range/metadata checks are enforced.
  - HFS-044: HFS+ Unicode normalization/case folding is not implemented;
    lookup is exact-byte.
  - HFS-049: `/`↔`:` name mapping limits some legal HFS+ names.
  - HFS-095: extended/logical MBR partitions are not traversed; supported iPod
    layouts use APM, primary GPT or primary MBR.
  - HFS-101: volume labels, volume creation dates and POSIX permissions are not
    exposed to Rockbox.

The V1.1 release notes remain at `docs/release-v1.1.md`; `BUGFIXES.md` contains
the per-ID status and evidence.

## Verification

- `make -C tools/hfsplus test`: 19 reader tests + 7 partition tests + 15 host
  adapter tests pass.
- `make -C tools/hfsplus sanitize`: the reader, partition and adapter suites
  pass under AddressSanitizer/UBSan on macOS.
- `tools/hfsplus/tests/check_integration.py`: macOS host-adapter mode passes;
  the full freestanding syntax path remains the Linux/GCC path.
- Firmware built for `ipod6g`; Thumb bootloader built with `--thumb`; database
  host tool built as a native macOS executable; `mks5lboot` and the DFU image
  built.
- `rockbox.zip` integrity checked with `unzip -t`; no `.DS_Store`/`._*` files.
- V1.1 was installed and booted on the tested iPod Classic 6G. V2.0 itself was
  host/build-verified in this pass and was not installed on hardware. The
  controlled theme → idle → clean power-off → reboot test remains the required
  hardware acceptance check (HFS-133).

## Assets

- `rockbox.zip` — complete `.rockbox` installation archive
- `rockbox.ipod` — main firmware image
- `bootloader-ipod6g.ipod` — Thumb HFS+ bootloader
- `bootloader-ipod6g.dfu` — DFU installer image
- `mks5lboot` — macOS DFU utility
- `database.ipod6g` — macOS database host tool

## SHA-256

```text
331e0cf990cbe7e02c6d522fa1c4d1b485d3d230e0bc5721fde6aa712af6fd19  rockbox.ipod
5ab3e1db9d94387be26b588f654a11d4e06d9ee8b2b0f95e4f08018923f46201  rockbox.zip
beeffd5ed8ad09ab4b52083a49b3d7a9f9d4d1329b440887bd333f69f1409855  bootloader-ipod6g.ipod
9a257e8f1cd6269901bb7bff1d263af43013df14fca48070cc9c6786e2b26db9  bootloader-ipod6g.dfu
8028ee85132c0905cb35ea494c88bbefcfe2d1a3d870aedc3b58c1be0274c0f0  database.ipod6g
25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b  mks5lboot
```

## Installation

1. Back up the iPod and `.rockpod-rw`.
2. Extract `rockbox.zip` over the mounted iPod root so that
   `.rockbox/rockbox.ipod` is replaced.
3. A firmware-only update does not require a new bootloader. The DFU assets
   are supplied for the separate bootloader installation described in
   `README.md`/`shipping.md`.
4. Use Apple Disk Mode for transfers. Do not use Rockbox USB mode for bulk
   transfers, and do not use on-device Database Initialize/Update or
   PictureFlow Rebuild; use the Mac database/index/cache workflow.
5. After a clean reboot, verify playback, Cover Flow, settings persistence and
   the selected theme.

## Upgrade notes

- Existing `RPOVL11` containers are mounted as-is and remain writable. Their
  header gets a checksum and in-block backup on the next clean unmount.
- New containers use `RPOVL12` with a separate backup block. No manual
  conversion is required.
- If the native HFS+ allocation file itself cannot be described by its eight
  inline extent descriptors, writes fail closed. Run `diskutil verifyVolume`
  and keep the Mac-host workflow; do not bypass the overlay.
