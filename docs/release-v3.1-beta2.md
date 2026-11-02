# Rockpod HFS+ V3.1 Beta 2 — read-only USB protocol fix

> **Superseded by [V3.1 Beta 5](release-v3.1-beta5.md).** Beta 2 fixed the
> write data-out drain, but macOS still saw the stale FAT32 MBR type and could
> not mount the HFS+ volume. Use Beta 5 for read-only USB.

Release tag: `v3.1-beta2`
Public version string: `3.1.0-beta2-v13.0`
Status: **beta**. V3.0 remains the latest stable release.

## Why Beta 2

Beta 1 exposed a USB bulk-only protocol bug: a read-only `WRITE_10`/`WRITE_16`
was rejected before the host's data-out phase was received. Sending CSW while
the host was still sending data can desynchronize the LUN, which matches the
macOS “disk could not be read” result and the iPod reset.

Beta 2 drains every data-out chunk first, then reports DATA PROTECT. It does not
write the sectors. This applies to multi-chunk writes too.

## Behavior

- HFS+ Rockbox USB mass storage is read-only.
- MODE SENSE reports write-protect.
- Writes return SCSI DATA PROTECT after the data phase is drained.
- `SYNCHRONIZE CACHE` calls `storage_flush()`.
- Storage is flushed after unmounting for USB and before remounting after
  disconnect.
- macOS can mount/read/copy files off and read `/.rockpod-rw` for diagnostics.
- Apple Disk Mode remains the only write path.

## Test checklist

1. Connect in Rockbox USB mode; macOS must mount **read-only**.
2. Copy a large file off and compare SHA-256.
3. Attempt a host write/delete; it must fail without resetting the iPod or
   disconnecting the LUN.
4. Cleanly eject, reboot, and verify:
   - `Debug → Disk Info`: overlay READY, stage 9;
   - `diskutil verifyVolume /Volumes/IPOD` is clean.
5. Copy `/.rockpod-rw` off and run `tools/rockpod/overlay_check.py` for
   diagnostics.

If the Mac still reports an unreadable disk, record:
- whether the iPod reboots at connect, mount attempt or eject;
- `diskutil list` and the exact macOS message;
- `Debug → Show Log File` from the iPod.

## Verification

- Firmware and `rockbox.zip` build with `3.1.0-beta2-v13.0`.
- HFS+ host suite: 20 reader, 7 partition and 30 adapter tests pass.
- No macOS/iFlash USB test has been recorded for this beta.

## Assets and SHA-256

```text
1dfbf40e51d83f958895fba8d0f88cf65830e44e3aacbf764d95069c738aa371  rockbox.ipod
6e571bb5ffa9d9891d05bc2ba1f2dab48376fe2b4882700b23d1fb96dc64c674  rockbox.zip
f711a2d826ab2bc7677bf68d7f3d0c4b03b0c1bcb58d02a9a71d799e9ffe803e  bootloader-ipod6g.ipod (optional)
f212bd23fa7fe990b3d4480521fd761c80c54b33367f62113ec66c468510bbf2  bootloader-ipod6g.dfu (optional)
```
