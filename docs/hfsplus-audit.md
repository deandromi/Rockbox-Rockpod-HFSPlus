# HFS+ implementation audit log

Status: **audit history through V3.0 Beta; six documented
format/compatibility limits remain (see `BUGFIXES.md`).** The prose below records
findings at discovery time and is intentionally historical; current status
lives in `BUGFIXES.md` and the release notes. V3.0 Beta is host/build verified
and awaits device/power validation.

Date started: 2026-11-01.
Scope: `firmware/hfsplus/*`, `firmware/export/hfsplus_rb.h`, the HFS+ hooks in `firmware/common/{disk,fat,file,file_internal}.c`, `bootloader/ipod-s5l87xx.c`, `tools/hfsplus`, `tools/rockpod`, and the HFS+-specific diagnostics.
Method: source audit, host reader tests, real macOS HFS+ images, a temporary host adapter harness with an in-memory disk and a real HFS+ container, and targeted fault/corruption injection.
Out of scope: the long-standing stock Rockbox USB mass-storage corruption bug reported by the device owner. It is tracked separately as an upstream Rockbox issue, although the overlay’s crash consistency still matters when a disconnect or reset occurs.

Severity:

- **S0 / critical**: silent data loss, overlapping allocations, memory safety, or namespace corruption.
- **S1 / high**: wrong file/partition selected, failed-write data changes, unrecoverable mount state, or a contract violation that can become data loss.
- **S2 / medium**: robustness/validation gaps, performance limits, or misleading behavior with a clear trigger.
- **S3 / low**: cosmetic, diagnostic, documentation, or future-proofing issues.

# Confirmed findings

### HFS-001 — Overlay mounts without checking entries against the allocation bitmap; allocator reuses live blocks

**Severity: S0.**

`overlay_load()` validates the header, reserved bitmap bits and each entry’s field bounds, but never verifies that every used entry’s `data_start..data_start+data_blocks` range is marked used in the bitmap. `overlay_rebuild_indexes()` then counts free blocks from the bitmap alone, and `overlay_alloc_run()` hands those blocks to new files.

Consequence: after a torn bitmap write, a lost write, or a deliberately damaged bitmap, the overlay reports `READY` and a new file can be allocated over a live file. Writes to the new file silently destroy the old one.

Reproduction with the temporary host adapter harness:

1. Create a real HFS+ image with `.rockpod-rw`.
2. Create `data.bin`, write 5,000 bytes, close. Overlay entry: `data_start=162`, `data_blocks=8`.
3. Clear bitmap bits 162..169 in the container.
4. Remount: `mount=0`, `overlay=1`, `stage=9`, `error=0` — reported healthy.
5. Create `second.bin`, write 5,000 bytes.
6. Read `data.bin` again: **5,000 of 5,000 bytes changed**. The two files now share the same blocks.

Expected: the loader must reject the overlay (or repair it from a verified backup) when an entry range is not fully allocated, ranges overlap, or the free count disagrees with the entries. It must not allocate over a live entry.

### HFS-002 — Overlay frees blocks before persisting the entry that references them

**Severity: S0 (confirmed with fault injection; same consequence as HFS-001).**

The crash-consistency order is wrong in several paths:

- `overlay_realloc()` allocates a new run, copies the data, updates the in-memory entry, then calls `overlay_free_run()` on the old run. The entry is synced by the caller **after** `overlay_realloc()` returns. A power loss between freeing the old run and syncing the entry leaves the on-disk entry pointing at freed blocks.
- `overlay_shrink()` frees the trailing blocks before `hfsplus_closewrite()` syncs the reduced `data_blocks`/size. A failed sync leaves the on-disk entry claiming the freed tail.
- `hfsplus_remove()` with `FAT_RM_DATA` frees the entry’s blocks, then removes indexes and clears the entry. A failed entry clear leaves an on-disk entry pointing at freed blocks.
- `hfsplus_closewrite()` calls `overlay_ensure_capacity()` (which may realloc/free) and `overlay_shrink()` (which may free) without an intervening entry sync. Any later error leaves the on-disk entry stale.

Combined with HFS-001, this turns a normal power loss, reset, or ATA write failure into overlapping allocations and silent file overwrite.

Fault-injection reproduction:

1. Create `a.bin` (5,000 bytes) and `b.bin` (5,000 bytes). `a` occupies blocks 162..169; `b` occupies 170..177.
2. Open `a.bin` and request a 100-sector write. The adjacent blocks are occupied, so `overlay_realloc()` allocates blocks 178..193, copies `a`'s data, frees 162..169, and updates the in-memory entry.
3. Inject a failure on the entry-table sector write (physical sector 23352 in the test image). `hfsplus_readwrite()` returns `-1`; the on-disk entry still says `a.bin: data_start=162, data_blocks=8`, while the bitmap marks 162..169 free and 178..193 used.
4. Remount: overlay reports `READY`, stage 9, error 0.
5. Create `c.bin` and write 5,000 bytes. It reuses 162..169.
6. Read `a.bin` again: **5,000 of 5,000 bytes changed** — silent overwrite.

Expected order: allocate new blocks → copy data → persist the new entry/backing run → only then free the old blocks. For shrink/delete, persist the smaller/cleared entry first, then free; a leak is preferable to a dangling reference.

### HFS-003 — Base-file long-name `strcpy` overflows `fat_direntry`

**Severity: S0 (confirmed with AddressSanitizer).**

`hfsplus_closewrite()` fills a base-file `fat_direntry` with:

```c
strcpy((char *)fatentp->name, base.name);
```

`base.name` can be up to 1,020 UTF-8 bytes from the HFS+ reader. `struct fat_direntry.name` is declared as `[255+1+4]` (260 bytes) inside a union whose storage is 650 bytes. A valid HFS+ name of 765 bytes (255 CJK characters) produced:

```text
ERROR: AddressSanitizer: stack-buffer-overflow
WRITE of size 766
#0 strcpy
#1 hfsplus_closewrite hfsplus_rb.c:1203
```

The name is reachable through `hfsplus_open()` even though `hfsplus_readdir()` skips base names longer than `FAT_DIRENTRY_NAME_MAX`. The adapter must reject such names at open time or use a bounded copy, and the base-file close path must never `strcpy` an unbounded HFS+ name.

Related: `fill_overlay_fatent()` also uses `strcpy`; overlay entries allow up to 260 bytes while the declared `fat_direntry.name` is 260 bytes including the NUL. The union’s 650-byte storage prevents an immediate stack overflow for 260, but the adapter violates the 255-byte `FAT_DIRENTRY_NAME_MAX` contract and can confuse consumers.

### HFS-004 — Renaming a base directory orphans pre-existing overlay children and tombstones

**Severity: S0 (confirmed).**

Overlay entries store the parent as either a native CNID or an overlay ID. When a base directory is shadowed/renamed, `overlay_shadow_base()` gives it a new overlay ID. Existing overlay entries under that directory still have `parent = old base CNID`. `hfsplus_readdir()` then uses the new overlay ID as `logical_parent`, so it cannot see those entries.

Confirmed both directions:

