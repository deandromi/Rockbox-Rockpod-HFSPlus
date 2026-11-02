# Hardware validation roadmap

This is the hardware-side checklist for Rockpod HFS+ V2.1.1 and later. Host
tests and ARM links do not prove storage, power-loss or bootloader behavior.

**V3.2 Thermo-Nuclear:** the stable release evidence is recorded in
[`v3.2-validation.md`](v3.2-validation.md). Boot, sleep/wake, overlay READY,
plugin data files and daily use were confirmed on one iFlash SD unit. Its P0
power-loss, native-integrity and 100-cycle SSD-resume gates are **not
executed**; keep backups and treat the stress results as outstanding.

## Minimum before calling a build fully stable

### 1. Device matrix

- iPod Classic 6G 80 GB, 120 GB and 160 GB where available.
- Original rotating drive and at least one iFlash/SD or iFlash/CF adapter.
- A second Classic 6G unit if possible.
- Record model, storage adapter, card/SSD model, battery condition and macOS version.

### 2. Cold boot and startup

- Power-on to Rockbox with the V2.1+ Thumb bootloader.
- Compare boot time with V1.1/V2.0; the full directory/CNID index must not run in the bootloader.
- Test with a large library and with a nearly empty library.
- Test a warm reboot and a battery-disconnect cold boot.

### 3. Overlay lifecycle

- Fresh all-zero `.rockpod-rw`: v2 initialization.
- Existing v1 overlay: read/write, checksum/backup sync, no data loss.
- Legacy duplicate names and legacy `base_id` without `BASE`: overlay READY, data readable.
- Fill the entry table and data area; confirm `ENOSPC`, not `EIO`.
- Test a 1 GiB and a 2 GiB container.
- Verify the container cannot be resized in place and document the migration path.

### 4. Commit and power-loss testing

Interrupt power during:

- create
- append/grow
- rename
- delete
- close/fsync
- settings/theme save
- database/index/cache install

Acceptance: after remount, the state is the complete old state or the complete
new state; no live entry points at freed/reused blocks. Repeat on both ATA and
iFlash storage.

### 5. Native HFS+ integrity

- `diskutil verifyVolume` before and after stress runs.
- Compare native catalog/allocation metadata checksums before/after overlay-only work; native HFS+ metadata must not change.
- Test alternate-header recovery on a copy with a damaged primary header.
- Test a dirty/inconsistent volume: must fail with `-4`, never replay or write.
- Test macOS-created journaled and non-journaled volumes.
- Test fragmented files and files requiring extents-overflow: must fail closed.

### 6. Rockbox USB mode safety

HFS+ firmware builds expose Rockbox USB mass storage as a write-protected LUN.

- Confirm the host mounts it read-only and can copy files off.
- Confirm a host write is rejected with DATA PROTECT.
- Confirm SYNCHRONIZE CACHE and clean disconnect leave the volume clean.
- Apple Disk Mode remains the only supported path for writes, backups and repairs.

Recorded run, 2026-11-02 (iPod Classic 6G + iFlash, V3.1 Beta 5):

- macOS listed `disk4s1` as `Apple_HFS` at offset 67,624,960 with 4096-byte
  blocks and mounted `/Volumes/IPOD` with the `read-only` flag.
- Media/volume read-only was reported as `Yes`.
- `rockbox.ipod` copied off the volume and its SHA-256 matched the build.
- A host `touch` failed with `Read-only file system`.
- `/.rockpod-rw` copied off and validated as `RPOVL13` with 15 live entries.
- `diskutil verifyVolume /Volumes/IPOD` completed with exit 0.
- `MODE_SENSE_10` logged one transient retry during enumeration; the volume
  mounted and the write-protect state was correct. Direct SCSI DATA PROTECT
  was not exercised because the host honored the read-only mount.

### 7. Bootloader and DFU recovery

- Install the Thumb bootloader over DFU; verify success tones and reboot.
- Recover from a deliberately interrupted bootloader install.
- Confirm `--single` is never used and the Apple NOR path remains.
- Test Apple firmware dual-boot where an Apple firmware partition exists.
- Test a freshly formatted HFS+ volume with no Apple firmware partition.
- Verify the bootloader still loads native `/.rockbox/rockbox.ipod` when the overlay is corrupted or contains a `.rockbox` tombstone.

### 8. Database and Cover Flow

- Mac database build, index install, artwork cache install on a clean volume.
- Large library: >16,384 parent directories and >4,096 CNIDs.
- Unicode/composed/case-only names, long names, `<Untagged>` albums.
- Missing/duplicate album artist and album art.
- Verify index/artwork conflict checks against overlay shadows and tombstones.
- Verify no on-device database/PictureFlow rebuild is required.

### 9. Audio and UI

- Playback of ALAC/AAC/MP3/FLAC/WAV, gapless behavior, seek.
- Album art: external and embedded, Now Playing and Cover Flow separately.
- Theme selection, fonts, glyph caches, status bar, backdrops, clean reboot persistence.
- Click wheel, hold switch, backlight, charging, battery gauge, sleep/wake drain.
- Dock/MFi/line-out behavior if available.

## Next hardware work

1. Add a small on-device stress image builder that exercises create/write/rename/delete cycles.
2. Add a repeatable power-cut rig or scripted USB power switch for commit tests.
3. Record boot-time measurements before/after bootloader index removal.
4. Test a second storage adapter family and a second Classic revision.
5. Consider disabling Rockbox USB mass storage in HFS+ builds.
6. Add a read-only overlay export/verify step to the release acceptance checklist.
