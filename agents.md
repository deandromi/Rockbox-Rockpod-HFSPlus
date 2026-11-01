# agents.md — working contract for Rockbox-Rockpod-HFSPlus

Status: reviewed 2026-11-01 for V2.1.1.
Target: **iPod Classic / 6G (`ipod6g`, target ID 71) only**.
Public release identity: **Stable V2.1.1 // Version V12.3** (`docs/VERSION` is `2.1.1-v12.3`).

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
8. **Do not grow `firmware/hfsplus/hfsplus_rb.c` further.** It is now ~2,800 lines and mixes I/O, overlay format, allocation, persistence, namespace merge, FAT adaptation and diagnostics. Decompose before adding features. See §8.
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

Two layouts are readable:

| | v1 `RPOVL11` | v2 `RPOVL12` |
|---|---|---|
| Header | block 0 | block 0 |
| Header checksum | offset 40 when synced | offset 40, required |
| Backup | 64 bytes at offset 2048 when synced | full block 1 |
| Bitmap start | block 1 | block 2 |
| Entry table | `(1 + bitmap_blocks) * 4096` | `(2 + bitmap_blocks) * 4096` |
| `data_start` | `1 + bitmap_blocks + entry_blocks` | `2 + bitmap_blocks + entry_blocks` |

Header fields (little-endian): magic, version at 8, block size 12, max entries 16, total blocks 20, bitmap bytes 24, data start 28, next ID 32, generation 36, checksum 40.

Entry size is 320 bytes:

```
0 id, 4 parent, 8 flags, 12 base_id, 16 size(u64),
24 data_start, 28 data_blocks, 32 mtime, 36 kind(u16),
38 name_len(u16), 40 name[260] (not NUL-terminated on disk)
```

Flags: `USED=1`, `DELETED=2`, `BASE=4`, `TOMBSTONE=8`.

- v2 writes the backup before the primary.
- v1 gets a checksum and an in-block backup when its header is next synced.
- Fresh initialization requires the header, backup, bitmap and entry-table blocks to be zero. A non-zero invalid header fails closed.

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
| 10 | Fresh initialization |
| 11 | Whole-overlay validation |

### 3.4 Legacy compatibility

- Duplicate live names are resolved deterministically: newer ID wins; older entry is hidden in RAM. Data is not deleted.
- Duplicate BASE/tombstone entries are resolved the same way.
- A non-`BASE` entry may carry a native `base_id` below `0x40000000`. This is a legacy namespace hint, not an unmaterialized BASE shadow.
- A `base_id` in the overlay ID range is rejected.
- Stage 11 must expose a failing slot/reason in future work; today it reports `-2` with slot `-1`.

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

---

## 5. Limits

| Limit | Value |
|---|---|
| Target | `ipod6g` only |
| Filesystem | HFS+ v4; no APFS/HFSX |
| Journal | clean only; no replay |
| Extents | eight inline; no extents-overflow tree |
| Overlay | 16 MiB minimum, 2 GiB maximum, fixed size |
| Entries | 2,048 |
| Names | exact bytes; no Unicode normalization/case folding |
| Native names | >255 bytes skipped |
| Directory index | 16,384 parents; fallback scan |
| CNID index | 4,096 entries; fallback scan |
| Database workflow | Mac host build; on-device rebuild unsupported |
| Hardware | V2.1.1 confirmed on one Classic 6G; other revisions/adapters unverified |

---

## 6. Build and test

Environment: macOS, Apple clang, `arm-elf-eabi-gcc` 9.5.0, GNU Make, Python 3.

```sh
make -C tools/hfsplus test
make -C tools/hfsplus sanitize
python3 tools/hfsplus/tests/check_integration.py
```

Current expected result:

- 19 reader tests.
- 7 partition tests.
- 17 adapter tests (including fault injection and legacy overlay shapes).
- ASan/UBSan passes on macOS.
- macOS host integration mode passes.
- Firmware, Thumb bootloader, database host tool, `mks5lboot`, DFU and `rockbox.zip` build.