- Delete `dir/child.txt` (tombstone parent = base `dir` CNID), then rename `dir` to `dir2`. Listing `dir2` **reappears** `child.txt`; the tombstone is invisible under the new overlay ID.
- Create an overlay-only `dir/new.txt` (parent = base `dir` CNID), then rename `dir` to `dir2`. Listing `dir2` **loses** `new.txt`; the file is invisible under the new overlay ID.

This is a namespace consistency bug that can look like files appearing or disappearing. It can also make a non-empty directory appear empty to `rmdir` logic.

The remedy is structural: introduce a stable logical-parent mapping (base CNID ↔ overlay ID) used consistently by readdir/open/rename/remove, or migrate all entries whose parent is the old CNID when a base directory is shadowed. A one-off fix in `hfsplus_readdir()` would leave `open`, `remove` and tombstone creation inconsistent.

### HFS-005 — Entry `size` is not checked against `data_blocks` capacity

**Severity: S1 (confirmed read beyond allocation).**

`overlay_decode_entry()` accepts any `size`; it does not require `size <= data_blocks * 4096`. `overlay_data_read()` checks `offset/length` against `size`, then reads from the container at `data_start*4096 + offset`; it does not check the requested range against the allocated run.

A temporary harness patched a live entry from `size=5000, data_blocks=8` to `size=40000`. The overlay still mounted `READY`; reading past 32,768 bytes returned data from beyond the file’s allocation. In a healthy overlay that area may belong to another file, leaking or corrupting it; `overlay_realloc()` can also copy beyond the old run because its loop uses the uncapped `size`. The same test with `data_start` pointing into metadata is rejected by the data-read/write guards, but the loader still accepts the entry.

Expected: reject `size > data_blocks * 4096` at load, or clamp reads to capacity, and bound the realloc copy by the old capacity.

### HFS-006 — MBR 4096-byte virtual-sector heuristic can select the wrong geometry and never try the real 512-byte partition

**Severity: S1 (confirmed).**

`hfsplus_partitions()` interprets an MBR `Apple_HFS` entry using 4,096-byte units before the generic scanner. It does not probe the candidate for `H+`/`HX` before returning it. If a 512-byte-unit MBR partition is small enough that the 4,096-byte interpretation still fits inside the disk, the wrong offset is returned; `hfsplus_mount()` then fails, and the real 512-byte partition is never tried.

Reproduction: a 128 MiB disk with a valid HFS+ volume at 512-byte LBA 64 and a fake `H+` at `64*4096 + 1024` produced:

```text
parts=1 mult=8
part 0 type af start 512 size 261632
mount=-3 overlay=0
```

The real volume at LBA 64 was ignored. `mult=8` also misreports the sector geometry to the storage layer.

Expected: probe the 4,096-unit candidate for a valid HFS+ signature before accepting it; if absent, try the storage-sector interpretation. The same rule should apply to the generic MBR path.

### HFS-007 — Raw FAT-like boot sector hides a valid MBR partition table

**Severity: S1 (confirmed).**

`hfs_partition_scan()` probes offset 0 for a raw volume **before** parsing the MBR. A boot sector with `0x55aa`, a plausible bytes-per-sector, sectors-per-cluster and FAT-count is treated as a whole-disk raw FAT volume, even when the MBR contains a valid `Apple_HFS` partition.

Reproduction: a valid MBR with an `Apple_HFS` partition and a boot sector that looks like FAT returned:

```text
No HFS+ partition candidate
```

The real HFS+ partition was ignored. This affects both HFS+ and FAT targets because the HFS+ fork replaces the legacy scanner.

Expected: parse a valid MBR/APM/GPT first; only fall back to raw-volume probing when no partition table is present or no partition is usable.

### HFS-008 — GPT 4096-first attempt aborts the 512-byte attempt on a malformed plausible header

**Severity: S1 (confirmed).**

For a protective MBR, `hfs_partition_scan()` tries GPT with 4,096-byte units first. If the 4,096-byte read starts with `EFI PART` but fails any validation, `gpt()` returns `HFS_RO_FORMAT`; the loop returns that immediately and never tries the real 512-byte GPT.

Reproduction: a valid 512-byte GPT image with the four bytes `EFI PART` written at offset 4,096 produced:

```text
Partition error -2
```

The valid GPT at LBA 1 was ignored.

Expected: try both unit sizes when the first candidate is not a fully valid GPT, or choose the unit size from the protective MBR/target policy and validate before committing.

### HFS-009 — Non-overlay write extending EOF reports success after writing only a prefix

**Severity: S1 (confirmed).**

Without a writable overlay, `hfsplus_readwrite()` allows a base file write if `offset < native_size`, clamps `writable` to the remaining native bytes, writes only that prefix, but sets `got = requested` and returns success for all requested sectors. `hfsplus_closewrite()` later returns `HFS_RO_UNSUPPORTED` because the size changed.

Reproduction on a 10-byte base file, one 512-byte write at offset 0:

```text
size=10 can=1
write=1
close=-3 size=10
```

The first 10 bytes were overwritten even though the logical extension failed.

Expected: reject `offset + requested > native_size` before writing anything, or report the true short write and do not modify the file when the operation cannot be committed.

### HFS-010 — `hfsplus_mount()` leaves a volume marked mounted when fast-index construction fails

**Severity: S1 (confirmed).**

`hfs_ro_mount()` sets `v->mounted = 1`. `hfsplus_mount()` then calls `hfs_ro_enable_fast_index()`. If that fails (for example, a malformed leaf record that the mount did not validate), it returns the error without calling `hfs_ro_unmount()`. `hfsplus_mounted(volume)` remains true, so a later `hfsplus_mount()` on the same volume returns `HFS_RO_ARGUMENT` and the next partition cannot be mounted through that volume slot.

Reproduction: a synthetic image with a corrupted leaf-node kind produced:

```text
mount1=-2 mounted=1 stage=2 err=-2
mount2=-8 mounted=1
```

Expected: unwind the partial mount on every failure path, or build the directory index before publishing `mounted`.

### HFS-011 — Overlay free count and bitmap can drift without detection

**Severity: S2/S1 depending on corruption.**

`overlay_rebuild_indexes()` recomputes `free_blocks` from the bitmap only. It does not reconcile:
- used entries whose bitmap bits are clear;
- free bitmap bits covered by an entry;
- overlapping entry ranges;
- `size > data_blocks*4096`;
- duplicate IDs;
- entries whose `parent` is a file, tombstone or missing object.

HFS-001 demonstrates the dangerous case. The other cases can leak space or produce duplicate/invisible entries. A single `overlay_validate()` at load should establish these invariants and refuse the overlay when they fail.

### HFS-012 — `overlay_free_run()` can free blocks that belong to another entry

**Severity: S2 (conditional on corrupt metadata).**

`overlay_free_run()` validates only the numeric range against the container; it does not verify that the blocks are currently allocated to the entry being freed. If an entry’s `data_blocks` is corrupt or overlaps another entry, removal frees another file’s blocks. Combined with HFS-001, this is another path to overlap.

### HFS-013 — Duplicate overlay IDs and duplicate BASE shadow entries are not rejected

**Severity: S2.**

