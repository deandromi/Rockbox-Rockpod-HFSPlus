# agents.md — working contract for Rockbox-Rockpod-HFSPlus

Status: reviewed 2026-11-02 for V3.0.
Target: **iPod Classic / 6G (`ipod6g`, target ID 71) only**.
Public release identity: **V3.0 // Version V13.0** (`docs/VERSION` is `3.0.0-v13.0`).

This checkout may be a source snapshot. Use a real git clone for history, branches and diffs.

---

## 0. Non-negotiables

1. **Native HFS+ metadata is immutable.** Never write the HFS+ catalog, allocation bitmap, extents-overflow tree, volume header, alternate volume header or journal. Creates, grows, truncates, renames, deletes and mtime changes belong in `/.rockpod-rw`.
2. **The overlay is user state.** It contains settings, playlists, tagcache data and generated artwork. Never delete, truncate, zero, recreate or “repair” it without a verified backup and an explicit diagnosis.
3. **Fresh initialization is only for an all-zero container.** A non-zero header, backup, bitmap or entry table that fails validation must fail closed. Do not restore the old “zero the header to reset” behavior.
4. **Never install a bootloader with `--single`.** It destroys the Apple NOR boot path.
5. **Never format, restore, erase or repartition a device to “fix” a software error.** Diagnose mount errors, overlay stages and DFU failures without wiping.
6. **A missing or invalid overlay disables writes.** The adapter keeps the native namespace readable for diagnostics but does not fall back to native writes (`force_readonly`).
7. **Keep HFS+ policy in the HFS+ layer.** Do not add feature checks to generic FAT code when an adapter capability hook exists.
8. **Keep the adapter decomposed.** The old `firmware/hfsplus/hfsplus_rb.c` monolith has been split by responsibility (I/O, format, allocation, store, namespace, FAT, mount, diagnostics). Put new behavior in the owning module; do not recreate a shared mega-file. See §8 and `docs/efficiency.md`.
9. **Do not claim hardware verification without a recorded device run.** Host tests and ARM links are not device tests.
10. **Ask before destructive device operations.** Backups, disk-mode state and recovery paths must be verified first.

---

## 1. Project shape

