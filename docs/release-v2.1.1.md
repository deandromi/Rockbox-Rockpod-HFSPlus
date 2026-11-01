# Rockpod HFS+ V2.1.1 — legacy overlay recovery

Release tag: `v2.1.1`
Base: `main` (V2.1 / V12.3) plus the legacy overlays fix
Public version string: `2.1.1-v12.3`

## Why V2.1.1 exists

V2.1 fixed duplicate live names, but the test device still failed overlay
validation with:

```text
HFS+ overlay: OFF
Overlay stage: 11
Overlay error: -2
Overlay slot: -1
```

The remaining entry was `pictureflow.cfg` in `.rockbox/rocks/demos/pictureflow`.
It stored a native backing CNID (`base_id = 20694`) but did **not** have the
`HFS_OVL_BASE` flag. Older firmware wrote that shape; V2.0/V2.1 rejected every
non-BASE entry with a nonzero `base_id`.

## Fixes

- A non-`BASE` entry may carry a native backing CNID below `0x40000000`.
  It is kept as a legacy namespace hint, not treated as an unmaterialized BASE
  shadow, so reads continue to use the overlay data.
- A backing CNID in the overlay ID range (`>= 0x40000000`) is still rejected.
- Duplicate live names and duplicate BASE/tombstone entries are shadowed in
  RAM (newer ID wins), preserving the V2.1 behavior.
- Adapter regression coverage is now 17 tests, including a legacy backing-ID
  fixture.
- The bootloader already skips the full directory/CNID index build from V2.1;
  V2.1.1 keeps that startup fix.

The audit status is unchanged: 103 findings fixed and 6 documented limits.

## Verification

- 19 reader tests, 7 partition tests and 17 host adapter tests pass.
- The reader/partition/adapter suite passes under ASan/UBSan.
- Firmware, Thumb bootloader, database tool, `mks5lboot`, DFU and
  `rockbox.zip` build.
- V2.1.1 firmware was installed on the test iPod in Apple Disk Mode and
  confirmed working: overlay READY (stage 9, error 0), startup improved,
  theme selection no longer panics, playback and settings work.

## SHA-256

```text
5ecc116beae4383169de5fd500e8f6cc15c845d29a86e41cb8204522d8c2cc53  rockbox.ipod
6dd133e97aeb05e02cd9d544e511d5cbdf5da58973fbbc3e9db62ebd3c812a85  rockbox.zip
4253d3b4065645ea2835ab0c359c240174e98d97d346e6487e1c7892a7010af4  bootloader-ipod6g.ipod
153a141f3f569725aa3360421def50976d95f95e03f3860b6508c96f0738351e  bootloader-ipod6g.dfu
8028ee85132c0905cb35ea494c88bbefcfe2d1a3d870aedc3b58c1be0274c0f0  database.ipod6g
25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b  mks5lboot
```

## Assets

- `rockbox.zip` — complete `.rockbox` installation archive
- `rockbox.ipod` — main firmware image
- `bootloader-ipod6g.ipod` — Thumb HFS+ bootloader
- `bootloader-ipod6g.dfu` — DFU installer image
- `mks5lboot` — macOS DFU utility
- `database.ipod6g` — macOS database host tool

## Upgrade notes

- Firmware-only update: extract `rockbox.zip` over the mounted iPod root.
- Existing overlay containers are not deleted or rewritten. Legacy entries are
  hidden in RAM where needed.
- The V2.1 bootloader already has the startup-speed fix; the V2.1.1 bootloader
  differs only in the version string, so reflashing is optional.
- Continue using Apple Disk Mode and the Mac database/PictureFlow workflow.