`overlay_rebuild_indexes()` inserts every used entry into the ID hash without a uniqueness check. `overlay_create_entry()` and `overlay_shadow_base()` do not check whether a BASE entry already exists for the same base object/parent/name. A corrupt entry or a second writer can create duplicate IDs or duplicate shadows; `overlay_find_id()` returns one, the other becomes inaccessible, and readdir can list duplicate names.

### HFS-014 — `next_id` from the header can be `0xffffffff`

**Severity: S2 (confirmed).**

The header validation only requires `next_id >= 0x40000000`. `overlay_new_id()` resets only when `next_id < 0x40000000` or `next_id == 0x7fffffff`. A header value of `0xffffffff` creates an ID outside the intended range and then wraps to 0. The loader should reject or normalize anything above `0x7fffffff`.

Reproduction: patched an initialized overlay header to `next_id=0xffffffff`; `hfsplus_create_file()` returned ID `4294967295` (`0xffffffff`) and the entry was persisted. On a 32-bit `long`, that ID becomes `-1` in `fat_file.firstcluster`/`fat_direntry.firstcluster`, outside both the native CNID and overlay ID contracts.

## Additional code-confirmed risks

| ID | Severity | Finding |
|---|---|---|
| HFS-020 | S2 | `overlay_data_read()` does not check `data_start + data_blocks <= total_blocks`; container bounds catch the final read, but the entry is accepted as valid. |
| HFS-021 | S2 | `overlay_decode_entry()` uses `HFS_OVL_MAX_BLOCKS`, not the volume’s actual `total_blocks`, for data bounds. |
| HFS-022 | S2 | `overlay_realloc()` copy loop is `while (copied < e->size && oldblocks)`; `oldblocks` never changes, so a corrupt `size` copies beyond the old run. |
| HFS-023 | S2 | `overlay_shadow_base()` rollback zeroes the entry but leaves its ID/parent hash links. |
| HFS-024 | S2 | `hfsplus_open()` accepts a non-inline fork; `hfsplus_can_write_existing()` can report writable when the overlay is ready. The failure appears later in `hfs_ro_pread()`/`overlay_materialize_base()`. |
| HFS-025 | **S1 (confirmed)** | `fat_file_is_same()` compares `firstcluster` only. Two open handles for `base.txt` (both base ID 19) each wrote via `overlay_shadow_base()` and produced two live `base.txt` entries (IDs `0x40000000` and `0x40000002`) plus a tombstone. Readdir listed `base.txt` twice; the two handles wrote divergent data. The adapter must recognize an existing shadow for the same base object/parent/name, or make `fat_file_is_same()` compare the base identity. |
| HFS-026 | S2 | `overlay_hides_base()` and `overlay_tombstone()` compare names with `strcasecmp`; native lookup is exact-byte. Case-only duplicates can behave differently in the two namespaces. |
| HFS-027 | S2 | `hfs_ro` directory index is capped at 4,096 parents; beyond that, every unresolved lookup scans the catalog. |
| HFS-028 | S2 | `hfs_ro_find_id()` scans the entire catalog from `first_leaf`; used by shadow/remove/closewrite. |
| HFS-029 | S2 | `node_record()` validates the whole offset table on every record read; O(records²) per node. |
| HFS-030 | S2 | `overlay_hides_base()` is O(2,048) per native directory entry; the maintained parent hash is never read. |
| HFS-031 | S2 | `hfsplus_partitions()` contains a hard-coded `16510 * 4096` fallback and iPod virtual-sector policy in generic adapter code. |
| HFS-032 | S2 | HFSX passes the `HX` probe, then `hfs_ro_mount()` rejects it with `-3`; `disk_mount()` does not try FAT. Decide the boundary explicitly. |
| HFS-033 | S2 | `disk_init()` compiles out the legacy FAT partition scanner when `HAVE_HFSPLUS_RO` is defined; FAT support depends entirely on `hfsplus_partitions()`. |
| HFS-034 | S2 | Static per-volume HFS+ state is ~733 KiB for the overlay plus caches; four volumes reserve roughly 3 MiB of BSS. |
| HFS-035 | S2 | Global diagnostics (`last_error`, `overlay_diag_*`) are not per-volume. |
| HFS-036 | S2 | `hfsplus_closewrite()` ignores `hfs_ro_find_id()` failure when filling a base-file `fat_direntry`; it returns success with an empty entry. |
| HFS-037 | S2 | A write to an overlay entry that is `USED|DELETED` is not rejected; `overlay_file_entry()` checks `USED` and ID but not `DELETED`. |
| HFS-038 | S2 | Overlay header `generation`/`next_id` are stale after normal use; only fresh initialization writes the header. |
| HFS-039 | S2 | No `storage_flush()` at overlay commit points. The ATA write cache can reorder or lose the “bitmap before entry before data” order on power loss/reset. |
| HFS-040 | S2 | A magic mismatch in the overlay header triggers destructive fresh initialization; there is no backup header or checksum to distinguish “new container” from “damaged header”. |
| HFS-041 | S2 | `overlay_load()` does not validate parent-child relationships; orphaned entries and entries parented to files/tombstones are accepted. |
| HFS-042 | S2 | `hfs_ro` does not validate that fork extents are in logical order; a malformed fork can silently produce wrong data. |
| HFS-043 | S3 | `hfs_ro` journal info flags must be exactly `1`; other valid flag combinations are rejected as unsupported. |
| HFS-044 | S3 | HFS+ UTF-16 names are converted to UTF-8 without normalization; `/` maps to `:`, `:` is rejected. |
| HFS-045 | S3 | `hfsplus_size()` returns `min(native_free, overlay_free)` when the overlay is ready; this underreports usable overlay space. |
| HFS-046 | S3 | README says parent-directory RAM indexes are used for overlay lookups; the parent hash is maintained but never queried. |
| HFS-047 | S3 | Base HFS+ names longer than 255 bytes are hidden by `hfsplus_readdir()` but not rejected by `hfsplus_open()`; this enabled HFS-003. |
| HFS-048 | S3 | Overlay names up to 260 bytes exceed the `FAT_DIRENTRY_NAME_MAX` 255-byte contract. |
| HFS-049 | S3 | `hfs_ro` name decoding rejects `:` and maps `/` to `:`; this is a compatibility limitation, not a crash. |
| HFS-067 | **S2 (confirmed)** | Overlay-full conditions return `HFS_RO_BUFFER` (-9). `create_stream_internal()` maps only `FAT_RC_ENOSPC` (-10) to `ENOSPC`; a full entry table or full container therefore surfaces as `EIO`, not “disk full”. Reproduction: 2,048 empty files created; file 2,049 returned `rc=-9` from `hfsplus_create_file()`. |
| HFS-068 | S2 | The direct MBR 4,096-unit path clamps a partition length that extends past the disk instead of rejecting it. A malformed MBR can therefore mount a truncated volume rather than failing closed. |
| HFS-069 | S3 | `hfsplus_open()` does not verify that an overlay entry’s stored `parent` equals the `parent` argument; an ID passed from the wrong directory opens the overlay entry. The file layer normally supplies the correct parent, but the API contract is unchecked. |
| HFS-070 | S3 | `hfsplus_closewrite()` ignores `hfs_ro_find_id()` failure when filling a base-file `fat_direntry`; it returns success with an empty entry. |

## Confirmed integration / UI findings

