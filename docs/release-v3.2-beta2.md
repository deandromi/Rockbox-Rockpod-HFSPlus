# Rockpod HFS+ V3.2 Beta 2 — Thermo-Nuclear

Release tag: `v3.2-beta2`
Public version string: `3.2.0-beta2-v13.0`
Status: **experimental beta**. Not a stable release.
Current stable remains **V3.0 // Version V13.0**.
Supersedes: **V3.2 Beta** (`v3.2-beta`).

## Update type

**Both firmware and bootloader.**

- **Firmware (required for this fix):** `rockbox.ipod` and `rockbox.zip`.
- **Bootloader (optional if already on V3.2 Beta 1):** the storage driver is
  shared with the bootloader, so a rebuilt bootloader is included. The freeze
  being fixed is a runtime firmware issue; reinstalling the bootloader is not
  required to get the fix.

## What changed in Beta 2

This release fixes the **4–6 second post-boot freeze** on iFlash SD reported
against V3.2 Beta 1.

- Removed the periodic `Q_STORAGE_IDLE_NOTIFY` path added in Beta 1. That path
  ran deferred storage-idle callbacks at about 5 seconds after boot even when
  `disk_spindown = 0`, from the storage thread. It is the first V3.2 code that
  runs at a fixed early-boot time, and the diagnostic build without it no
  longer freezes on the affected device.
- Reverted the corresponding storage event and handler. With
  `disk_spindown = 0`, deferred settings/status callbacks again stay deferred
  until a forced flush point, matching V3.1 behavior.
- Everything else from V3.2 Beta 1 remains: album-art/tagcache locality,
  read-ahead, adapter name cache, overlay batching, diagnostics and the
  remaining power/wake changes.

The zero-spindown idle-notify decoupling needs a safer redesign; it is not
part of Beta 2.

## Device verification

On the affected iPod Classic 6G with iFlash SD:

- V3.2 Beta 1 froze every boot at about 4–6 seconds with the screen lit and
  buttons frozen.
- A diagnostic firmware containing only this removal booted, stayed responsive
  past the former freeze window, and did not freeze during the initial check.
- Beta 2 carries the same storage change with the Beta 2 version string.

**Known issues still under investigation (not fixed or claimed fixed here):**

- slow boot observed on the diagnostic build;
- directories not loading reliably;
- scroll-wheel ghosting/repeated rotation;
- a freeze when opening Cover Flow.

These were observed on the diagnostic firmware and need to be checked again on
Beta 2 before they are attributed to a specific V3.2 change. They are not
release blockers for the freeze fix, but they are blockers for a stable V3.2.

**Still not executed:** P0 physical power-loss matrix, full native HFS+
integrity stress/`diskutil verifyVolume` run, and the 100-cycle SSD/spindown
resume test. See `docs/v3.2-validation.md`.

## Rollback

Reinstall **V3.0** or **V3.1 Beta 5** firmware if needed. The bootloader can
stay as-is. Do not format, erase, restore or delete `/.rockpod-rw` to recover
from a regression. Keep a verified overlay backup.

## Assets and SHA-256

```text
f8f71f50dbf6301666f07bd408079a966bfc2b268de14fca7a89c5ccb492a4f7  rockbox.ipod
750995e5de18ce883a4c1ce61cf57d48f4452bb8b98c3d8deedb8f4c62e7f6ae  rockbox.zip
be03f010bf4995e4462feb2fb596390427c93cd44c5b0f024fe08f9d25243f0a  bootloader-ipod6g.ipod
e379ebe0466d0c8f817143ae7e277147f71ea9b0a2c594382f1f87da5ba56201  bootloader-ipod6g.dfu
```
