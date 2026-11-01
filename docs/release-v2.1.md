# Rockpod HFS+ V2.1 — release notes

Release tag: `v2.1`
Base: `main` (V2.0 / V12.2) plus the legacy-overlay and startup hotfix
Public version string: `2.1-v12.3`

## Why V2.1 exists

V2.0’s full overlay validator rejected an otherwise readable legacy overlay
when it contained duplicate live names. On the test device this presented as:

```text
HFS+ overlay: OFF
Overlay stage: 11
Overlay error: -2
```

Stage 11 is whole-overlay validation; `-2` is `HFS_RO_FORMAT`. Older firmware
had allowed duplicate `.resume.cfg.old` entries. Rejecting the entire overlay
also hid settings and glyph caches, which made startup and theme selection
behave badly.

## Fixes

- Legacy duplicate live names no longer disable the overlay.
  - The newer entry is kept.
  - The older duplicate is hidden in RAM.
  - Its data blocks remain allocated; nothing is deleted or overwritten.
- Duplicate BASE/tombstone entries are resolved the same way.
- `next_id` considers shadowed IDs, so a new entry cannot collide with a
  hidden legacy entry.
- The overlay entry table is read in 96-entry chunks instead of 2,048 separate
  320-byte requests. This removes a large part of the mount delay.
- The bootloader no longer builds the full directory and CNID indexes. It only
  loads the firmware path, which is the main power-on speed improvement.
- Adapter regression coverage is now 16 tests, including a legacy duplicate
  name fixture.

The audit status is unchanged: 103 findings fixed and 6 documented format
limits.

## Verification

- 19 reader tests, 7 partition tests and 16 host adapter tests pass.
- The reader/partition/adapter suite passes under ASan/UBSan.
- macOS host integration mode passes.
- Firmware, Thumb bootloader, database tool, `mks5lboot`, DFU and
  `rockbox.zip` build.
- `rockbox.zip` passes `unzip -t`; no macOS `.DS_Store`/`._*` entries.

V2.0 was installed on the test iPod and the duplicate-name overlay failure was
reproduced. V2.1 is host/build-verified; the final boot, overlay READY and
theme-selection acceptance still needs confirmation on the device.

## SHA-256

```text
4c2b9cccc133ea8a23c24b36426c9612d33b4e2bb638ac293d6f8444c5095bfd  rockbox.ipod
c8d12e3ba5898d9503b363e1ff8555c235dc108311dbf804b2539d36012089da  rockbox.zip
2277257c9fce97a60fe5afdea45b56edccaa2b181cf7b9858cffe6023f248d29  bootloader-ipod6g.ipod
59b2a05d80b6ca14acbea8185bdf3d48ca5fad8fae80ccf811aca326fb823c1e  bootloader-ipod6g.dfu
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
- Install the V2.1 bootloader over DFU if you want the startup-speed fix. It is
  separate from `.rockbox`.
- Existing overlay v1/v2 containers remain readable. Duplicate legacy entries
  are hidden, not repaired destructively.
- Keep using Apple Disk Mode for transfers and the Mac database/PictureFlow
  workflow.