| ID | Severity | Finding |
|---|---|---|
| HFS-075 | S2 | `build_rockpod_pictureflow_cache.py` falls back to the track `artist` when `album_artist` is absent, but tagcache stores `<Untagged>` in `database_7.tcd` for that case, and PictureFlow hashes `get_album_artist()` from that table. Result: the generated cache filename does not match the firmware filename. Host example: `No AA Album` / `No AA Artist` produced `cfb8c73efa53347.pfraw`; firmware would look for `cfb8c73eab0cdf30.pfraw` (`<Untagged>`). |
| HFS-081 | S2 | `apps/debug_menu.c` `dbg_disk_info()` returns from inside the `#ifdef HAVE_HFSPLUS_RO` overlay block. On HFS+ builds the subsequent `SSD detected`, spin-up, power-management, SMART, flush-cache, PIO and timing lines are never added. The early `return btn;` should be moved after the remaining disk-info lines. |
| HFS-082 | **S1** | `pictureflow.c` `read_pfraw()` trusts the `.pfraw` header: it does not check the header read, does not validate `width`/`height`, computes `sizeof(dim) + sizeof(pix_t)*width*height` in `int`, and does not check the data read. A corrupt/truncated `.pfraw` (exactly the kind of file a filesystem incident can produce) can cause a small allocation followed by an oversized/negative read, or display uninitialized pixels. |
| HFS-083 | **S1** | `pictureflow.c` `load_album_index()` reads `artist_len`/`album_len` from the file into `size_t`, adds them into an `unsigned int` `name_sz` (overflow possible), checks `name_sz + album_idx_sz` against the buffer, then reads the two name pools. A corrupt index with huge lengths can overflow the destination buffer or pass a huge length to `read2buf()`; `read2buf()` also casts the byte count to `int`. |
| HFS-084 | **S1** | `pictureflow.c` `save_pfraw()` ignores both `rb->write()` return values and always returns true after closing. A full disk or I/O error can leave a truncated `.pfraw` that is then treated as a successful cache entry; combined with HFS-082, this is a crash/corruption path. It also removes the old file before saving. |
| HFS-085 | S2 | HFS+ readdir/fill paths leave `fat_direntry.wrtdate`/`wrttime` at zero. `dir_get_info()` converts those DOS fields, so every HFS+ file and directory reports a bogus/zero timestamp. `hfs_ro_entry` does not parse HFS+ create/content-modify dates, and `hfsplus_modtime()` stores an overlay mtime that is never exposed through readdir. For a base (not-yet-shadowed) file, `hfsplus_modtime()` returns success without storing anything; the requested time is silently lost. |
| HFS-086 | **S1 (hypothesis, not device-confirmed)** | The documented on-device tagcache/database instability writes many growing files through the overlay while the device may be powered off/reset. HFS-001/HFS-002 can tear bitmap/entry state and produce exactly the “database/cache corruption” symptoms; this is a plausible contributor, not proof. The host database workflow avoids the risk. |
| HFS-087 | S2 | `pictureflow.c` `save_album_index()` checks only `creat()`; it ignores every `rb->write()` return and returns 0 even if the header/name/index writes fail. A partial `pictureflow_album.idx` is treated as saved and later read by HFS-083. |
| HFS-088 | **S1 (confirmed)** | `overlay_id_insert()` can fail to insert after the 4,096-entry ID hash has accumulated enough tombstones and live entries: it only commits when it finds a `NONE` slot, using a remembered tombstone. After enough create/delete churn in one session, a new entry may be unindexed and `hfsplus_open()` by ID fails. A remount rebuilds the hash, masking the bug. Reproduction: 5,000 create/remove cycles, then create `final`; readdir listed it with ID `1073746824`, but `hfsplus_open()` returned `-5` (not found). |
| HFS-089 | S2 | Most adapter entry points index `states[IF_MV_VOL(file->volume)]` without validating the volume. Public functions such as `hfsplus_sector_size()`/`hfsplus_cluster_size()` also index directly. A corrupted/uninitialized `fat_file` volume can cause an out-of-bounds state access; the file layer normally supplies a valid volume, but the boundary is unchecked. |
| HFS-090 | **S1** | Tombstones are matched by `parent + base_id` only; `overlay_hides_base()` deliberately ignores the tombstone name. HFS+ may reuse CNIDs (`kHFSCatalogNodeIDsReusedBit`, volume attribute bit 12 is not checked). A new base object that reuses a deleted object’s CNID is hidden by the stale tombstone, even if its name differs. The reader should check CNID reuse or tombstones should match a stronger identity. |
| HFS-091 | S2 | `overlay_tombstone()`/`overlay_shadow_base()` can create multiple overlay entries for the same base object; `overlay_create_entry()` does not enforce parent/name uniqueness. HFS-025 shows two live shadows from two handles; the same class applies to a tombstone plus a later shadow. |
| HFS-092 | S3 | The reserved name `.rockpod-rw` is only filtered from root readdir and used for the container lookup. `hfsplus_create_file()` does not reject it, so a user can create an overlay entry with that name in `.rockpod-rw`’s place (it is not the container, but the namespace becomes confusing and the container remains hidden). |
| HFS-093 | S3 | `bootloader/ipod-s5l87xx.c` prints `HFS+ v8 mount error` whenever `hfsplus_last_error()` is nonzero after any mount failure. A plain FAT disk that fails to mount can therefore produce a misleading HFS+ diagnostic. The `HFS+ read-only test` message is also stale/misleading now that the overlay is writable. |
| HFS-094 | S3 | `hfs_ro` does not validate the B-tree `keyCompareType` (0xCF case-insensitive for HFS+, 0xBC for HFSX); HFSX is rejected earlier, but a malformed catalog can carry an unexpected compare type without rejection. |
| HFS-095 | S3 | `hfs_partition_scan()` skips extended/logical MBR partitions; primary HFS/FAT partitions are the tested layout. Unusual iPod preparation with an HFS+ logical partition would not be found. |
| HFS-096 | S3 | `hfsplus_partitions()` sets a single sector multiplier for all returned partitions. If partitions use different unit sizes, the USB/ATA geometry cannot represent both. The tested layouts use a uniform unit. |
| HFS-097 | S3 | `hfsplus_mount()` accepts a partition whose bytes at `base+1024` begin with `H+`/`HX` even if it is not a real HFS+ volume; `hfs_ro_mount()` then fails and `disk_mount()` does not try FAT. A false signature can hide a FAT partition. |
| HFS-098 | S3 | `hfsplus_mount()` leaves `last_error`/`last_stage` and overlay diagnostics stale on success; diagnostic displays can show a previous partition’s failure. |
| HFS-099 | **S2 (confirmed)** | `hfs_ro` does not validate that extents are in logical order; a malformed fork can read data in the wrong order without a format error. Reproduction: swapping the two valid extents of `/Music/track.bin` in the synthetic fixture returned 5,003 bytes with success; the first 907 bytes matched, then the output diverged (byte 907 expected 42, got 0). HFS+ has no per-file data checksum, so a damaged extent array is a silent data-integrity problem; the reader can at least reject non-monotonic physical starts as a heuristic, but cannot recover the intended order. |
| HFS-100 | S3 | `hfs_ro` accepts only journal info `flags == 1`; other valid flag combinations are reported as unsupported. |
| HFS-101 | S3 | `hfs_ro` does not parse or expose HFS+ volume labels, file create/modify dates or permissions; the adapter exposes only size/kind/name and a placeholder mtime. |
| HFS-102 | S2 | `pictureflow.c` `get_album_name()`, `get_album_name_idx()`, `get_albumart_for_index_from_db()` and `create_track_index()` index `pf_idx.album_index[slide_index]` without a common bounds check. Normal UI paths clamp `center_index` to `album_ct`, but corrupt index state or a future caller can pass an out-of-range value. |
| HFS-103 | **S1** | `load_album_index()` validates `album_idx >= data.album_len` and `artist_idx >= data.artist_len` but never rejects a **negative** `album_data.name_idx`. A corrupt index with `name_idx = -1` passes validation; `get_album_name()` then computes `album_names - 1` and reads before the name pool. `artist_idx` may legitimately be -1, but `name_idx` may not. |
| HFS-104 | S2 | `empty_slide_hid` is a static int with default 0. `read_pfraw()` returns it when the empty-slide file is missing, so the first missing-empty-slide load can set `empty_slide_hid = 0` and later code may treat handle 0 as a valid bitmap. Initialize to -1 and fail the load explicitly. |
| HFS-105 | S3 | `pictureflow.c` `configfile_save()` return values are ignored when persisting config, so a failed settings write is silent. |
| HFS-106 | S3 | `pictureflow.c` `save_pfraw()` does not verify that the final file size equals the header + pixel payload; a partial file is accepted by the unchecked `read_pfraw()` path (HFS-082). |
| HFS-107 | S3 | `pictureflow.c` `incremental_albumart_cache()` removes the old `.pfraw` before attempting the new save; a failed save leaves no old and no valid new artwork. |
| HFS-108 | S2 | `fat_dir_is_parent()` compares `file->dircluster` with `dir->firstcluster`. A base directory and its overlay shadow have different IDs, so after a shadow/rename the parent relationship can be misclassified. This feeds `fileobj_sync_parent()` cache invalidation and the path-prefix check; stale directory scans or a missed “move into own subtree” check are possible. |
| HFS-109 | S2 | `fileobj_mgr.c` `binding_assign()` uses `fat_file_is_same()`; a base file and its overlay shadow compare different, so two open handles can get separate file-object bindings for the same logical file. This is the mechanism behind HFS-025 duplicate shadows and divergent writes. |
| HFS-110 | S3 | `buffering.c` `load_image()` uses `path + strlen(path) - 4` without checking `strlen(path) >= 4`. The external-image path normally has a filename extension, but a short path would read before the string. |
| HFS-111 | S3 | `fat_file_sector_size()`/`hfsplus_sector_size()` and `hfsplus_cluster_size()` are called with file/volume IDs; the adapter versions do not bounds-check `states[volume]` before indexing (same class as HFS-089). |
| HFS-112 | **S0/S1** | `pictureflow.c` `get_tcs_search_res()` does `*bufsz -= data_size` **before** checking that `*bufsz >= data_size`. On a nearly full index buffer, `bufsz` is `size_t` and underflows to a huge value; the subsequent `l > *bufsz` check passes and `rb->strcpy(*buf, tcs->result)` writes past the buffer. This is reachable during on-device index rebuild (`create_album_index`) when the allocated PictureFlow buffer is small relative to the library. |
| HFS-113 | S2 | `pictureflow.c` `create_album_untagged()` dereferences `pf_idx.album_index[-(pf_idx.album_ct - 1)]`. If the album search returned zero albums but artists exist (partial/corrupt tagcache), `album_ct == 0` and the code reads out of bounds. `create_album_index()` should reject zero albums before this helper. |
| HFS-114 | **S0/S1 (code-confirmed)** | `pictureflow.c` `write_album_entry()` stores `album_untagged_idx = name_idx`, i.e. a **name-pool byte offset**, but at the end of `create_album_index()` the code does `pf_idx.album_index += pf_idx.album_untagged_idx + 1; pf_idx.album_ct -= pf_idx.album_untagged_idx + 1;`. When an `<Untagged>` album is present at a nonzero name offset larger than the album count, the pointer jumps too far and the `uint16_t` album count underflows; the following qsort/read walks far outside the index buffer. With no untagged album the sentinel `UINT_MAX` wraps to 0 by accident. This is a strong candidate for the reported on-device database/PictureFlow instability. The Mac prebuilt index does not take this path. |
| HFS-115 | S3 | `create_album_index()` increments `uint16_t` album/artist counts without checking for 65,535 overflow. A very large library can wrap the counts and make the index inconsistent. |
| HFS-116 | **S1** | `load_album_index()` validates name offsets against `album_len`/`artist_len` but does not verify that each referenced name is NUL-terminated inside the pool. `get_album_name()`/`get_album_artist()`/`mfnv()` then scan until a NUL; a corrupt pool without a terminator reads past the index buffer. |
| HFS-117 | **S1 (high, confirmed by artifact)** | The bootloader uses the same `hfsplus_mount()` as the firmware, including `overlay_load()`. A valid-but-wrong tombstone can hide `/.rockbox` or `/.rockbox/rockbox.ipod`, leaving the device unbootable even though the native directory exists. If the overlay header is damaged, the bootloader’s overlay_load can also take the **fresh-initialization** path and overwrite the overlay before the user can back it up. The bootloader does not need overlay state to load a native firmware file. The built `bootloader-ipod6g.ipod` contains both `RPOVL11` and `/.rockpod-rw`, confirming overlay code is linked into the bootloader. |
| HFS-118 | S1 | An overlay entry/tombstone for `/.rockbox/rockbox.ipod` can shadow the native firmware image. A Mac firmware update then does not take effect, or the bootloader loads an old/invalid image. The bootloader should prefer the native boot file (or load native-only) and the update procedure should verify the actual firmware source. |
| HFS-119 | S1 | `.rockbox` can disappear from both the firmware and bootloader namespace if a tombstone matches the native `.rockbox` directory’s parent/base ID. The tombstone name is ignored (HFS-090), so CNID reuse or a torn entry can hide a perfectly valid native `.rockbox`. This is a prime candidate for the reported “could not find .rockbox” incident. Inspect overlay root entries (`parent == 2`) for `TOMBSTONE`/`DELETED` flags and the native `.rockbox` CNID before touching the container. Device snapshot 2026-11-01: no root tombstone and no `.rockbox` shadow currently; still a hypothesis for the historical event. |
| HFS-120 | S2 | `pictureflow.c` builds the temporary artist/album index at `buf + bufsz - sizeof(entry)` without aligning the **end** pointer. `buf` is aligned, but an arbitrary `bufsz` can make `struct artist_data *`/`struct album_data *` unaligned; the initial `write_artist_entry()`/`write_album_entry()` stores may fault or be emulated on ARM. The final copies are aligned, but the on-device build writes unaligned first. | Align `bufsz` down to the index-entry alignment before deriving the end pointer, or copy through byte-wise access. Add an alignment assertion/test with odd buffer sizes. |
| HFS-121 | **S1 (native-write safety)** | `hfs_ro` validates that extents are within the volume and non-overlapping with each other, but never reads the HFS+ allocation file or checks that extents are allocated nor that they avoid the volume header, catalog, extents, attributes, startup, and overlay container blocks. `write_existing_inline()` trusts those extents. A corrupted catalog can therefore direct a same-size native write into HFS+ metadata; a corrupted container fork can direct overlay writes into metadata. | Parse the allocation file and validate every writable fork against it; explicitly reserve the volume header/alternate and B-tree/container metadata ranges. Prefer requiring the overlay for all writes rather than using the native same-size path on an unverified volume. |
| HFS-122 | **S1 (container safety)** | Overlay stage 2 checks container size and inline extents but not that the container is actually fully allocated in the HFS+ allocation bitmap. A sparse/cloned/partially allocated `.rockpod-rw` can be written as if its blocks were owned. | Validate the container’s extents against the native allocation bitmap; reject unallocated/reserved-range blocks. Keep the documented `mkfile` fully-allocated requirement as a second gate. |
| HFS-123 | **S1 (confirmed)** | `hfsplus_mount()` treats every `overlay_load()` failure as “overlay optional”: it zeroes the overlay and returns success. If `.rockpod-rw` exists but has a bad version, header, bitmap or entry, the volume mounts with `overlay=0`; `hfsplus_writable()` is false, but `hfsplus_can_write_existing()` still permits same-size native writes. Reproduction: set the overlay version field to 2; mount returned `mount=0 ovl=0 stage=4 err=-2`, then `base.bin` was opened, 512 bytes were overwritten in place, and `hfsplus_closewrite()` returned 0. This silently downgrades to v10 semantics and writes native data even though the user’s overlay failed. | Distinguish “container absent” (read-only fallback, explicit warning) from “container present but invalid” (fail closed with no writes and a diagnostic). Never silently enable native writes after an overlay load failure. |
| HFS-124 | **S1 (confirmed)** | `overlay_decode_entry()` accepts any 32-bit `id`; it does not require `HFS_OVL_ID_BASE..0x7fffffff` or uniqueness against native CNIDs. `overlay_find_id()` gives overlay IDs precedence, and `fat_direntry.firstcluster` is a signed `int32_t`, so an ID with bit 31 set becomes negative. Reproduction: patched a valid `renamed.bin` entry’s ID to the native `other.bin` CNID (20) and remounted. Readdir listed both `other.bin` (native) and `renamed.bin` (overlay) with ID 20; opening the directory entry for `other.bin` returned the 5,000-byte overlay `renamed.bin` and read `AAAA...`, not `OTHER-CONTENT...`. A single corrupt ID silently redirects a native path to overlay data. | Validate the overlay ID range at load/create; reject duplicate and native-colliding IDs; make `hfsplus_open()` verify the stored parent/name identity (HFS-069) before honoring an overlay ID. |
| HFS-125 | **S2 (confirmed)** | `hfsplus_closewrite()` unconditionally calls `overlay_ensure_capacity()` for every overlay file. For a BASE shadow whose logical size is unchanged (for example after `hfsplus_rename()` of a native file, then open+close for write with no data write), the entry has `data_blocks=0` and `HFS_OVL_BASE` set; capacity allocates a new run without calling `overlay_materialize_base()`, so the blocks are never populated and the entry stays BASE. The next write calls `overlay_materialize_base()`, allocates another run, and overwrites `data_start`, leaking the first run. Host test: rename a native 5,000-byte `base.bin`; close with unchanged size dropped free space from 15,736 KiB to 15,704 KiB; the first 512-byte write dropped it again to 15,672 KiB for the same file. The leaked blocks remain marked used in the container bitmap. | Materialize (or skip allocation entirely) whenever the entry is still BASE; never allocate backing blocks for an unchanged BASE entry. Add a host test that renames, closes without writing, then writes and checks free space and bitmap ownership. |
| HFS-126 | **S1 (confirmed)** | `build_dir_index()` and `hfs_ro_iter_next()` assume catalog keys are sorted by parent, but never validate key ordering. `hfs_ro_iter_next()` stops as soon as `key_parent > it->parent`; a single corrupted parent key therefore ends the directory silently. Reproduction with the synthetic catalog: change the `.rockbox` folder record’s parent from 2 to 3; healthy `ls /` prints `.rockbox` and `Music`, patched `ls /` prints only `Music` with rc=0. The fixture used the unindexed host inspector, so the record is skipped by the parent test; on-device the fast index also consumes the non-monotonic sequence and may build an unsorted index or stop at the first greater key. In both paths the reader does not report corruption, and a normal `.rockbox` lookup becomes “not found”. This is a direct mechanism for the reported `.rockbox` disappearance. Device snapshot 2026-11-01: native fsck/catalog hierarchy is clean and the current overlay has no root anomaly; not reproduced on the device. | Validate parent/name key monotonicity while building the directory index (using the catalog `keyCompareType`); fail the mount or fall back to a full validated scan on any ordering anomaly. Never silently stop iteration. |
| HFS-127 | **S3** | `create_album_index()` duplicate removal uses `if (idxi > 0 && ...)`. `artist_idx` is the byte offset of the artist name, and the first artist legitimately has offset 0, so duplicate albums by the first artist are never marked for removal. The final `qsort` then keeps duplicate album slots. | Test the sentinel separately (`artist_idx >= 0`) or compare the full `(artist_idx, seek)` identity with an explicit no-artist sentinel. Add a host/PictureFlow test with two identical albums by the first artist. |
| HFS-128 | **S2 (tooling install safety)** | `build_rockpod_coverflow_index.py`’s `overlay_conflicts()` only reports used entries whose literal `name` is one of the installed filenames. It ignores tombstone identity (`base_id` + parent) even though `overlay_hides_base()` ignores a tombstone’s name, and it does not validate entry ranges or duplicate IDs. A tombstone for a base `pictureflow_album.idx` under an old name (or a colliding base CNID) can hide the freshly installed native file while the script reports no conflict. | Reuse the reader’s tombstone/CNID matching rules and validate the overlay namespace before an install; abort on any entry or tombstone that could hide a target. Add a fixture with a stale tombstone whose name differs from the target. |
| HFS-129 | **S1 (confirmed code path)** | `create_track_index()` calls `buf_ctx_lock()` and returns without unlocking when `slide_index == pf_tracks.cur_idx` or on any `goto fail` path. The lock is only released by `free_borrowed_tracks()`, which some callers do not call on failure; `track_list_ready()` can call `create_track_index()` for the already-current slide when `pf_state != pf_show_tracks`. The next `buf_ctx_lock()` then deadlocks the plugin. `free_borrowed_tracks()` also unlocks unconditionally, even if the lock is not held. | Make `create_track_index()` release the lock on early return/error, or acquire it only around the actual buffer mutation. Add an explicit `buf_ctx_locked` guard to `free_borrowed_tracks()`, and test repeated “open track list”/context-menu transitions and tagcache failure. |
| HFS-130 | **S2** | `reselect()` hashes `pf_idx.artist_names + artist_idx` without checking whether `artist_idx` is the valid `-1` “no artist” sentinel. `load_album_index()` deliberately permits `artist_idx == -1`, and untagged album entries can retain it. `mfnv()` then scans before the artist-name pool. `reselect()` also hashes the album name without validating `name_idx`. | Use `get_album_artist()`/`get_album_name()` accessors with explicit bounds; skip the artist hash for `artist_idx < 0`; reject negative or out-of-range `name_idx` during index load. Add a corrupt/no-artist index fixture. |
| HFS-131 | **S2** | The generic MBR fallback in `hfs_partition_scan()` breaks out of the loop on the first `add()` failure (malformed/overlapping/out-of-range partition) and then never examines later primary entries. The APM and GPT loops return on the first invalid entry for the same reason. A bad entry before a valid Apple_HFS/FAT partition can hide the whole disk. | Continue past invalid entries, remember the first error, and return a valid later partition; abort only on capacity/buffer-full or an explicit policy stop. Add fixtures with a malformed entry 0 and valid entry 1 for MBR, APM and GPT. |
| HFS-132 | **S2** | `hfs_ro_mount()` reads only the primary volume header at offset 1024 and ignores the HFS+ alternate volume header near the end of the volume. A torn primary-header sector makes the whole volume unmountable even when the alternate is intact. | Validate the primary and alternate headers; on primary failure, use a validated alternate only for a read-only recovery mount with an explicit diagnostic. Reserve both header ranges in HFS-121’s write validation. Add a fixture with a deliberately damaged primary header. |
| HFS-133 | **S2 (device-reported; root cause open)** | Theme/skin selection applies immediately but after a reboot the firmware returns to cabbiev2. The 2026-11-01 device snapshot has a minimal `config.cfg` (volume/scrollbar/brightness/QS only) and a `config.cfg.old` containing the SNARTYNEON theme settings; there is no `config.cfg.new`. Rockbox saves changed settings lazily to `config.cfg.new` through the storage-idle callback, then `rename_temp_file()` promotes it on the next boot. That path crosses `hfsplus_create_file`, overlay closewrite and rename, where HFS-002/HFS-025/HFS-091 identity/ordering bugs already exist. It may also be a reset before the idle flush or a settings reset/version change. The exact failure has not been isolated. | Add a controlled settings test: select a theme, wait for storage idle, verify `config.cfg.new` exists and contains `wps`/`sbs`/font settings, cleanly power off (not hard reset), reboot and verify the theme survives. Instrument settings save and overlay rename; make config commit write→flush→rename→readback atomic and unique. Do not mark fixed until a device reboot preserves the theme. |