Rockpod HFS+ is a fork of [Rockpod by nuxcodes](https://github.com/nuxcodes/rockpod), itself a fork of [Rockbox](https://www.rockbox.org).

Custom code:

- `firmware/hfsplus/hfsplus_ro.{c,h}` — read-only HFS+ reader.
- `firmware/hfsplus/hfsplus_partition.{c,h}` — APM/MBR/GPT/raw scanner.
- `firmware/hfsplus/hfsplus_rb.{c,h}` — FAT adapter, overlay and diagnostics.
- `bootloader/ipod-s5l87xx.c` — native-only HFS+ boot integration.
- `tools/hfsplus/` — host inspector and tests.
- `tools/rockpod/` — Mac Cover Flow index/artwork builders, overlay validator and exporter.
- `apps/plugins/pictureflow`, JPEG/album-art fixes and HFS+ diagnostics.

Everything else is inherited Rockbox/Rockpod. Change it only for a specific tested reason.

Target facts:

- 64 MiB RAM, ARM `s5l8702`, ATA/LBA48, 512-byte logical sectors, 4096-byte virtual/physical sector support.
- `HAVE_HFSPLUS_RO` is enabled for non-simulator, non-PCTOOL builds.
- `HFS_NUM_VOLUMES = 1` on `ipod6g`; the adapter has one active HFS+ state. A second partition returns `HFSPLUS_NOT_HFS` so FAT can mount it.
- `HAVE_DIRCACHE` is disabled for HFS+ builds.

---

## 2. Boot and mount chain

```
power-on
  -> Apple NOR boot / dual-boot selector
  -> Rockbox bootloader (Thumb)
     -> storage_init()
     -> disk_mount_all()
        -> hfsplus_partitions()       [canonical scanner]
        -> hfsplus_mount()
           -> hfs_ro_mount()          [primary, alternate-header recovery]
           -> fast index              [firmware only now]
           -> overlay_load()          [firmware only]
        -> fat_mount()                [only on HFSPLUS_NOT_HFS]
     -> load_firmware("/.rockbox/rockbox.ipod")
  -> Rockpod firmware
     -> fat_init() -> hfsplus_init()
     -> same scanner/mount path
     -> applications
```

Important:

- The HFS+ bootloader mounts **native HFS+ only**. It does not load overlay state and does not build the full directory/CNID index.
- The bootloader and firmware use the same partition scanner. A regression there also breaks FAT32 Rockpod.
- The overlay is required for writes. A missing or invalid overlay leaves the volume readable but read-only.

### 2.1 Mount and recovery

- Primary volume header at offset 1024.
- If the primary is unreadable or structurally invalid, a validated alternate header can mount read-only with `recovered=1`; both copies corrupt fail closed.
- HFSX (`HX`) is rejected as not-HFS so the FAT fallback can run.
- Dirty/inconsistent volumes return `-4`. The journal is never replayed.
- The reader supports HFS+ version 4 only.

### 2.2 Indexes

- Directory index: 16,384 parent IDs.
- CNID hash index: 4,096 entries; overflow falls back to a validated full catalog scan.
- Catalog node cache: one 32 KiB node per volume.
- v3 overlays may persist both indexes in a 256 KiB reserved cache, keyed by
  native volume write count/modify date and catalog shape.
- Node offset tables are validated once per node read, not per record.

### 2.3 Reader invariants

- Power-of-two block size >= 512; `blocks * block_size` within the volume window.
- Volume attribute bit 8 set, bit 11 clear.
- Journaled volumes require internal journal, valid info/header and `start == end`.
- Catalog B-tree header/leaf links/offset tables are bounds-checked.
- Records: folder 1, file 2, thread 3/4. Threads are validated and skipped for the namespace.
- Names are UTF-16BE -> UTF-8. `/` becomes `:`, `:` is rejected, `.`/`..` are rejected, invalid surrogates fail.
- Lookup is **exact byte comparison**. No Unicode normalization or case folding.
- Non-inline forks are marked unsupported at open.
- Extents-overflow trees are unsupported.

---

## 3. Writable overlay

### 3.1 Model

- Container: `/.rockpod-rw`, ordinary fully allocated HFS+ file, 16 MiB to 2 GiB, 4 KiB blocks.
- Native HFS+ IDs are CNIDs (`>= 2`); overlay IDs start at `0x40000000`.
- `base_id` is a native backing CNID for a shadowed object.
- The container’s data fork must fit in eight inline extents; fragmented containers fail closed at stage 2.
- The adapter never creates or grows the container. `mkfile 1g` is the Mac creation path.

### 3.2 Persistent formats

Three layouts are readable:

| | v1 `RPOVL11` | v2 `RPOVL12` | v3 `RPOVL13` |
|---|---|---|---|
| Header | block 0 | block 0 | block 0 |
| Header checksum | offset 40 when synced | offset 40, required | offset 40 over 0..63 except the checksum |
| Backup | 64 bytes at offset 2048 | full block 1 | full block 1 |
| Bitmap start | block 1 | block 2 | block 2 |
| Entry table | `(1 + bitmap_blocks) * 4096` | `(2 + bitmap_blocks) * 4096` | `(2 + bitmap_blocks) * 4096` |
| Index cache | none | none | 64 blocks after the entry table |
| `data_start` | `1 + bitmap_blocks + entry_blocks` | `2 + bitmap_blocks + entry_blocks` | `2 + bitmap_blocks + entry_blocks + 64` |

Header fields (little-endian): magic, version at 8, block size 12, max entries 16, total blocks 20, bitmap bytes 24, data start 28, next ID 32, generation 36, checksum 40. v3 adds: entry high-water 44, cache start 48, cache blocks 52, state 56 (`READY=0`, `INIT=1`).

Entry size is 320 bytes:

```
0 id, 4 parent, 8 flags, 12 base_id, 16 size(u64),
24 data_start, 28 data_blocks, 32 mtime, 36 kind(u16),
38 name_len(u16), 40 name[260] (not NUL-terminated on disk)
```

Flags: `USED=1`, `DELETED=2`, `BASE=4`, `TOMBSTONE=8`.

- v2/v3 write the backup before the primary.
- v1 gets a checksum and an in-block backup when its header is next synced.
- Fresh initialization is v3 only. It writes the `INIT` header first, then the
  bitmap/entry table/cache, then publishes `READY`; a power loss is resumable.
- Fresh initialization requires every header, bitmap, entry-table and cache
  block to be zero. Non-zero metadata anywhere fails closed.

### 3.3 Validation stages

| Stage | Meaning |
|---:|---|
| 1 | Look up `/.rockpod-rw` |
| 2 | Container type, inline extents, size/layout |
| 3 | Read header/backup |
| 4 | Header fields/layout |
| 5 | Read bitmap |
| 6 | Reserved metadata bits |
| 7 | Read entry chunk |
| 8 | Decode entry |
| 9 | Ready |
| 10 | Fresh v3 initialization / resume |
| 11 | Whole-overlay validation |

### 3.4 Legacy compatibility

- Duplicate live names are resolved deterministically: newer ID wins; older entry is hidden in RAM. Data is not deleted.
- Duplicate BASE/tombstone entries are resolved the same way.
- A non-`BASE` entry may carry a native `base_id` below `0x40000000`. This is a legacy namespace hint, not an unmaterialized BASE shadow.
- A `base_id` in the overlay ID range is rejected.
- Stage 11 reports a reason code and failing slot; the debug screen shows both.

### 3.5 Allocation and commit

- Persist order: bitmap -> entry/extent -> data -> logical size.
- Realloc copies the old run, persists the new entry, then frees the old run.
- Shrink frees the tail only after the new size is persisted.
- Remove persists the cleared entry before freeing data.
- `storage_flush()` is called after metadata commits when `HAVE_STORAGE_FLUSH` is enabled.
- Old blocks may leak on a failed free; they must never be reused while a live entry points at them.

### 3.6 Namespace merge

- `hfsplus_readdir()` lists native entries then overlay entries; tombstones hide native objects.
- A renamed base directory matches both its logical overlay parent and native backing parent.
- `overlay_hides_base()` uses the validated parent hash and exact-byte names.
- Legacy duplicate shadowing is a RAM-only visibility state and is checked by readdir/open/free paths.

---

## 4. FAT adapter contract

| Hook | Behavior |
|---|---|
| `fat_is_readonly()` | Historical name; true for a mounted HFS+ volume. |
| `hfsplus_writable()` | True only when mounted, overlay ready and not `force_readonly`. |
| `hfsplus_can_write_existing()` | Capability gate; with a ready overlay writes go through the overlay. Without one, the native fork must pass allocation/metadata validation. |
| `hfsplus_mount()` | `0`, `HFSPLUS_NOT_HFS` (-100), or a negative HFS+ error. |
| `hfsplus_create_file()` | Overlay only; ENOSPC maps to `FAT_RC_ENOSPC`. |
| `hfsplus_remove()` | Overlay data; native objects get a tombstone. |
| `hfsplus_rename()` | Overlay only; native objects are shadowed and tombstoned. |
| `hfsplus_closewrite()` | Commits exact size, shrinks, returns a synthetic direntry. |
| `hfsplus_size()` | Size in KiB; free is overlay free when the overlay is ready. |

- Read/write paths are sector-addressed and zero-pad the final sector.
- A write past native EOF without a working overlay is rejected before modifying bytes.
- File errors map to FAT/errno; `-83` historically meant a blocked settings write while the overlay was disabled.

Integration points: `firmware/common/{disk,fat,file,file_internal}.c`, `firmware/export/{fat.h,hfsplus_rb.h}`, `firmware/export/config/ipod6g.h`, `bootloader/ipod-s5l87xx.c`.

- USB mode unmounts the filesystem before exposing the raw disk. The HFS+
  partition geometry is cached during the partition scan, and the USB read
  filter relabels the data partition type to `Apple_HFS` in RAM
  (`0x0B`/`0x0C` -> `0xAF`). The native MBR is never written. Without the
  cached geometry, macOS sees a stale FAT32 type and refuses to probe HFS+.

---

## 5. Limits

| Limit | Value |
|---|---|
| Target | `ipod6g` only |
| Filesystem | HFS+ v4; no APFS/HFSX |
| Journal | clean only; no replay |
| Extents | eight inline; no extents-overflow tree |
| Overlay | 16 MiB minimum, 2 GiB maximum, fixed size; v3 reserves 256 KiB for the index cache |
| Entries | 2,048 |
| Names | exact bytes; no Unicode normalization/case folding |
| Native names | >255 bytes skipped |
| Directory index | 16,384 parents; fallback scan |
| CNID index | 4,096 entries; fallback scan |
| Database workflow | Mac host build; on-device rebuild unsupported |
| Hardware | V2.1.1 confirmed on one Classic 6G; V3.1 Beta 5 read-only USB validated on the same iFlash device |
| USB transfer | HFS+ firmware USB mass storage is read-only; Apple Disk Mode for writes. The USB view relabels the data partition as Apple_HFS in RAM. |

---

## 6. Build and test

Environment: macOS, Apple clang, `arm-elf-eabi-gcc` 9.5.0, GNU Make, Python 3.

```sh
make -C tools/hfsplus test
make -C tools/hfsplus sanitize
python3 tools/hfsplus/tests/check_integration.py
```

Current expected result:

- 20 reader tests.
- 7 partition tests.
- 30 adapter tests (including fault injection, legacy v1/v2 shapes, v3 init
  resume/cache, timestamps and stage-11 reason/slot diagnostics).
- ASan/UBSan passes on macOS.
- macOS host integration mode passes.
- Firmware, Thumb bootloader, database host tool, `mks5lboot`, DFU and `rockbox.zip` build.

Release hashes are in `docs/release-v3.0-beta.md`. Do not reuse historical hashes from this file.

Build commands:

```sh
mkdir -p build-ipod6g && cd build-ipod6g
../tools/configure --target=ipod6g --type=n
make -j"$(sysctl -n hw.ncpu)"
make -j"$(sysctl -n hw.ncpu)" zip

mkdir -p ../build-ipod6g-bootloader && cd ../build-ipod6g-bootloader
../tools/configure --target=ipod6g --type=b --thumb
make -j"$(sysctl -n hw.ncpu)"

tools/rockpod/build-database-macos.sh
make -C utils/mks5lboot CC=clang LDOPTS="-framework IOKit -framework CoreFoundation"
```

The bootloader must remain Thumb and fit `MOVE_AREA`.

---

## 7. Diagnostics

- Healthy overlay: `READY`, stage 9, error 0, slot -1, reason 0 (`none`).
- Debug also reports index-cache HIT/MISS and ATA read/write/flush counters.
- Stage 2: container/extent/allocation validation.
- Stages 3–6: header/bitmap.
- Stages 7–8: entry table/decode; slot identifies the record.
- Stage 11: whole-overlay validation; slot identifies the failing entry and
  reason identifies the check. `-1`/`layout` or `-1`/`next-id` are whole-table
  failures. The debug screen also shows the failing entry fields.
- Stage 11 reasons: 0 none, 1 layout, 2 entry-id, 3 duplicate-id, 4 entry-name,
  5 entry-flags, 6 entry-kind, 7 tombstone, 8 base-id, 9 data-range,
  10 data-size, 11 data-unallocated, 12 data-overlap, 13 parent, 14 next-id.
- `-4`: native HFS+ dirty/inconsistent/journal replay required.
- `diskutil verifyVolume` checks native HFS+ only, not overlay internals.
- Rockbox USB mass storage is read-only on HFS+ firmware builds. Host writes
  return SCSI DATA PROTECT; use Apple Disk Mode for changes. The read-only LUN
  is safe for copying files off and reading `/.rockpod-rw` for diagnostics.
  The USB view relabels the HFS+ data partition as `Apple_HFS` in RAM; the
  raw MBR may still say `0x0B`/`0x0C` and must not be repaired from the host.

---

## 8. Thermo-nuclear code-quality review (V2.1.1)

Post-V2.1.1 decomposition status is annotated inline. The original findings are
retained so the review history is not lost.

Prioritized structural findings. These are maintainability blockers, not cosmetic nits.

1. **Monolith removed (resolved post-V2.1.1).** `hfsplus_rb.c` no longer exists. The adapter is split into `hfsplus_io.c`, `hfsplus_overlay_format.c`, `hfsplus_overlay_alloc.c`, `hfsplus_overlay_store.c`, `hfsplus_overlay_validate.c`, `hfsplus_overlay_load.c`, `hfsplus_namespace.c`, `hfsplus_fat.c`, `hfsplus_index_cache.c`, `hfsplus_mount.c`, `hfsplus_state.c`, `hfsplus_diag.c` and `hfsplus_time.c`. Keep new behavior in the owning module.
2. **`shadowed[]` is still a second source of truth, but reads are centralized.** Every visibility read now goes through `overlay_entry_visible()`; the validator still computes the shadow set. A resolved-entry table (or an explicit entry mode) would remove the remaining write-side coupling.
3. **Legacy compatibility is ad-hoc.** Duplicate names, duplicate BASE/tombstone, and `base_id` without `BASE` are handled by separate conditionals in validation. Introduce an explicit entry mode (`FILE`, `BASE`, `TOMBSTONE`, `LEGACY_BASE`) resolved once at load. This deletes condition chains rather than centralizing them.
4. **Stage 11 diagnostics were opaque (resolved after V2.1.1).** The overlay used to be rejected with `-2`, slot `-1`, so the actual cause required reverse-engineering with Python. `overlay_validate()` now returns a `struct hfs_overlay_validation` with a reason and failing slot; `overlay_load()` applies it to diagnostics, and the debug screen shows both plus the failing entry fields. Extend the enum and the host reason/slot tests together; do not renumber existing values.
5. **The C validator and `tools/rockpod/overlay_check.py` are independent implementations.** They diverged on `base_id`/`BASE`, causing the V2.1.1 bug. Add a shared fixture corpus and run both validators against the same images, or make the Python tool consume a C-generated manifest.
6. **v1/v2 format branches now live in one module (resolved post-V2.1.1).** `hfsplus_overlay_format.c` owns header parse/sync, bitmap/backup offsets and entry serialization. Any future format version belongs there.
7. **`overlay_load()` is isolated (resolved post-V2.1.1).** It is now a separate `hfsplus_overlay_load.c` rather than part of a 2,600-line file. It still runs several phases; split further only with loader tests.
8. **Bootloader lifecycle is separated (resolved post-V2.1.1).** `hfsplus_mount_indexes()` and `hfsplus_mount_overlay()` own the firmware/bootloader differences; `hfsplus_mount()` no longer contains `#ifdef BOOTLOADER` in its main flow.
9. **Endian/bounds helpers are duplicated** across `hfsplus_ro.c`, `hfsplus_partition.c` and the adapter. Use one checked-arithmetic/endian utility header.
10. **The write capability model is historical.** `fat_is_readonly()` lies by name, while `hfsplus_writable()`/`hfsplus_can_write_existing()` express the real policy. Replace with one explicit write-mode/capability query.
11. **`overlay_validate()` uses O(n²) duplicate checks and branch-heavy nesting.** With 2,048 entries this is tolerable, but a two-pass key/winner resolution would be simpler and faster.
12. **The host adapter is one large test file.** Split fixtures, storage stubs and scenario tests; add a regression fixture for the real legacy overlay shape (`base_id` without `BASE`).
13. **Bootloader no longer compiles the overlay loader (resolved post-V2.1.1).** `hfsplus_overlay_load.c` is wrapped out of `BOOTLOADER` builds, and the unused-function warning is gone.
14. **Docs drift is a real defect.** This file and README previously described v1-only overlays, no checksums and 4,096-entry indexes after the code changed. Keep docs and tests updated with format changes.

What is already good:

- HFS+ metadata is protected; writes are overlay-only.
- The v2 header has a checksum and backup; v1 is handled non-destructively.
- Allocation persists before exposure; free-after-commit ordering is explicit.
- Reader bounds, journal, extent and catalog validation are strong.
- Host tests are deterministic and cover failure injection.
- The bootloader skips the full index, and the overlay entry table is read in chunks.

---

## 9. Change recipes

### Overlay behavior/format

1. Decide whether the on-disk format changes. If yes, bump magic/version and provide migration or explicit refusal.
2. Add the operation to the adapter, not generic FAT code.
3. Persist bitmap -> entry/extent -> data -> logical size.
4. Update `overlay_validate()`, `overlay_check.py`, the exporter and the host inspector together.
5. Add create/write/grow/truncate/rename/delete/remount/fault-injection tests.
6. Run `make -C tools/hfsplus test` and `sanitize`.

### HFS+ reader

1. Add a fixture in `tools/hfsplus/tests/test_images.py`.
2. Never call a write callback from `hfs_ro`.
3. Prefer `HFS_RO_FORMAT` over clamping.
4. Run reader tests under ASan/UBSan.
5. Test a real macOS HFS+ and journaled image.

### Partition scanner

1. Add APM/MBR/GPT/raw, 512/4096, malformed-entry cases.
2. Keep iPod fallback behind target policy.
3. Verify FAT still reaches `fat_mount()`.

### Bootloader

1. Build with `--thumb`; check `MOVE_AREA`.
2. Verify `mks5lboot --mkdfu-inst`.
3. Back up and have a DFU recovery plan.
4. Never use `--single`.

---

## 10. Agent rules

Before changing code:

- Read the source, not just names.
- Identify inherited vs custom code.
- Check whether an adapter hook exists.
- If a file exceeds 1,000 lines, decompose or justify it.
- Prefer deleting a layer over polishing it.

While changing code:

- Keep native HFS+ metadata immutable.
- Keep the USB MBR relabel RAM-only; never write the native partition type.
- Preserve persist-before-expose ordering.
- Use checked arithmetic.
- Return specific errors.
- Do not refactor unrelated code or mass-rename.

Before claiming success:

- Run the smallest relevant test, then the full host suite.
- Build firmware and bootloader for shared changes.
- Distinguish host test, ARM link, hardware boot and hardware playback.
- Record what was and was not tested.

Hard stops:

- Destructive device operation proposed.
- Overlay layout unknown/damaged.
- Backup cannot be verified.
- Artifact does not match its manifest.
- Device in unexpected mode.
- A failure could indicate data loss.
- The only proposed fix is format/restore/delete/try another image.

---

## 11. Repository map

| Path | Role |
|---|---|
| `firmware/hfsplus/hfsplus_ro.{c,h}` | Read-only HFS+ reader |
| `firmware/hfsplus/hfsplus_partition.{c,h}` | Partition scanner |
| `firmware/hfsplus/hfsplus_overlay.h` | Internal overlay layout/result contract |
| `firmware/hfsplus/hfsplus_overlay_validate.c` | Whole-overlay validator (stage 11) |
| `firmware/hfsplus/hfsplus_overlay_load.c` | Overlay loader/recovery (empty in bootloader) |
| `firmware/hfsplus/hfsplus_overlay_format.c` | v1/v2 on-disk format and container I/O |
| `firmware/hfsplus/hfsplus_overlay_alloc.c` | Bitmap, runs, growth and materialization |
| `firmware/hfsplus/hfsplus_overlay_store.c` | Entry lifecycle, IDs and hash indexes |
| `firmware/hfsplus/hfsplus_namespace.c` | Native+overlay namespace merge |
| `firmware/hfsplus/hfsplus_fat.c` | FAT/VFS adapter boundary |
| `firmware/hfsplus/hfsplus_index_cache.c` | v3 persisted directory/CNID index cache |
| `firmware/hfsplus/hfsplus_mount.c` | Partition scan and mount lifecycle |
| `firmware/hfsplus/hfsplus_io.c` | Sector I/O and inline writes |
| `firmware/hfsplus/hfsplus_state.{c,h}` | Per-volume adapter state |
| `firmware/hfsplus/hfsplus_diag.{c,h}` | Stage/reason diagnostics |
| `firmware/hfsplus/hfsplus_time.{c,h}` | HFS+/FAT timestamp conversion |
| `firmware/export/hfsplus_rb.h` | Adapter API |
| `firmware/export/fat.h` | HFS+ fields in FAT structs |
| `firmware/common/{disk,fat,file,file_internal}.c` | Integration |
| `bootloader/ipod-s5l87xx.c` | Bootloader and native HFS+ mount |
| `utils/mks5lboot/` | DFU scanner/installer |
| `tools/hfsplus/` | Inspector and reader/partition/adapter tests |
| `tools/rockpod/overlay_check.py` | Canonical Python overlay validator |
| `tools/rockpod/export_overlay.py` | Read-only Mac overlay exporter |
| `tools/rockpod/build_rockpod_*` | Cover Flow index/artwork builders |
| `tools/database/` | Mac tagcache builder |
| `tools/xerj/` | Optional local XERJ semantic index/memory scripts (not part of the firmware) |
| `.xerjignore` | Index scope for the local XERJ node |
| `BUGFIXES.md` | Per-finding status |
| `docs/hfsplus-audit.md` | Evidence log |
| `docs/release-v2.1.1.md` | Current release notes/hashes |
| `docs/release-v2.0.md`, `docs/release-v1.1.md` | Historical releases |
| `README.md` | User install/usage |
| `shipping.md` | Build/release/install runbook |
| `docs/hardware-validation.md` | Hardware test roadmap |
| `docs/efficiency.md` | Disk/CPU power audit and measurement checklist |
| `docs/release-v3.0.md` | V3.0 release notes and hashes |
| `docs/next-steps.md` | Deferred hardware work, thermo-nuclear review and XERJ licensing note |

### 11.1 Optional semantic index

`tools/xerj/` runs the XERJ Community binary as a loopback-only node and keeps
its data in the git-ignored `.xerj-data/`. `.xerjignore` limits indexing to the
project docs and the custom HFS+ host/firmware code; the inherited Rockbox tree,
build directories, backups and `.rockpod-rw` are never indexed.

```sh
tools/xerj/start.sh      # 127.0.0.1:9200, feedback disabled
tools/xerj/index.sh      # reindex after docs/code changes
tools/xerj/recall.sh "why was the overlay rejected at stage 11?"
```

It is a research aid, not a source of truth: retrieved passages must still be
checked against the source and the host tests. Never index device backups,
overlay containers or secrets.

---

## 12. Glossary

- **APM**: Apple Partition Map.
- **CNID**: HFS+ catalog node ID; root is 2.
- **Extent**: contiguous allocation-block run.
- **Inline extents**: the eight descriptors stored in the fork record.
- **BASE**: overlay entry that still references a native object and is not materialized.
- **Materialize**: copy a native fork into overlay blocks.
- **Tombstone**: overlay entry that hides a native object.
- **Overlay**: namespace stored inside `/.rockpod-rw`.
- **`RPOVL11` / `RPOVL12`**: internal overlay format identifiers, not public release versions.
