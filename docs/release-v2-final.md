# HFSPlus Rockboxed iPod V2 Final Legacy — V1/V2 conclusion

Release tag: `v2-final`
Public version string: `2.1.1-v12.3` (the final V2 line)
Status: **legacy final**. This release concludes V1 and V2. It is not V3.0,
does not require a new overlay, and does not include the V3 overlay format,
persistent index cache or V3 power work.

## Why this release exists

V2 Final is the last install of the V1/V2 line. It packages the exact V2.1.1
firmware and bootloader with a single install note and Apple-device blocker,
so users who want the mature V2 behavior do not have to assemble the old
release artifacts.

Use **V2 Final** if:

- you want to keep an existing v1/v2 `/.rockpod-rw` overlay;
- you do not want to recreate the overlay for V3.0;
- you want the final V1/V2 bugfix set without the v3 format/cache.

Use **V3.0** if you want the v3 overlay, persistent index cache, high-water
entry reads and the newer power work. V3.0 requires a **new** v3 overlay;
V2 Final does not.

## What V2 Final contains

V2 Final is byte-identical to V2.1.1. It includes the accumulated V1/V2 work:

- **V1.1:** overlay safety fixes, stable directory rename, bootloader
  native-only mount, PictureFlow bounds/recovery fixes, partition hardening.
- **V2.0:** whole-overlay validation, crash-consistent commit ordering,
  header checksum/backup, native allocation/metadata checks, alternate-header
  recovery, ENOSPC mapping, timestamp exposure, partition hardening.
- **V2.1:** legacy duplicate live-name handling, chunked entry-table mount,
  bootloader index-build removal.
- **V2.1.1:** legacy `base_id` without `BASE` compatibility; final V2
  adapter coverage.

V2 Final does **not** include the V3.0 v3 overlay format, index cache,
high-water reads or the V3.0 Disk Info fixes beyond V2.1.1.

## Simple install

Full copy in `INSTALL.txt` inside the bundle. Short version:

1. **Back up** the iPod and, if present, `/.rockpod-rw`.
2. Enter **Apple Disk Mode**.
3. Keep an existing v1/v2 overlay. If there is no overlay, create one:
   ```sh
   mkfile 1g "/Volumes/IPOD/.rockpod-rw"
   ```
4. Extract the firmware:
   ```sh
   unzip -o rockbox.zip -d "/Volumes/IPOD"
   sync
   diskutil eject "/Volumes/IPOD"
   ```
5. Run `python3 apple-device-blocker.py` in Terminal 1.
6. In Terminal 2, scan and install the bootloader:
   ```sh
   ./mks5lboot --dfuscan --loop 1
   ./mks5lboot --bl-inst bootloader-ipod6g.ipod
   ```
   **Never use `--single`.** Do not unplug during the NOR write.
7. After the success tone and reboot, stop the blocker with **Control-C**.
8. Boot and check **Debug → Disk Info → Overlay stage 9, error 0**.
9. Use the Mac database/artwork workflow from the main README.

The bootloader install requires DFU. The exact DFU button sequence and
troubleshooting are in the main project instructions; the bundle includes the
scanner and installer.

## Compatibility

- Reads and writes v1 and v2 `/.rockpod-rw` overlays, including the legacy
  duplicate-name and backing-ID shapes.
- Does **not** create or use v3 `RPOVL13` overlays.
- HFS+ version 4, clean volumes only; no journal replay.
- Single `ipod6g` HFS+ volume; HFSX/APFS unsupported.
- Exact-byte names; no Unicode normalization/case folding.
- Native HFS+ metadata remains immutable; all writes stay in the overlay.
- Bootloader and firmware are the historical V2.1.1 binaries; this repackaging
  does not alter them.

## Verification

- Artifacts were downloaded from the existing `v2.1.1` GitHub release and
  SHA-256 verified against `docs/release-v2.1.1.md`.
- Historical V2.1.1 verification: 19 reader + 7 partition + 17 adapter tests,
  ASan/UBSan, firmware/bootloader/zip/DFU builds, and device confirmation on
  the test iPod.
- No new hardware test was performed for this repackaging.

## Assets and SHA-256

Individual files and the bundle contain the same binaries.

```text
5ecc116beae4383169de5fd500e8f6cc15c845d29a86e41cb8204522d8c2cc53  rockbox.ipod
6dd133e97aeb05e02cd9d544e511d5cbdf5da58973fbbc3e9db62ebd3c812a85  rockbox.zip
4253d3b4065645ea2835ab0c359c240174e98d97d346e6487e1c7892a7010af4  bootloader-ipod6g.ipod
153a141f3f569725aa3360421def50976d95f95e03f3860b6508c96f0738351e  bootloader-ipod6g.dfu
25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b  mks5lboot
8028ee85132c0905cb35ea494c88bbefcfe2d1a3d870aedc3b58c1be0274c0f0  database.ipod6g
c06a5dfb2f9c4db67fb21ba37c166e96c18a1fdaaf5174b46178e51d6b017292  rockpod-hfs-v2-final-legacy.zip
```

## Bundle contents

```text
rockbox.zip
rockbox.ipod
bootloader-ipod6g.ipod
bootloader-ipod6g.dfu
mks5lboot
database.ipod6g
INSTALL.txt
apple-device-blocker.py
SHA256SUMS
```