## Build/test/tooling findings

| ID | Severity | Finding |
|---|---|---|
| HFS-060 | S2 | `tools/hfsplus/Makefile` `sanitize` passes `-no-pie`, which Apple Clang rejects; the reader can be sanitized with adjusted flags. |
| HFS-061 | S2 | `check_integration.py` is Linux/GCC-only and fails on macOS clang (`-ffreestanding`, Mach-O sections, duplicate endian helpers). |
| HFS-062 | S2 | No default host test exercises overlay writes; `test_native_adapter` skips without `HFS_RB_TESTS`. The temporary harness found HFS-001/003/004 immediately. |
| HFS-063 | S2 | `make zip` copies `.DS_Store` from the source tree into `.rockbox`. |
| HFS-064 | S3 | The database tool build requires a manual `sed` of the generated Makefile after every configure; the restored upstream simulator glue needs `sim_filesize`/`os_filesize` and `dir_get_info` compatibility edits. |
| HFS-065 | S3 | `build_rockpod_pictureflow_cache.py` removes old album `.pfraw` files before installing the new set and does not inspect overlay shadowing; a failure mid-copy can leave a partial cache. |
| HFS-066 | S3 | `build_rockpod_coverflow_index.py` writes the index via per-file replace/readback but does not fsync the containing directory. |

