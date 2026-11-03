# HFSPlus Rockboxed iPod V1.1 — release notes

Release tag: `v1.1`
Base: `main` (V12.0) plus the V1.1 release-blocker fixes

## Highlights

- Overlay validation now fails closed instead of reusing live blocks, and
  realloc/shrink/delete commit the entry before freeing data.
- Overlay load failures and invalid entry IDs can no longer fall back to
  native HFS+ writes or redirect a native name to overlay data.
- HFS+ names are bounded before every `fat_direntry` fill.
- Base-directory renames keep pre-existing overlay children and tombstones
  visible/consistent.
- The bootloader mounts the native HFS+ namespace only, so a bad overlay
  cannot hide `.rockbox`/`rockbox.ipod` or trigger fresh initialization at
  boot.
- HFS+ catalog parent-key ordering is validated before the directory index is
  used.
- PictureFlow index/cache parsing is bounds-checked; the on-device index
  rebuild no longer underflows its buffer or treats the `<Untagged>` name
  offset as an album count.
- PictureFlow track-index locking and no-artist reselect are fixed.
- Settings/theme commits are fsynced and forced after an explicit config/theme
  load; the normal `.new` promotion path is covered by the host rename test.
- Partition scanning parses tables before raw probing, tries both GPT unit
  sizes, and continues past malformed entries.
- Non-overlay extending writes are rejected before modifying any bytes, and
  failed mounts unwind the volume state.

## Fixed audit items

HFS-001, HFS-002, HFS-003, HFS-004, HFS-005, HFS-006, HFS-007, HFS-008,
HFS-009, HFS-010, HFS-011, HFS-014, HFS-021, HFS-022, HFS-025, HFS-039, HFS-047,
HFS-048, HFS-069, HFS-075, HFS-082, HFS-083, HFS-084, HFS-087, HFS-090,
HFS-094, HFS-102, HFS-103, HFS-104, HFS-106, HFS-108, HFS-109, HFS-112,
HFS-113, HFS-114, HFS-116, HFS-117, HFS-118, HFS-120, HFS-123, HFS-124,
HFS-125, HFS-126, HFS-127, HFS-129, HFS-130, HFS-131, HFS-133.

The remaining OPEN findings in `BUGFIXES.md` are real and are **not** claimed
fixed by this release. In particular, native allocation-file validation
(HFS-121/122), secondary volume-header recovery (HFS-132), and the broader
performance/format work remain open.

## Verification

- `make -C tools/hfsplus test`: 19 reader tests + 7 partition tests pass;
  the optional native-adapter test is skipped without `HFS_RB_TESTS`.
- Firmware built for `ipod6g`; bootloader built with `--thumb`; database host
  tool built; `mks5lboot` built; DFU image generated.
- Host adapter harness: overlay bitmap corruption → read-only with no native
  writes; injected entry-write failure → `a.bin` unchanged; long native name
  → open rejected; delete-child/rename-parent and create-child/rename-parent
  kept the same namespace after remount; two handles shared one shadow;
  unchanged BASE close leaked no blocks; EOF write rejected without a prefix;
  invalid index unwound the mount; colliding overlay ID was rejected.
- A normal boot of the tested iPod succeeded with the existing overlay. The
  `config.cfg` save/rename path was reproduced on the host. The final
  theme → idle → clean power-off → reboot check on the iPod is still the
  required device acceptance test for HFS-133.

## Assets

- `rockbox.zip` — complete `.rockbox` installation archive
- `rockbox.ipod` — main firmware image
- `bootloader-ipod6g.ipod` — Thumb HFS+ bootloader
- `bootloader-ipod6g.dfu` — DFU installer image
- `mks5lboot` — macOS DFU utility
- `database.ipod6g` — macOS database host tool

## Installation

1. Back up the iPod and `.rockpod-rw`.
2. Extract `rockbox.zip` over the mounted iPod root so that
   `.rockbox/rockbox.ipod` is replaced.
3. A firmware-only update does not require a new bootloader. The DFU assets
   are supplied for the separate bootloader installation described in
   `README.md`/`shipping.md`.
4. Do not use on-device Database Initialize/Update or PictureFlow Rebuild;
   use the Mac database/index/cache workflow.
5. After a clean reboot, verify playback, Cover Flow, settings persistence and
   the selected theme before treating the update as accepted.
