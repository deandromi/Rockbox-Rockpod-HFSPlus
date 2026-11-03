# HFSPlus Rockboxed iPod V3.1 Beta — read-only Rockbox USB

> **Superseded by [V3.1 Beta 5](release-v3.1-beta5.md).** Beta 1 had a
> bulk-only write-rejection bug and did not present the HFS+ MBR type.

Release tag: `v3.1-beta`
Public version string: `3.1.0-beta-v13.0`
Status: **beta** — firmware/build verified; macOS/iFlash USB mounting still needs
a device test. V3.0 remains the latest stable release.

## What changed

Rockbox USB mass storage is now read-only on HFS+ firmware builds.

- The LUN reports write-protect (WP) in MODE SENSE.
- `WRITE_10`, `WRITE_16` and raw disk writes return SCSI `DATA PROTECT`.
- `SYNCHRONIZE CACHE` (10/16-byte) calls `storage_flush()`.
- The USB handoff flushes storage after unmounting and before remounting after
  disconnect.
- This makes the native HFS+ volume and `/.rockpod-rw` safe from host writes
  while still allowing macOS to mount, browse, copy files off and inspect the
  overlay for diagnostics.

Apple Disk Mode remains the supported path for adding, changing or deleting
files.

## What to test

1. Connect the iPod in Rockbox USB mode.
2. Confirm macOS mounts the volume **read-only**.
3. Copy a large file off and compare its SHA-256.
4. Attempt a host write; it must fail with a write-protect error.
5. Cleanly eject/disconnect, reboot, and check:
   - `Debug → Disk Info`: overlay READY, stage 9, expected format;
   - `diskutil verifyVolume /Volumes/IPOD` is clean.
6. With the read-only volume mounted, copy `/.rockpod-rw` to the Mac and run:
   ```sh
   python3 tools/rockpod/overlay_check.py /path/to/copied/.rockpod-rw
   ```
   or the read-only exporter. This is the diagnostic access path.

## Limitations

- Rockbox USB cannot write to the iPod in this beta. Use Apple Disk Mode.
- The bootloader is not required for this behavior; firmware alone is enough.
- The read-only guard does not make a host-side HFS+ repair safe. Do not format,
  erase or repair through Rockbox USB.
- No v3→v3 migration or overlay write mode is included.

## Verification

- Firmware and `rockbox.zip` build with the version above.
- HFS+ host suite: 20 reader, 7 partition and 30 adapter tests pass.
- No macOS USB mount or device test was recorded for this beta.

## Assets and SHA-256

```text
8bb2adbf49d9a8ca15bc765e481a5f2de5c232205e3309cb323579046d545cdb  rockbox.ipod
1934254ce79e9200f8d76eaeee1ad94fed4b9f0deb03238ae95c22961f320056  rockbox.zip
e89c3d19b971635ca990b4cba3ab67defffe1b6e3b3a6bf13b993f32d67b5840  bootloader-ipod6g.ipod (optional; firmware-only update is sufficient)
12dc68f6a8cab38471878641558eacfb05d0c34a1c76f73b65eb5cf8f0e02a4e  bootloader-ipod6g.dfu (optional)
```