## Verified-good areas

These were tested or carefully checked and did not show a defect in the reviewed paths:

- `hfs_ro` mounts and reads real macOS-created HFS+ and HFS+J images.
- Synthetic reader/partition tests: API, malformed records, journal endianness, dirty flags, extents, unicode, partial reads.
- Basic overlay lifecycle with a 16 MiB container: init (stage 9), create directory, create file, write 5,000 bytes, read back, rename, remove, unmount.
- Overlay growth to 153,600 bytes (normal realloc path) completed without an immediate error.
- Host-generated Cover Flow index: `seek`/`artist_seek` matched `database_*` tag offsets for a two-album database; name-pool offsets resolved correctly.
- Database host tool: restored and built; smoke test produced `database_idx.tcd` (`0x54434810`, one track, `dirty=0`).
- Firmware and Thumb bootloader rebuilt after the `dir.h` compatibility edit.
- `mks5lboot --mkdfu-inst` accepted the bootloader.
- JPEG quant-table repair is targeted and does not assume tables 0/1.

## Test gaps

- No committed host adapter write harness. The temporary harness used custom stub headers and a real HFS+ image; it should be cleaned up and added to `tools/hfsplus/tests/` with a deterministic fixture.
- No fault-injection tests for `storage_write_sectors()` failures at each commit point.
- No power-loss/remount model for the overlay (bitmap/entry/data ordering).
- No test for directory rename with pre-existing overlay children/tombstones.
- No test for long HFS+ names through the adapter.
- No test for MBR 512 vs 4096 geometry selection, raw-vs-MBR ordering, or GPT fallback.
- No test for non-overlay EOF-extension rejection.
- No test for partial fast-index failure cleanup.
- No test for duplicate IDs / overlapping entry ranges.
- No test for an invalid-but-present overlay failing closed (HFS-123), native-CNID collision (HFS-124), or the BASE close/write leak (HFS-125).
- No test for catalog parent-key ordering (HFS-126), negative/no-artist reselect (HFS-130), first-artist duplicate removal (HFS-127), or track-index lock lifecycle (HFS-129).
- No test for a malformed MBR/APM/GPT entry before a valid later partition (HFS-131) or for the Cover Flow install checker’s tombstone identity (HFS-128).
- No device test in this audit.

