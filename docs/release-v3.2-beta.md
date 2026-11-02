# Rockpod HFS+ V3.2 Beta — Thermo-Nuclear

Release tag: `v3.2-beta`
Public version string: `3.2.0-beta-v13.0`
Status: **experimental beta**. Not a stable release.
Stable release remains **V3.0 // Version V13.0**.

> **Superseded by V3.2 Beta 2.** Beta 1 froze 4–6 seconds after boot on an
> iFlash SD because of the periodic storage idle-notify path. Beta 2 removes
> that path. See [`release-v3.2-beta2.md`](release-v3.2-beta2.md).

## Update type

**Both firmware and bootloader.** This release contains:

- **Firmware:** `rockbox.ipod` and `rockbox.zip` (the zip contains
  `.rockbox/rockbox.ipod`, codecs, plugins, themes, fonts and languages).
- **Bootloader:** `bootloader-ipod6g.ipod` and the DFU installer
  `bootloader-ipod6g.dfu`.

Installing the firmware alone gives you the V3.2 firmware features but not the
bootloader minimal-state/Thumb changes. Installing the bootloader alone does
not update the firmware. A full V3.2 install means the firmware assets **and**
the bootloader assets, in that order (bootloader first if you are recovering a
device, then firmware).

## Read this first

This beta is a large, experimental efficiency batch. It is **not**
hardware-validated for the P0 power-loss, native-integrity or SSD-resume gates.
`docs/v3.2-validation.md` records what was and was not run.

- Keep a verified backup of `/.rockpod-rw` and the native music library.
- Use Apple Disk Mode for all writes and backups.
- Do not treat this beta as a production database or only-copy device.
- Do not format, erase, restore or “repair” the volume to fix a beta issue.

## Why “Thermo-Nuclear”?

Because this is not a scented-candle optimization pass. Thermonuclear fuel has
the highest energy density we know how to build, and that is the whole joke:
this batch tries to get more useful work out of every joule, ATA transaction,
PMU write, catalog scan and tag read. Same battery, fewer wasted reactions.

The safety analogy is the serious half. The reaction is contained in
`/.rockpod-rw`; native HFS+ is the shielding and is never written; a bad state
fails closed instead of melting down. The big red button labelled **“P0
hardware validation: not run”** is real, though, so please do not press it on
your only iPod.

## Scope

The batch implements the P0/P1/P2 efficiency portfolio from
`docs/efficiency-paper.md`:

- P0 power-loss host matrix, baseline instrumentation and native-region
  integrity checks;
- zero-spindown idle-notify and wake-amplification changes;
- SSD/spindown validation procedure;
- backlight PMU write cache;
- album-art directory cache;
- runtime DB/autoresume filename locality;
- tagcache title/cursor improvements;
- track-load and playlist-viewer reductions;
- read-only stream read-ahead;
- tagcache RAM batching;
- adapter read-only name/path cache for linear path resolution;
- 16 KiB overlay copy chunks;
- readdir parent-bucket walk and high-water validator bounds;
- bootloader minimal-state work;
- ATA flush error propagation;
- lineout-volume write cache.

Some items are conservative or staged; the release notes below record the exact
status and the unverified areas.

## Rollback

If Beta boots but misbehaves, reinstall the stable **V3.0** firmware or the
last known read-only USB build, **V3.1 Beta 5**. Do not format, erase, restore
or delete `/.rockpod-rw` to recover from a beta regression. Keep a verified
overlay backup before testing.

## Verification

- Host suite: **20 reader + 7 partition + 38 adapter tests pass**.
- ASan/UBSan: **passes** (38 adapter tests).
- macOS integration: **passes**.
- Firmware: **`3.2.0-beta-v13.0-261102`**, 830,344 B.
- `rockbox.zip`: 19,100,956 B, contains the same `rockbox.ipod` SHA-256 and
  no `.DS_Store`.
- Thumb bootloader: 105,256 B; `_moveend` `0x2203a320` inside `MOVE_AREA` end
  `0x22040000`.
- DFU installer: 113,392 B; model Classic 6G, platform ipod6g, RB `ip6g`/71.
- Database host tool: rebuilt as native Mach-O arm64.
- P0 hardware gates: **not run**.
- SSD/spindown 100-cycle test: **not run**.
- Device boot/playback/settings/overlay test: **not run**.

`docs/v3.2-validation.md` has the detailed evidence table and the exact device
procedures still outstanding. `docs/v3.2-review.md` records the strict
code-quality review, including the fixes made before this build and the
accepted structural debt.

## Assets and SHA-256

```text
b00505b7fd6832d3bf0fa21d18f9db6ccaaffb84faed3be82b50158fdcf5b72a  rockbox.ipod
d5d6b6e2ea211aea76ff5dbe8f33f7601aadb4f32c37eff6fa390a137879dc55  rockbox.zip
5ae34f96d4abdba01730f28529c54d055f8882c5e0d68165e420adbdd6ff025b  bootloader-ipod6g.ipod
a1f1eb6eaf6cae61f19c8cce141e5fe37d5c9d144d9d7f5f5ef90365261ef750  bootloader-ipod6g.dfu
```

The release page must keep **V3.0** as the GitHub “Latest” release and mark
this build as a pre-release.
