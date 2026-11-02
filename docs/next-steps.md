# Next steps — V2.1.1 and later

Status: V3.0 Beta built from the V2.1.1 hardware baseline. The adapter is
decomposed, the v3 overlay format and persistent index cache are implemented,
and the write fast path is in. Host/build verified; no device, power-loss or
power measurement has been recorded for the v3 changes.
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
   allocation, store, validator, loader, namespace, FAT, index-cache, mount,
   state, diagnostics and time modules.
2. **`shadowed[]` reads are centralized.** Every visibility read goes through
   `overlay_entry_visible()`; the validator still computes the shadow set.
3. **Legacy compatibility is ad-hoc.** Duplicate names, duplicate
   BASE/tombstone entries and `base_id` without `BASE` are still handled by
   separate branches in the validator.
4. **The C and Python validators are independent implementations.** Share
   fixtures or generate both from one specification.
5. **Endian/bounds helpers are still duplicated** across the reader, partition
   scanner and adapter.
6. **The write capability model is historical.** `fat_is_readonly()` lies by
   name; consolidate into one explicit write-mode query.
7. **`overlay_validate()` still resolves duplicate names with O(U²) work.**
   Duplicate-ID validation is now independent and hash-based; a two-pass
   key/winner table would finish the job.

### Fix first

1. Run the hardware power-loss matrix and device tests on the V3.0 beta.
2. Add a tested v2→v3 overlay migration tool if existing users need the cache.
3. Share validator fixtures between C and `overlay_check.py`.
4. Finish the two-pass duplicate-name resolution.
5. Measure the v3 cache/high-water power effect on the iFlash device.

Stage 11 diagnostics (reason code + failing slot) were completed after V2.1.1:
`overlay_validate()` returns a result struct, the debug screen shows the reason
name and failing entry, and seven host adapter tests assert the reason/slot
pairs.

---

## Efficiency and power

The disk/CPU audit is in [`efficiency.md`](efficiency.md). Implemented in V3.0:

- Container writes skip repeated native-bitmap validation.
- v3 persists the directory/CNID index cache.
- v3 high-water bounds the entry-table read.
- A redundant entry sync after realloc was removed.
- Debug reports cache HIT/MISS and ATA read/write/flush counters.

Still open, in priority order:

1. Existing v2 containers do not get the v3 cache without a migration tool.
2. ATA flush batching still needs a WAL/commit-group design and the hardware
   power-loss matrix.
3. Full duplicate-name resolution is still O(U²).
4. Readdir hidden-name caching remains a low-priority optimization.

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