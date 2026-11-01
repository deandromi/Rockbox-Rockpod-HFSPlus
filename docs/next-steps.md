# Next steps — V2.1.1 and later

Status: V2.1.1 released and confirmed working on the test iPod Classic 6G.
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

The full current review is in [`agents.md`](../agents.md) §8. Top findings:

1. **`hfsplus_rb.c` is ~2,800 lines** and mixes I/O, overlay format,
   allocator, persistence, namespace merge, FAT adapter and diagnostics.
   Decompose before more features.
2. **`shadowed[]` is a second source of truth.** Visibility is checked in
   roughly ten places; one missed check reintroduces the duplicate-name
   failure. Replace it with one `overlay_entry_visible()` predicate or a
   resolved-entry table.
3. **Legacy compatibility is ad-hoc.** Duplicate names, duplicate
   BASE/tombstone entries and `base_id` without `BASE` are handled by separate
   branches. Resolve an explicit entry mode once at load.
4. **Stage 11 cannot report why validation failed.** It returns `-2` with
   slot `-1`; finding the V2.1.1 bug required reverse-engineering. Add a
   reason code and failing slot.
5. **The C and Python validators are independent implementations.** They
   diverged on the exact `base_id`/`BASE` bug. Share fixtures or generate both
   from one specification.
6. **v1/v2 format branches are scattered** through header parse, sync, bitmap
   and backup offsets. A small format driver would delete the conditional
   matrix.
7. **`overlay_load()` is a ~190-line monolith.** Split header/backup recovery,
   fresh initialization, chunked entry reads and index rebuild.
8. **`#ifdef BOOTLOADER` sits inside `hfsplus_mount()`.** Separate the native
   boot mount path from the firmware mount lifecycle.
9. **Endian/bounds helpers are duplicated** across the reader, partition
   scanner and adapter.
10. **The write capability model is historical.** `fat_is_readonly()` lies by
    name; consolidate into one explicit write-mode query.

### Fix first

1. Stage 11 diagnostics: reason code + failing slot.
2. One central overlay visibility predicate/resolved-entry table.
3. Then decompose `hfsplus_rb.c`.

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

### Draft reply

```text
Hi Ivan,

Thanks for the note and the kind words about the V1.1 work.

I can't confirm blanket MIT relicensing for that commit. The repository is
GPL-2.0-or-later, and the commit includes Rockbox/Rockpod-derived and restored
upstream code, so the whole commit isn't mine to relicense under MIT. I'm happy
for it to be included in Corpus Hub under GPL-2.0-or-later with attribution to
me and to deandromi/Rockbox-Rockpod-HFSPlus, provided the GPL notices and
source are preserved.

If you want MIT terms for a specific subset, send me the exact files and lines
you consider solely my original work and I'll review that separately.

I did look at XERJ. The 2.7x token claim is tempting, and I'll try it once my
coding agent stops burning 1.06M tokens reading files one by one. If it can
also retrieve the GPL-2.0 passage about not imposing further restrictions, I'll
consider that a benchmark.

Best,
Deandromi
```