Release hashes are in `docs/release-v2.1.1.md`. Do not reuse historical hashes from this file.

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

- Healthy overlay: `READY`, stage 9, error 0, slot -1.
- Stage 2: container/extent/allocation validation.
- Stages 3–6: header/bitmap.
- Stages 7–8: entry table/decode; slot identifies the record.
- Stage 11: whole-overlay validation; currently no slot detail.
- `-4`: native HFS+ dirty/inconsistent/journal replay required.
- `diskutil verifyVolume` checks native HFS+ only, not overlay internals.
- Do not use Rockbox USB mode for transfers; use Apple Disk Mode.

---

## 8. Thermo-nuclear code-quality review (V2.1.1)

Prioritized structural findings. These are maintainability blockers, not cosmetic nits.

1. **`hfsplus_rb.c` is ~2,800 lines and still growing.** It mixes I/O bouncing, v1/v2 overlay format, allocation, persistence, namespace merge, FAT adaptation and diagnostics. Decompose before any new feature:
   `hfsplus_io.c`, `hfsplus_overlay_format.c`, `hfsplus_overlay_alloc.c`, `hfsplus_overlay_store.c`, `hfsplus_namespace.c`, `hfsplus_fat.c`, `hfsplus_diag.c`.
2. **`shadowed[]` is a second source of truth.** Visibility is enforced in validation, rebuild, hides-base, tombstone, create, file lookup and readdir. One missed check reintroduces a duplicate-name bug. Define one `overlay_entry_visible()` predicate (or a resolved-entry table) and use it everywhere.
3. **Legacy compatibility is ad-hoc.** Duplicate names, duplicate BASE/tombstone, and `base_id` without `BASE` are handled by separate conditionals in validation. Introduce an explicit entry mode (`FILE`, `BASE`, `TOMBSTONE`, `LEGACY_BASE`) resolved once at load. This deletes condition chains rather than centralizing them.
4. **Stage 11 cannot say why it failed.** The overlay was rejected with `-2`, slot `-1`; the actual cause required reverse-engineering with Python. `overlay_validate()` should return a reason code and failing slot, and the debug screen should show them.
5. **The C validator and `tools/rockpod/overlay_check.py` are independent implementations.** They diverged on `base_id`/`BASE`, causing the V2.1.1 bug. Add a shared fixture corpus and run both validators against the same images, or make the Python tool consume a C-generated manifest.
6. **v1/v2 format branches are spread across header parse, sync, bitmap offsets and backup offsets.** A small `overlay_format` driver (`bitmap_start`, `backup_offset`, `parse`, `write`) would remove the conditional matrix.
7. **`overlay_load()` is a 190-line monolith** mixing lookup, format selection, backup recovery, fresh initialization, chunked entry reads, validation and index rebuild. Split loader phases from format parsing.
8. **`#ifdef BOOTLOADER` lives inside `hfsplus_mount()`.** The bootloader path (native-only, no index, no overlay) should be its own mount function; conditional branches inside a large function hide two different lifecycles.
9. **Endian/bounds helpers are duplicated** across `hfsplus_ro.c`, `hfsplus_partition.c` and the adapter. Use one checked-arithmetic/endian utility header.
10. **The write capability model is historical.** `fat_is_readonly()` lies by name, while `hfsplus_writable()`/`hfsplus_can_write_existing()` express the real policy. Replace with one explicit write-mode/capability query.
11. **`overlay_validate()` uses O(n²) duplicate checks and branch-heavy nesting.** With 2,048 entries this is tolerable, but a two-pass key/winner resolution would be simpler and faster.
12. **The host adapter is one large test file.** Split fixtures, storage stubs and scenario tests; add a regression fixture for the real legacy overlay shape (`base_id` without `BASE`).
13. **The bootloader still compiles unused `overlay_load()` code.** Wrap it out of `BOOTLOADER` builds to reduce size and remove the warning.
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
| `firmware/hfsplus/hfsplus_rb.c` | FAT adapter + overlay (decompose) |
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
