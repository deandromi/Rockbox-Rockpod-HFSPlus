# Rockpod HFS+ V3.1 Beta 5 — read-only Rockbox USB that macOS mounts

Release tag: `v3.1-beta5`
Public version string: `3.1.0-beta5-v13.0`
Status: **beta**. V3.0 remains the latest stable release.

Beta 5 supersedes Beta 1 and Beta 2. It is the first read-only USB build that
was mounted and read end-to-end by macOS on the test iPod.

## Why Beta 2 was still unreadable

Beta 2 fixed a real bulk-only protocol bug by draining every write data-out
chunk before returning DATA PROTECT. macOS still reported:

> The disk you attached was not readable by this computer.

The remaining cause was the USB view of the partition table:

- The native MBR still labels the data partition `0x0B`/`0x0C` (FAT32) because
  it was formatted over an older FAT layout. macOS trusts the type, tries FAT,
  and never probes the HFS+ content.
- Apple Disk Mode synthesizes an `Apple_HFS` MBR, so the same volume mounts
  there. Rockbox was exporting the raw MBR unchanged.
- The `MODE_SENSE(10)` write-protect response used a long-LBA block descriptor
  even when the host asked for the 8-byte header, so macOS logged a failed WP
  query during enumeration.
- Rockbox unmounts the filesystem before exposing USB. The partition geometry
  therefore has to be cached at scan time for the MBR presentation to work.

## What changed

- The USB read path relabels the mounted HFS+ data partition as `Apple_HFS`
  (`0xAF`) in the MBR buffer. The native MBR is never written.
- The HFS+ partition offset/unit is cached before USB mode unmounts the volume.
  If the exact entry cannot be matched, the largest FAT-typed entry is used.
- `MODE_SENSE(10)` returns a valid 8-byte header with the write-protect bit and
  no block descriptors. The host only reads byte 3.
- The read-only LUN still reports write-protect, drains write data-out chunks
  before DATA PROTECT, and flushes on SYNCHRONIZE CACHE/handoff.
- Apple Disk Mode remains the only write path.

## Recorded device test

Tested 2026-11-02 on an iPod Classic 6G with an iFlash adapter:

- macOS listed `disk4s1` as `Apple_HFS` at offset 67,624,960 with 4096-byte
  blocks and mounted `/Volumes/IPOD` with the `read-only` flag.
- Media and volume read-only were reported as `Yes`.
- `rockbox.ipod` copied off the volume; its SHA-256 matched the installed
  build.
- A host `touch` failed with `Read-only file system`.
- `/.rockpod-rw` copied off and validated as `RPOVL13` with 15 live entries.
- `diskutil verifyVolume /Volumes/IPOD` completed with exit 0.
- The overlay mtime and size were unchanged.

`MODE_SENSE(10)` still logged one transient retry during enumeration; the
volume mounted and the write-protect state was correct. Direct SCSI DATA
PROTECT was not exercised because macOS honored the read-only mount.

## Host verification

- 20 HFS+ reader tests.
- 7 partition tests (1 native-adapter test skipped when not supplied).
- 30 adapter tests, including the new MBR-relabel test that also verifies the
  cached geometry survives unmount.
- ASan/UBSan suite passes.
- Firmware, Thumb bootloader, `mks5lboot` DFU and `rockbox.zip` build.

## Install

The bootloader is optional; the read-only behavior is firmware-only.

In Apple Disk Mode:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
sync
diskutil eject "/Volumes/IPOD"
```

Boot Rockpod, then connect in Rockbox USB mode. macOS should mount the volume
read-only. Use Apple Disk Mode for all writes, backups and repairs.

## Limitations

- Read-only USB only. Phase 2 (write-capable USB with an explicit overlay reset)
  is not implemented.
- The raw MBR type is unchanged. Do not use the USB view as a reason to
  format, erase or repair the native partition table.
- `diskutil verifyVolume` checks the native HFS+ volume, not overlay internals.
- One Classic 6G + iFlash device is confirmed; other revisions and adapters
  remain unverified.

## Assets and SHA-256

```text
e6971e35c1b3fbe730c82c330a32b28ed11d6550b9b183a07a920ee62151b2d7  rockbox.ipod
b766d74243cad77e439869c4fb6f74a38cda7354f7de4680a2e9f0b679e56b1c  rockbox.zip
cb6b95fa1cc7620a725763fbd7db26c3be6a3447366f0ec041af9f111dff53d7  bootloader-ipod6g.ipod (optional)
5e3e6c1ebae699f936b4900f99493c3b96643cf94ebc6c25ca7e89d2db8e5ace  bootloader-ipod6g.dfu (optional)
```