## Corruption triage for a device report

Do not assume one cause. Capture the layer before changing anything.

| Symptom | First checks | Likely layer |
|---|---|---|
| Native music file changed/unplayable | `diskutil verifyVolume`, ATA SMART, USB/connection history, storage error logs | macOS/USB write while in Rockbox mode, storage/hardware, or a false partition geometry (HFS-006/007/008). Overlay writes normally stay inside `.rockpod-rw`; verify the container extents were not changed under a running mount. |
| `.rockbox` config, playlists or database lost/corrupt | Debug disk info overlay stage/error/slot/reason; preserve `.rockpod-rw`; read-only overlay inspection; host backup | Overlay HFS-001/002 (bitmap/entry ordering), HFS-004 (directory rename), HFS-090 (CNID/tombstone), HFS-039 (no flush on reset). |
| Files disappear/reappear after renaming a directory | Reproduce on a copy with the host harness; inspect overlay parent IDs | HFS-004. |
| Duplicate filenames or divergent copies | Inspect overlay entries for duplicate IDs/BASE shadows | HFS-025/091. |
| PictureFlow crash/garbage after a corruption incident | Check `.pfraw`/`pictureflow_album.idx` sizes; validate headers; disable cache; avoid on-device rebuild | HFS-082/083/084/087/103/112/114. |
| `View disk info` missing SSD/SMART/flush lines | Source check | HFS-081. |
| Album art cache ignored on albums without Album Artist | Compare tagcache `database_7.tcd` value with `.pfraw` filename | HFS-075. |
| Startup `-4` | `diskutil verifyVolume`, clean eject, journal state | Native HFS+ dirty/inconsistent volume, not the overlay. |
| Config write error `-83` or overlay `OFF` | Overlay stage/error/slot/reason | Overlay load failure (stage 1–10) or validation failure (stage 11), not native HFS+ corruption. |

