# Next steps — V2.1.1 and later

Status: V2.1.1 released and confirmed working on the test iPod Classic 6G.
Post-V2.1.1: the adapter decomposition is complete, stage 11 diagnostics are
implemented, and the immutable-container write fast path is in. Host/build
verified; no device or power measurement has been recorded for these changes.
This file records the deferred hardware work and the current code-quality review
so it can be picked up later without re-deriving it.

---

## Hardware suggestions

Priority order:

1. **Power-loss matrix**
   - Cut power during create, append, rename, delete, close/fsync, settings
     save and cache install.
   - Verify old-or-new complete state, no block reuse and no live entry
     pointing at freed blocks.
2. **Second device/adapter matrix**
   - Another Classic 6G revision.
   - iFlash SD, iFlash CF and original HDD.
   - Record model, adapter, card/SSD, battery health and macOS version.
3. **Rockbox USB mode guard**
   - Disable or strongly block mass-storage mode in HFS+ builds.
   - It is a known corruption path; Apple Disk Mode remains the supported
     transfer path.
4. **Bootloader/DFU recovery**
   - Interrupted DFU install.
   - No Apple firmware partition.
   - Dual-boot layout.
   - Stale-overlay `.rockbox` boot.
   - Never use `--single`.
5. **Large-volume/large-library tests**
   - 2 GiB overlay.
   - Allocation-file fragmentation and extents-overflow cases.
   - More than 16K parent directories and 4K CNIDs.
   - Unicode, composed/decomposed and long names.
6. **Native HFS+ integrity checks**
   - Verify native catalog/allocation metadata is untouched after stress.
   - Alternate-header recovery on a copy.
   - Dirty/inconsistent volume must fail with `-4`, never write.
7. **Audio/UI/battery soak**
   - Album-art paths, docks/MFi, sleep drain, charging, click wheel and glyph
     caches.
   - Theme persistence after a clean power-off.

Full checklist: [`docs/hardware-validation.md`](hardware-validation.md).

---

## Thermo-nuclear code-quality review

The full current review is in [`agents.md`](../agents.md) §8. Post-decomposition
state:

1. **The monolith is gone.** `hfsplus_rb.c` was split into I/O, format,
   allocation, store, validator, loader, namespace, FAT, mount, state,
   diagnostics and time modules. The largest adapter module is under 500 lines.
2. **`shadowed[]` reads are centralized.** Every visibility read now goes
   through `overlay_entry_visible()`; the validator still computes the shadow
   set. A resolved-entry table or explicit entry mode would remove the remaining
   write-side coupling.
3. **Legacy compatibility is ad-hoc.** Duplicate names, duplicate
   BASE/tombstone entries and `base_id` without `BASE` are still handled by
   separate branches in the validator.
4. **The C and Python validators are independent implementations.** They
   diverged on the exact `base_id`/`BASE` bug. Share fixtures or generate both
   from one specification.
5. **Endian/bounds helpers are still duplicated** across the reader, partition
   scanner and adapter.
6. **The write capability model is historical.** `fat_is_readonly()` lies by
   name; consolidate into one explicit write-mode query.
7. **`overlay_validate()` keeps O(U²) legacy duplicate resolution.** It only
   matters at a full 2,048-entry overlay; a two-pass key/winner table would
   remove it.

### Fix first

1. Run the hardware power-loss matrix now that the commit paths are isolated.
2. Share validator fixtures between C and `overlay_check.py`.
3. Resolve the remaining legacy entry-mode branches in the validator.
4. Decide the next efficiency step: persisted directory index or a v3
   used-slot summary (see [`efficiency.md`](efficiency.md)).

Stage 11 diagnostics (reason code + failing slot) were completed after V2.1.1:
`overlay_validate()` returns a result struct, the debug screen shows the reason
name and failing entry, and seven host adapter tests assert the reason/slot
pairs.

---

## Efficiency and power

The disk/CPU audit is in [`efficiency.md`](efficiency.md). Implemented:

- Overlay container writes no longer revalidate the immutable container fork or
  reread the native allocation bitmap on every write. Stage 2 validates once;
  `write_container_inline()` only maps extents.

Still open, in priority order:

1. Firmware mount builds the full directory/CNID index by scanning every
   catalog leaf record. Persist it or build it lazily.
2. Overlay mount reads all 2,048 entry slots (640 KiB) even for tiny overlays.
   A v3 used-slot summary is the clean fix.
3. ATA flushes are part of the durability contract; batching them needs a
   WAL/commit-group design and the power-loss matrix.
4. `overlay_validate()` and readdir have CPU-only hotspots that matter mainly
   for large overlays/directories.

Do not trade crash consistency for power without the hardware matrix.

---

## Other deferred decisions

- Existing overlay v1/v2 containers remain readable; do not force a format
  migration without a backup and a tested migration tool.
- The overlay is fixed at creation. A resize/migration tool is still missing.
- On-device database/PictureFlow rebuild remains unsupported; use the Mac
  workflow.
- Unicode normalization/case folding is a documented compatibility limit.
- Non-inline allocation forks and extents-overflow trees fail closed.

---

## XERJ / Corpus Hub request

Do **not** grant blanket MIT relicensing for the whole V1.1 commit. The commit
contains Rockbox/Rockpod-derived and restored upstream code under
GPL-2.0-or-later; only original contributions can be separately licensed.

Safe position:

- Allow inclusion under **GPL-2.0-or-later** with attribution and preserved
  notices.
- Ask for the exact files/lines if they want a separate MIT grant for a subset.
- Ask how the hub stores source/snippets and satisfies GPL source/notice
  requirements.