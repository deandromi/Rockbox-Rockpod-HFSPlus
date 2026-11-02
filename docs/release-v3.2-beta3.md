# Rockpod HFS+ V3.2 Beta 3 — Thermo-Nuclear

Release tag: `v3.2-beta3`
Public version string: `3.2.0-beta3-v13.0`
Status: **experimental beta**. Not a stable release.
Current stable remains **V3.0 // Version V13.0**.
Supersedes: **V3.2 Beta 2** (`v3.2-beta2`).

## Update type

**Both firmware and bootloader.**

- **Firmware (required):** `rockbox.ipod` and `rockbox.zip`.
- **Bootloader:** rebuilt because the ATA driver is shared with the bootloader.
  Reinstalling it is optional if the device already boots Beta 2.

## What changed in Beta 3

### Sleep/wake rollback

Beta 2 still carried the V3.2 power/backlight experiment. On the affected
iFlash SD device, entering sleep by hold or inactivity could leave the device
unresponsive: the screen stayed dead, buttons did nothing, and only a restart
recovered it. V3.0 and V3.1 Beta 5 did not have this behavior.

Beta 3 restores the V3.1 power path:

- `storage_ata-6g.c` is back to the V3.1 driver: synchronous PRE_WAKE/SSD
  power-up, no explicit SSD sleep with `disk_spindown = 0`, no new SSD
  deep-sleep state machine, and no ATA flush-error propagation experiment.
- `backlight.c` and `backlight-6g.c` are back to the V3.1 PMU/backlight path:
  no cached `0x28`/`0x29` writes and no transition-only PRE_WAKE gating.
- The lineout-volume write cache in `cs42l55.c` is kept; it is not part of the
  sleep/wake state machine.

The ATA/backlight efficiency items are deferred until sleep/wake and resume can
be validated on hardware.

### Freeze fix kept

The Beta 2 fix for the 4–6 s post-boot freeze remains: the periodic
`Q_STORAGE_IDLE_NOTIFY` path is not present. With `disk_spindown = 0`, deferred
storage-idle callbacks stay deferred as in V3.1.

### Tagcache batch-read hardening

`read_index_entries()` now fills the whole requested batch even if the backing
store returns a short read. The batched RAM-index load no longer fails because
one transfer was split at a sector boundary. This is a correctness hardening
and may affect the slow-boot investigation.

Everything else from V3.2 remains: album-art/tagcache locality, read-ahead,
adapter name cache, overlay batching, diagnostics and the HFS+ namespace work.

## Known issues under investigation

- **Slow boot:** about 20–30 seconds reported after the freeze fix. Not yet
  root-caused. Check `Debug → Disk Info` for `Index cache: HIT`/`MISS` and
  report whether the delay is every boot or only the first.
- Directory loading, scroll-wheel ghosting and Cover Flow behavior need to be
  rechecked after the power rollback.

**Still not executed:** P0 physical power-loss matrix, full native HFS+
integrity stress/`diskutil verifyVolume` run, and the 100-cycle SSD/spindown
resume test. See `docs/v3.2-validation.md`.

## Rollback

Reinstall **V3.0** or **V3.1 Beta 5** firmware if needed. Do not format, erase,
restore or delete `/.rockpod-rw`. Keep a verified overlay backup.

## Assets and SHA-256

```text
6331455252bf71ad4fbb58288c1fcaec747cc8088d8db4b854a38bd3b858363c  rockbox.ipod
cd3abad7922f11c4899438eed907af7687c2bcb9ab571554a027f1fc8f44cc57  rockbox.zip
76dd3e3d991ebe02f14e69a7cb22437847d929e71a199ddbf1dfdfd3276cae32  bootloader-ipod6g.ipod
d4effd1ac2047c0c6cbacf1f15a66511d693b11bc3c52a2509a96f8b07f9d0a1  bootloader-ipod6g.dfu
```
