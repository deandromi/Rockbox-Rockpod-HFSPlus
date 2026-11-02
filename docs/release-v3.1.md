# Rockpod HFS+ V3.1 — “Nuclear and Readable”

Status: **DRAFT — DEFERRED.** V3.1 stable was not released. The active test
line is **V3.2 Beta — “Thermo-Nuclear”**; this document is retained only as the
deferred V3.1 plan.
Planned tag: `v3.1.0`
Planned public version: `3.1.0-v13.0`
Current beta build: `3.1.0-beta6-v13.0` (review build, not released)
Current stable release: **V3.0 // Version V13.0**

This document is the working release note for the first official V3.1 release.
Do not tag, publish or upload assets until the owner has reviewed the final
package.

## Theme

“Nuclear and Readable” covers two changes:

1. **Readable** — Rockbox USB mass storage now mounts read-only on macOS
   instead of reporting the disk as unreadable.
2. **Nuclear** — the first round of efficiency work removes avoidable overlay
   header flushes and documents the next power/performance upgrades.

## Included from the beta line

- Read-only USB LUN with write-protect reporting and DATA PROTECT.
- Write data-out drain before failure, so bulk-only transport stays in sync.
- Valid 8-byte `MODE_SENSE(10)` write-protect header for macOS.
- RAM-only `Apple_HFS` MBR presentation with cached HFS+ partition geometry
  across the USB unmount.
- `SYNCHRONIZE CACHE` and USB handoff flushes.
- Recorded macOS mount test: `Apple_HFS` at offset 67,624,960, read-only mount,
  firmware hash match, overlay `RPOVL13` validation and clean
  `diskutil verifyVolume`.

## V3.1 efficiency changes in this draft

- v3 high-water updates now reserve slots in 64-slot chunks. A new entry no
  longer forces a header flush on every create; the header is flushed once per
  reserved chunk, before the bounded entries become visible.
- Host regression: 2,048 creates stay within one entry flush per create plus
  the chunk header flushes (bound: 2,112; expected 2,080, down from 4,096).
- The repository-wide efficiency paper is
  [`docs/efficiency-paper.md`](efficiency-paper.md). It covers the custom HFS+
  layer, inherited Rockbox storage/CPU/display/caches, build/tooling and
  non-code power upgrades.

## Release checklist

- [ ] Review the efficiency paper and high-water change.
- [ ] Record flush counts on the device before/after a create/delete/settings
      workload.
- [ ] Re-run the power-loss matrix with the high-water batching.
- [ ] Confirm the overlay cache and high-water bounds survive clean reboot.
- [ ] Decide whether any inherited Rockbox power settings become documented
      defaults; do not change safety defaults without device measurements.
- [ ] Freeze `docs/VERSION` at the approved stable string.
- [ ] Build firmware, Thumb bootloader, DFU and `rockbox.zip`.
- [ ] Run host tests and ASan/UBSan.
- [ ] Record the hardware test result and hashes here.
- [ ] Obtain explicit owner approval before tagging or publishing.

## Open questions for the owner

- Should the codename appear in the boot version string, or only in the release
  title and notes?
- Should V3.1 keep read-only USB as the final behavior, with write-capable USB
  deferred to a later release?
- Which efficiency items must be in V3.1 versus a later release?
- Should V3.1 fix the zero-spindown idle-notify coupling (settings/status
  flushing and SSD sleep reachability), or defer it to a dedicated power
  release with the hardware resume matrix?
