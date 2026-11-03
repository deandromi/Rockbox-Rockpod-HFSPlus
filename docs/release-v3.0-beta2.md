# HFSPlus Rockboxed iPod V3.0 Beta 2 — Disk Info hotfix

Release tag: `v3.0-beta2`
Public version string: `3.0.0-beta2-v13.0`
Status: **beta** — firmware-only hotfix on top of V3.0 Beta.

## What changed

- The ATA **Disk Info** callback adds about 46 lines, but `simplelist` was
  capped at 32 and wrapped its line counter. The visible result was just the
  final two lines (`Fixed device`, `Removeable media`).
- `SIMPLELIST_MAX_LINES` is now 64, and `simplelist_addline()`/`setline()` are
  bounded so an overlong list truncates instead of wrapping or underrunning
  the shared buffer.
- No overlay format, bootloader or HFS+ behavior changed.

## Installation

This is a firmware-only update from V3.0 Beta. The V3.0 Beta bootloader is
unchanged and does **not** need reflashing.

1. Enter Apple Disk Mode and mount `/Volumes/IPOD`.
2. Extract `rockbox.zip` over the volume:
   ```sh
   unzip -o rockbox.zip -d "/Volumes/IPOD"
   sync
   diskutil eject "/Volumes/IPOD"
   ```
3. Boot and open **Debug → View disk info**. The HFS+ overlay, stage, reason,
   index-cache and I/O lines should now be visible and scrollable.

## Verification

- Firmware and `rockbox.zip` build.
- HFS+ host suite unchanged: 20 reader, 7 partition and 30 adapter tests.
- No hardware test was recorded for this UI-only hotfix.

## Assets and SHA-256

```text
48b209c9f750dccd9eec2488a18b0219211ab15297850ff104cf2290c9bf31d3  rockbox.ipod
589530d62f67ed6d719fed86e12e0c9878b66f774f620c1a5eee50298c2744a7  rockbox.zip
e398b95d087dfb676fbc6dbc21c46445bbe47e6ef6ba84d8b6ef80e0f3ec4359  bootloader-ipod6g.ipod (unchanged from V3.0 Beta)
f557dd5b7d7e19710b00efb4c8138515ca7f538916334576227d4548849625e4  bootloader-ipod6g.dfu (unchanged)
```
