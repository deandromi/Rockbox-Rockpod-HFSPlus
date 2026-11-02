# Rockpod HFS+ V3.2 — Thermo-Nuclear

Release tag: `v3.2.0`
Public version string: `3.2.0-v13.0`
Status: **stable release**.
Previous stable: **V3.0 // Version V13.0**.
Target: **iPod Classic 6G (`ipod6g`) only**.

## Update type

**Both firmware and bootloader.**

- **Firmware:** `rockbox.ipod` and `rockbox.zip` (contains
  `.rockbox/rockbox.ipod`, codecs, plugins, themes, fonts and languages).
- **Bootloader:** `bootloader-ipod6g.ipod` and the DFU installer
  `bootloader-ipod6g.dfu`.

Installing the firmware alone gives you the V3.2 firmware features but not the
bootloader changes. Installing the bootloader alone does not update the
firmware. A full install uses both asset groups.

## Why Thermo-Nuclear?

Because this is not a scented-candle optimization pass. Thermonuclear fuel has
the highest energy density we know how to build, and that is the whole joke:
this release tries to get more useful work out of every joule, ATA transaction,
PMU write, catalog scan and tag read. Same battery capacity, fewer wasted
reactions — and on the test iPod, almost two more hours of runtime.

The safety analogy is the serious half. The reaction is contained in
`/.rockpod-rw`; native HFS+ is the shielding and is never written; a bad state
fails closed instead of melting down. The P0 stress gates are recorded honestly
below, so please keep a backup even though this is no longer a beta.

## What changed, and what you will notice

| Area | What changed | What you will notice |
|---|---|---|
| Boot reliability | The experimental periodic storage idle-notify path was removed; the V3.1 ATA/backlight power paths were restored after a sleep/wake regression. | No 4–6 second boot freeze and no dead sleep/hold state. Sleep by hold or inactivity wakes and responds. |
| Plugin data files | The HFS+ reader now follows the extents-overflow B-tree for native data files, read-only, up to 128 overflow extents. | Large/fragmented plugin data files (WAD, PAK, GRP) that were invisible before now appear and load. Files needing more than 128 overflow extents, hardlinks or symlinks remain unsupported. |
| Directory and path lookup | HFS+ v3 keeps a persistent directory/CNID index cache; repeated path lookups use a small read-only name cache; v3 readdir walks parent buckets instead of all 2,048 slots. | Browsing and repeated file opens are faster once the cache is warm. The first boot after a macOS Disk Mode write rebuilds the index once; later boots report `Index cache: HIT`. |
| Overlay writes | v3 entry high-water reserves 64 slots per header flush; validator/rebuild loops are bounded by the high-water mark; overlay realloc/materialize copies use 16 KiB chunks. | Fewer overlay header flushes and fewer full-table scans during creates and edits. |
| Database browsing | Persistent filename lookup, tagcache fill/title path without dircache, browser cursor preservation, and 64-entry RAM-index batching. | The database browser and playlist rows do less repeated disk scanning; large libraries feel less sluggish after the cache is warm. |
| Album art | Per-directory positive/negative result cache for external art. Track-specific art is still checked first and is never cached. | Folder art is not re-probed for every track in the same album. |
| Playback startup | Redundant track-load existence checks were removed; the playlist viewer only parses metadata for title display modes; read-only streams use a 4 KiB read-ahead window. | Less repeated metadata work when starting tracks and browsing playlists; small metadata reads need fewer ATA transactions. |
| Diagnostics | `Debug → Disk Info` reports index-cache HIT/MISS, high-water, per-operation create/write/close/rename/remove/settings counts, and flush attribution. | You can see whether the cache is warm and where overlay operations/flushes are going. |
| Power and battery life | The experimental ATA/backlight power state machine was not shipped; stable V3.2 keeps the known-good V3.1 power path. The efficiency work is in fewer ATA transactions, catalog scans, tag lookups and overlay flushes. | Owner-reported on the test iPod: almost **2 hours longer battery life**. V3.0 introduced most of the architectural savings; V3.2 fixes the V3.0-era bugs that masked them, so the improvement is finally noticeable in daily use. See the battery note below. |

## Battery life

Owner-reported result on the affected iPod Classic 6G with iFlash SD: almost
**two hours longer runtime** with V3.2 than before.

V3.0 did most of the architectural efficiency work — the v3 overlay, persistent
index cache, high-water entry reads and reduced validation. V3.2 fixes the
V3.0-era bugs and regressions that kept those savings from being felt, and adds
further work on path lookup, tagcache/album-art locality, overlay batching and
read-only stream reads. The result is that the same efficiency is finally
visible in daily use, including battery life.

This is a single-device owner observation, not a controlled power measurement.
The formal P0 power-loss matrix, native-integrity stress run and 100-cycle
SSD/spindown test remain unexecuted.

## Compatibility and upgrade notes

- V3.2 uses the **v3 overlay** (`RPOVL13`). An existing v3 overlay is kept;
  settings, playlists, tagcache and generated art remain.
- v1/v2 overlays are **not migrated**. Back them up only as recovery material.
- Native HFS+ catalog, allocation bitmap, extents tree, volume header,
  alternate header and journal are never written. All mutations stay in
  `/.rockpod-rw`.
- Rockbox USB mass storage remains **read-only**. Use Apple Disk Mode for
  writes.
- Journaled volumes must be clean; the journal is never replayed.
- APFS/HFSX, resource forks, symlinks, hardlinks, compression and encryption
  remain unsupported.
- Extents-overflow support is read-only and limited to 128 overflow extents;
  metadata forks and the overlay container must stay inline.

## Verification

Host:

- 21 reader tests, including a synthetic native file with one inline and eight
  overflow extents.
- 7 partition tests (1 native-adapter case skipped).
- 39 adapter tests, including overlay fault injection, native-region integrity
  and a native extents-overflow read.
- ASan/UBSan passes.
- macOS integration passes.
- Firmware, plugins, `rockbox.zip`, Thumb bootloader, DFU and database host
  tool build.

Device, on one iPod Classic 6G with iFlash SD:

- boot, overlay `READY` / stage 9 / error 0 / slot -1 / overlay format v3;
- sleep by hold and inactivity wakes and responds;
- plugin data files that previously appeared missing now load;
- normal daily use confirmed by the owner.

**Still not executed:** the formal P0 physical power-loss matrix, the full
native HFS+ integrity stress/`diskutil verifyVolume` run, and the 100-cycle
SSD/spindown resume test. This is a stable release by owner decision, not a
claim that those stress gates passed. Keep a verified backup.

## Rollback

Reinstall **V3.0** or **V3.1 Beta 5** if needed. Do not format, erase, restore
or delete `/.rockpod-rw`. Keep a verified overlay backup.

## Assets and SHA-256

```text
4ba94ada592da5d350bf679330c6226c2b60cb6a8b9aa7d2ed0d61cf7abca578  rockbox.ipod
a0396ac6824ca5a8ae0b051abb33646a7efeba71ff2f99db5751c6bd93aa2c3b  rockbox.zip
6ba0afcda44e86e6034ccc168a0a8e2753f6132dea5ae03715ff5583ba74d8a0  bootloader-ipod6g.ipod
a4574ab301dc6803f3a76be2bc40f1d62613fd79cb5331406f2d747480e997c9  bootloader-ipod6g.dfu
```

V3.2 supersedes V3.0 as the current stable release.
