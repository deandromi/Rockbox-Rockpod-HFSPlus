# HFSPlus Rockboxed iPod V3.0 Beta — v3 overlay, power and adapter decomposition

Release tag: `v3.0-beta`
Public version string: `3.0.0-beta-v13.0`
Status: **beta** — host/build verified; hardware validation for the new v3
overlay and index cache is still pending.

## Why V3.0

This release replaces the 2,797-line HFS+ adapter monolith with focused
modules, fixes the remaining overlay-initialization and timestamp defects, and
introduces the v3 overlay format with a resumable initializer, a high-water
entry bound, and a persistent directory/CNID index cache.

## Overlay format v3 (`RPOVL13`)

- New containers initialize as v3. v1 (`RPOVL11`) and v2 (`RPOVL12`) remain
  readable; existing v2 containers are **not** force-migrated and do not gain
  the v3 cache/high-water features.
- The v3 header is checksummed over the extended fields.
- `state=INIT` makes first initialization resumable after power loss. The
  initializer writes its marker first, then the bitmap/entry table/cache, then
  publishes `state=READY`.
- `entry_high_water` bounds the entry-table read. The bound is persisted before
  a newly allocated slot is exposed.
- A 64-block (256 KiB) reserved index-cache area stores the directory/CNID
  index with a generation checksum. The generation covers the native volume
  write count/modify date, catalog fork size, block count and node counts. A
  stale or torn cache is rejected and rebuilt.
- Fresh-container detection now scans **all** bitmap, entry-table and cache
  blocks before initializing. Non-zero metadata anywhere fails closed.

## Adapter decomposition

`firmware/hfsplus/hfsplus_rb.c` no longer exists. The adapter is split into:

`hfsplus_io`, `hfsplus_overlay_format`, `hfsplus_overlay_alloc`,
`hfsplus_overlay_store`, `hfsplus_overlay_validate`, `hfsplus_overlay_load`,
`hfsplus_namespace`, `hfsplus_fat`, `hfsplus_mount`, `hfsplus_state`,
`hfsplus_diag`, `hfsplus_time` and `hfsplus_index_cache`.

The validator returns a result struct, the bootloader self-selects the
native-only lifecycle, and the public API remains `hfsplus_rb.h`.

## Power and efficiency

- Overlay container writes no longer revalidate the immutable container fork
  or reread the native allocation bitmap on every write.
- The v3 index cache avoids the full catalog index scan on ordinary boots.
- `entry_high_water` avoids reading all 2,048 entry slots for small overlays.
- A redundant entry sync after a successful realloc was removed.
- Debug → Disk Info now reports index-cache hit/miss and live ATA
  read/write/flush counters.
- ATA flush batching beyond these changes remains hardware-gated; the
  persist-before-expose ordering is unchanged.

## Bug fixes

- Fresh initialization can no longer erase a container whose later metadata
  blocks are non-zero.
- Overlay timestamps no longer mix Unix and HFS epochs; overlay files show the
  current year rather than 1980.
- Duplicate overlay IDs are validated independently of legacy shadowing.
- v1 backup probing works even when the primary magic is unreadable.
- Unmount records header-sync failure instead of silently clearing the dirty
  flag.
- The remaining native readdir `strcpy` was replaced with a bounded copy.

## Upgrade notes

- **Bootloader:** this release ships a new Thumb bootloader and DFU image. The
  V3.0 beta pair is firmware + bootloader; use the matching artifacts for the
  tested configuration.
- **Existing v2 overlays** keep working read-only-to-v3 features as v2: no
  forced migration, no cache area, full entry-table scan. A migration tool is
  not included.
- **New/fresh overlays** initialize as v3.
- Use Apple Disk Mode and the Mac database/index/artwork workflow. Rockbox USB
  mass storage remains unsupported.

## Verification

- 20 reader tests, 7 partition tests and 30 host adapter tests pass.
- ASan/UBSan passes on macOS.
- macOS host integration mode passes.
- Firmware, Thumb bootloader, `rockbox.zip`, DFU image and `mks5lboot` build.
- No hardware boot, power-loss matrix or power measurement has been recorded
  for the v3 overlay/cache changes. Treat this as a beta.

## Assets and SHA-256

```text
1221d6b97125703e8cdd5379bd7ba9303293bc2e68ba2df6eee52a51a2e49bb6  rockbox.ipod
c4b5d0fc0c0ea1ba9b7ce0d109a96ab9389736e18fa61c3c198e924e23312971  rockbox.zip
e398b95d087dfb676fbc6dbc21c46445bbe47e6ef6ba84d8b6ef80e0f3ec4359  bootloader-ipod6g.ipod
f557dd5b7d7e19710b00efb4c8138515ca7f538916334576227d4548849625e4  bootloader-ipod6g.dfu
25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b  mks5lboot
8028ee85132c0905cb35ea494c88bbefcfe2d1a3d870aedc3b58c1be0274c0f0  database.ipod6g
```

## Known limits

- v1/v2 overlays are not migrated to v3.
- Single HFS+ volume per `ipod6g` build; HFSX/APFS are not supported.
- Clean HFS+ only; no journal replay.
- Eight inline extents; extents-overflow trees fail closed.
- Exact-byte names; no Unicode normalization/case folding.