The official Rockbox USB mass-storage corruption is a separate upstream issue. It can damage the native volume independently of this fork; do not use Rockbox USB mode as a test of overlay correctness.

## Device verification (2026-11-01)

A read-only snapshot of the connected iPod Classic 6G was taken; details are in `docs/ipod-device-investigation.md`. Key results:

- `diskutil verifyVolume` and the 2026-10-04 fsck log show a clean native HFS+ volume (catalog hierarchy OK).
- `/.rockbox` exists natively, CNID 190, visible to macOS; `/.rockbox/rockbox.ipod` exists natively (24,112, 806,540 bytes).
- The preserved `/.rockpod-rw` has 16 used entries, no `parent == 2` entries, no `.rockbox` tombstone and no `rockbox.ipod` shadow. All used ranges are bitmap-owned and non-overlapping. The only tombstone is `pictureflow.cfg` under parent 567.
- The current Rockbox `config.cfg` has no `show files` override, so the default `SHOW_SUPPORTED` filter hides dotfiles in the file browser. This is a benign explanation for “can’t find `.rockbox`” in Files/Finder, not proof of a filesystem bug.
- Raw catalog access for a direct `hfs_ro` check was blocked by macOS restricted block-device policy even with admin authorization; no raw bytes were read. The native fsck remains the authoritative current check.

The snapshot does not prove the historical cause. It does show that the current native volume and overlay are not in a state that hides `.rockbox`, and that the next discriminator is a controlled boot test, not a repair.

## Case study: the `/.rockbox` directory disappears

The bootloader and the firmware both mount HFS+ through `hfsplus_mount()`, which loads the overlay. A native `/.rockbox` directory can therefore be hidden by overlay metadata even when `diskutil` and Finder still show it.

Candidate mechanisms, in investigation order:

1. **Stale tombstone with a reused CNID (HFS-090/119).** `overlay_hides_base()` matches a tombstone by `parent + base_id` and ignores its name. If `.rockbox` receives a CNID previously used by a deleted root object that has a tombstone, the native `.rockbox` is hidden. Check volume attribute bit 12 (`kHFSCatalogNodeIDsReusedBit`) and overlay root entries with `parent == 2`.
2. **A real tombstone for `.rockbox`.** If `rmdir(".rockbox")` was ever allowed, `hfsplus_remove()` created a tombstone. `test_dir_empty_internal()` can see an empty directory if all children are hidden by overlay entries or are unsupported (HFS-004/047). Check the tombstone’s `base_id` and name.
3. **Torn/corrupt overlay entry or bitmap (HFS-001/002/011).** A tombstone/entry can acquire a wrong `base_id`, or a valid `.rockbox` entry can lose its bitmap/data. The loader currently reports `READY` for several such states.
4. **Boot file shadow (HFS-118).** `.rockbox` exists but `rockbox.ipod` is hidden or shadowed by an overlay entry; the bootloader error can look like a missing directory.
5. **Native catalog damage.** If no overlay tombstone explains it, verify native HFS+ (`diskutil verifyVolume`) and inspect the root catalog. HFSPlus Rockboxed iPod never writes the native catalog; this points to macOS/USB/hardware or storage damage.
6. **Bootloader overlay initialization (HFS-117).** If the overlay header is damaged, bootloader overlay_load may fresh-initialize the container. Preserve a byte copy before any boot if the overlay is suspect.
7. **Catalog key-order corruption (HFS-126).** A single parent key changed to a larger value makes indexed iteration stop before `.rockbox`; the directory is reported empty/partial with rc=0 rather than an error. This was reproduced in the synthetic fixture (`ls /` changed from `.rockbox + Music` to `Music` only). Check catalog key ordering on a copy before assuming the directory is gone.

Applied in V1.1: the bootloader now mounts HFS+ **without the overlay** (or retry without it when `/.rockbox/rockbox.ipod` is missing) so a bad overlay cannot prevent boot. Then diagnose the overlay read-only in Apple Disk Mode. Do not delete `.rockpod-rw` or recreate `.rockbox` as a first response.

Read-only overlay check to run on a copied `.rockpod-rw`:

```python
import struct
b = open('.rockpod-rw', 'rb').read()
version, block, count, total, bitmap = struct.unpack_from('<5I', b, 8)
entry_table = (1 + (bitmap + block - 1) // block) * block
for slot in range(count):
    e = b[entry_table + slot * 320: entry_table + (slot + 1) * 320]
    ident, parent, flags, base = struct.unpack_from('<4I', e, 0)
    if not flags & 1 or parent != 2:
        continue
    size, = struct.unpack_from('<Q', e, 16)
    data_start, data_blocks = struct.unpack_from('<2I', e, 24)
    kind, name_len = struct.unpack_from('<2H', e, 36)
    name = e[40:40 + name_len].decode('utf-8', 'replace')
    print(slot, hex(ident), hex(flags), hex(base), kind, name)
```

If a root tombstone has `flags & 8` and a `base` equal to the native `.rockbox` CNID, that is the immediate suspect. Keep the copy; do not edit it until the native CNID and volume attributes are recorded.

## Stop condition

The code is **not** bug-free by the review standard yet. The S0 findings alone (overlay bitmap/entry validation, free-before-sync ordering, long-name overflow, directory parent-ID migration) are sufficient to explain silent corruption or memory unsafety. The fault-injection run for HFS-002 and the bitmap-overlap run for HFS-001 both ended with a live file losing all its bytes while the overlay reported `READY`. Do not start autonomous installation until these are fixed and covered by host tests, and until a supervised device run passes the `shipping.md` acceptance matrix.

## Next actions (not applied)

1. Add `overlay_validate()` and make `overlay_load()` fail closed (read-only volume) on inconsistent bitmap/entry/state.
2. Reorder realloc/shrink/remove/close commits: persist the new entry state before freeing old blocks; add `storage_flush()` at commit boundaries.
3. Replace base/overlay `strcpy` with bounded copies and enforce `FAT_DIRENTRY_NAME_MAX` at open/create/rename/shadow.
4. Introduce a stable logical-parent mapping or migrate child entries when a base directory is shadowed.
5. Validate entry `size <= data_blocks*4096`; bound realloc copy by old capacity; reject duplicate IDs/ranges.
6. Fix partition scanning order and dual-unit fallback; add the four reproduction cases as tests.
7. Reject non-overlay EOF extensions before writing; report true short writes.
8. Unwind `hfsplus_mount()` on fast-index failure.
9. Add the host adapter write harness and fault injection to the default test target.
10. Re-run the full matrix and a supervised device test before claiming completion.
