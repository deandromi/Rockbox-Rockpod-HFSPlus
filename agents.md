# agents.md — working contract for Rockbox-Rockpod-HFSPlus

Status: source-derived, reviewed 2026-11-01.
Target: **iPod Classic / 6G (`ipod6g`, target ID 71) only**.
Public release identity: **Stable V1.0 // Version V12.0** (`docs/VERSION` is `1.0-v12.0`).
This checkout is a source snapshot without `.git` metadata. Do not assume commit history, branches, or a clean diff exists.

This file is the operating contract for AI agents and maintainers. Read it before changing code. It explains what is custom, what is inherited, what must never be broken, and where the sharp edges are.

---

## 0. Non-negotiables

1. **Native HFS+ metadata is immutable.** The firmware must never write the HFS+ catalog, allocation bitmap, extents-overflow tree, volume header, alternate volume header, or journal. All creates, grows, truncates, renames, deletes and mtime changes belong in `/.rockpod-rw`.
2. **The overlay is not a scratch file.** It contains the current settings, playlists, tagcache database and generated artwork. Never delete, truncate, zero, re-create or “repair” it without a verified backup and an explicit diagnosis.
3. **A zeroed overlay header is destructive by design.** A header whose magic is not `RPOVL11` takes the fresh-container initialization path, which zeroes the entry table and bitmap. See §4.5.
4. **Never install a bootloader with `--single`.** That destroys the Apple NOR boot path. Use the dual-boot installer documented in `shipping.md`.
5. **Never format, restore, erase or repartition a device to “fix” a software error.** `-4` at mount, a failed overlay stage, or a DFU mistake are diagnosed and recovered without wiping.
6. **Only same-size, fully-inline writes are legal without a working overlay.** Everything else must return `EROFS`/`HFS_RO_UNSUPPORTED`, not silently fail or partially write.
7. **Do not add HFS+ special cases to generic FAT code if the adapter API already owns the concept.** The canonical hook points are listed in §4.4.
8. **Do not grow `firmware/hfsplus/hfsplus_rb.c` further.** It is already 1,980 lines and mixes seven responsibilities. Decompose first; see §8.2.
9. **Do not claim hardware verification without a recorded device run.** Host tests and ARM links are not device tests. `shipping.md` has the evidence format.
10. **Ask before any destructive device operation.** Even when the user has granted general autonomy, the hard stops in `shipping.md` §18.3 still apply.

---

## 1. What this project is

Rockpod HFS+ is a fork of [Rockpod by nuxcodes](https://github.com/nuxcodes/rockpod), itself a fork of [Rockbox](https://www.rockbox.org). The project keeps the Rockpod/Rockbox application layer and adds:

- an HFS+ read-only reader (`firmware/hfsplus/hfsplus_ro.c`);
- partition-map detection (`firmware/hfsplus/hfsplus_partition.c`);
- a FAT-compatible adapter with a writable overlay (`firmware/hfsplus/hfsplus_rb.c`);
- HFS+ integration in the bootloader (`bootloader/ipod-s5l87xx.c`);
- host tooling for testing, Cover Flow index generation and artwork caching;
- JPEG decoder and album-art diagnostics fixes.

The motivating problem is real: the author experienced repeated FAT32 corruption on an iPod Classic. HFS+ keeps the Mac-native library format and moves Rockpod's mutable state into a preallocated HFS+ container. The trade-off is a custom filesystem adapter with its own limits, not a general HFS+ driver.

### 1.1 Inherited vs custom

| Area | Origin | Treat as |
|---|---|---|
| `apps/`, `lib/`, most of `firmware/` | Rockbox / Rockpod | Upstream. Change only for a specific, tested reason. |
| HFS+ reader, partition scanner, overlay adapter | This project | Custom. High risk. Read this document first. |
| `bootloader/ipod-s5l87xx.c` HFS+ diagnostics | This project | Small integration delta around a standard bootloader. |
| `utils/mks5lboot` | Upstream Rockbox/Rockpod | Standard DFU installer. Do not “improve” casually. |
| `tools/hfsplus` | This project | Host tests and image inspector. |
| `tools/rockpod` | This project | Mac-side Cover Flow index and `.pfraw` cache builders. |
| `apps/plugins/pictureflow` | Rockpod with Rockbox base | Large inherited file with Rockpod UI changes. |
| `apps/recorder/jpeg_load.c` | Rockbox with a targeted repair | The repair is the quant-table selection at the end of `jpeg_decode()`. |
| `apps/buffering.c`, `apps/playback.c`, `apps/debug_menu.c` | Rockpod/Rockbox plus diagnostics | The `buf_albumart_debug` path is custom. |
| `uisimulator/common/filesystem-sim.{c,h}` | Upstream Rockbox, restored | Host-only `DBTOOL` glue. Compatibility edits are listed in §6.1; not part of the ARM firmware. |
| Themes, fonts, WPS, backdrops | Rockpod / upstream | Assets. Do not reformat. |

### 1.2 Target facts

- `firmware/export/config/ipod6g.h` defines `HAVE_HFSPLUS_RO` for all non-simulator, non-PCTOOL builds.
- Memory is 64 MiB (`MEMORYSIZE=64`), CPU `arm`, manufacturer `s5l8702`.
- The storage stack is ATA with LBA48, 512-byte logical sectors, `MAX_VIRT_SECTOR_SIZE 4096`, `MAX_PHYS_SECTOR_SIZE 4096`, `MAX_VARIABLE_LOG_SECTOR 4096`.
- `HAVE_MULTIVOLUME` is active in the firmware (ATA + LBA48), so `NUM_VOLUMES = 4`; the bootloader drops multi-volume, so `NUM_VOLUMES = 1`.
- `firmware/export/config.h` deliberately disables `HAVE_DIRCACHE` when `HAVE_HFSPLUS_RO` is defined. Do not re-enable it without proving the HFS+ adapter’s `dirscan_info.entry` semantics are compatible.

---

## 2. Boot and mount chain

```
power-on
  -> Apple NOR boot / dual-boot selector (mks5lboot installer)
  -> Rockbox bootloader in NOR (bootloader-ipod6g.ipod, Thumb)
     -> storage_init()
     -> disk_mount_all()
        -> disk_init(drive)
           -> hfsplus_partitions()          [HAVE_HFSPLUS_RO path]
           -> hfs_partition_scan()          [fallback]
        -> disk_mount(drive)
           -> hfsplus_mount(volume, drive, start, count)
              -> hfs_ro_mount()             [read-only core]
              -> hfs_ro_enable_fast_index() [catalog cache + dir index]
              -> overlay_load()             [optional /.rockpod-rw]
           -> or fat_mount()                [only on HFSPLUS_NOT_HFS]
     -> load_firmware("/.rockbox/rockbox.ipod")
  -> Rockbox/Rockpod firmware
     -> fat_init() -> hfsplus_init()
     -> same disk_mount_all() path
     -> apps: file layer, tagcache, playback, PictureFlow
```

Important consequence: **under `HAVE_HFSPLUS_RO`, the legacy FAT partition scanner in `disk_init()` is compiled out.** `hfsplus_partitions()` is now responsible for finding HFS+, FAT and raw-volume partitions. A regression there breaks FAT32 Rockbox as well as HFS+.

### 2.1 Mount decision

`hfsplus_mount()` first probes the `H+`/`HX` signature at `base + 1024`.

- Signature absent -> returns `HFSPLUS_NOT_HFS` (`-100`); `disk_mount()` then tries `fat_mount()` on the same partition.
- Signature present -> `hfs_ro_mount()` validates the HFS+ volume. If that fails with any other error, `disk_mount()` does **not** fall back to FAT.
- `HX` (HFSX, case-sensitive) passes the probe but `hfs_ro_mount()` rejects it as unsupported. This is intentional behavior but a documentation/UX inconsistency; see §8.3.
- The overlay is optional. If `overlay_load()` fails, the volume stays mounted read-only and only `hfsplus_can_write_existing()` same-size inline writes remain possible.

### 2.2 Two mount-time indexes

1. **Catalog node cache**: one 32 KiB node (`HFS_RO_MAX_NODE_SIZE`) per volume, supplied by the adapter.
2. **Directory index**: up to `HFS_RO_DIR_INDEX_MAX` (4,096) `struct hfs_ro_dirloc` entries, mapping a parent CNID to the first catalog leaf/record. Built by `build_dir_index()` during `hfs_ro_enable_fast_index()`.

The directory index is sorted by parent because HFS+ catalog keys are `(parentID, name)`. If more than 4,096 distinct parent IDs exist, `dir_index_complete` becomes false and `hfs_ro_iter_init()` falls back to scanning from `first_leaf` for unresolved parents. That is correct but slow on large libraries.

### 2.3 Reader invariants

`hfs_ro_mount()` accepts:

- signature `0x482b` (`H+`) and version 4;
- power-of-two block size >= 512;
- volume length `blocks * block_size` within the supplied window;
- volume attribute bit 8 (`kHFSVolumeUnmountedBit`) set;
- volume attribute bit 11 (`kHFSVolumeInconsistentBit`) clear;
- if journaled (bit 13): creator `HFSJ`, a valid journal info block, a valid journal header, and `start == end` (no pending transactions).

It does **not**:

- replay the journal;
- support HFSX;
- support resource forks, symlinks, hardlinks, macOS compression, encryption or HFS wrappers;
- support extents-overflow trees. Only the 8 inline extents are usable, for the catalog and every file.

### 2.4 Catalog parsing

- B-tree header node is validated for node size, depth, root, leaf count, first/last leaf, total/free nodes, key length 516, attributes and B-tree type.
- Each leaf node is validated for kind `255`, height 1, record count, forward/back links, and a monotonic offset table.
- `node_record()` validates the whole offset table on every call. This is safe but O(records²) per node.
- Records: kind 1 = folder, 2 = file, 3/4 = thread. Threads are validated and skipped for the namespace.
- `parse_record()` rejects unsupported mode types, compressed flags and `hlnkhfs+` / `fdrpMACS` Finder metadata.
- Names are decoded from UTF-16BE to UTF-8. `/` becomes `:`, `:` is rejected, `.`/`..` are rejected, invalid surrogates are rejected. Output is capped at `HFS_RO_NAME_BYTES` (1,021 bytes).
- Lookup is **exact byte comparison**. There is no Unicode normalization and no case folding. HFS+ is normally case-insensitive and stores decomposed names; this is a known compatibility limit, not an oversight to “fix” casually.

---

## 3. The writable overlay (`/.rockpod-rw`)

### 3.1 Why it exists

HFS+ metadata is a journaled B-tree. Writing it from a small ARM firmware is risky and would make the volume less trustworthy to macOS. Instead, Rockpod preallocates one ordinary HFS+ file. The adapter merges that file’s internal namespace with the native HFS+ namespace.

macOS sees one opaque file. Finder does not show the overlay’s files. A native `config.cfg` on the Mac can differ from the logical file Rockpod uses, because an overlay entry can shadow it. This is the single most important operational fact for backups and debugging.

### 3.2 Container constraints

- Name: `/.rockpod-rw` at the volume root.
- Must be an ordinary, fully allocated HFS+ **data-fork** file.
- Minimum size: 16 MiB.
- Logical size is capped at 2 GiB (`HFS_OVL_MAX_BLOCKS = 524,288` blocks of 4,096 bytes).
- The container’s data fork must fit in the **8 inline extents**. A fragmented container is rejected with `HFS_RO_UNSUPPORTED` at overlay stage 2.
- The adapter does not create or grow the container. `mkfile 1g` on macOS is the documented way to create it; do not use sparse mode.

### 3.3 On-disk layout (format `RPOVL11`, version 1)

All integers are little-endian. Block size is 4,096 bytes.

**Header block — offset 0, 4,096 bytes**

| Offset | Size | Field |
|---:|---:|---|
| 0 | 7 | magic `RPOVL11` |
| 7 | 1 | zero |
| 8 | 4 | format version (`1`) |
| 12 | 4 | block size (`4096`) |
| 16 | 4 | max entries (`2048`) |
| 20 | 4 | `total_blocks` (container size / 4096, clamped to 524,288) |
| 24 | 4 | `bitmap_bytes` (`(total_blocks + 7) / 8`) |
| 28 | 4 | `data_start` (`1 + bitmap_blocks + entry_blocks`) |
| 32 | 4 | `next_id` (>= `0x40000000`; reconstructed on mount) |
| 36 | 4 | `generation` (incremented when the header is written) |
| 40..4095 | | zero |

**Allocation bitmap**

- Starts at block 1 (offset 4,096), length `bitmap_bytes`.
- Bit `n` set means overlay block `n` is allocated.
- Blocks `[0, data_start)` are metadata and must be set.
- The bitmap is persisted as a full write on fresh initialization and as sector-aligned ranges on allocation/free.

**Entry table**

- Starts at `(1 + bitmap_blocks) * 4096`.
- `HFS_OVL_MAX_ENTRIES = 2048` entries of `HFS_OVL_ENTRY_SIZE = 320` bytes.
- `entry_blocks = ceil(2048 * 320 / 4096) = 160`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `id` |
| 4 | 4 | `parent` (logical parent ID; native CNID or overlay ID) |
| 8 | 4 | `flags` |
| 12 | 4 | `base_id` (native CNID this entry shadows/backs, or 0) |
| 16 | 8 | `size` (logical bytes) |
| 24 | 4 | `data_start` (first overlay block) |
| 28 | 4 | `data_blocks` (capacity in blocks) |
| 32 | 4 | `mtime` (uint32) |
| 36 | 2 | `kind` (`1` directory, `2` file) |
| 38 | 2 | `name_len` (0..260) |
| 40 | 260 | name bytes, **not** NUL-terminated on disk; `name_len` bytes are meaningful |

**Flags**

| Value | Name | Meaning |
|---:|---|---|
| `0x00000001` | `HFS_OVL_USED` | Slot is occupied. |
| `0x00000002` | `HFS_OVL_DELETED` | Logical object is deleted; still occupies the slot until data is freed. |
| `0x00000004` | `HFS_OVL_BASE` | Entry refers to a native HFS+ object and has not been materialized yet. |
| `0x00000008` | `HFS_OVL_TOMBSTONE` | Hide the native object with this `base_id`/name/parent. |

**Data area**

- Blocks `[data_start, total_blocks)`.
- File data is addressed as `data_start * 4096 + file_offset`.
- Allocation/growth/shrink are described in §3.6.

**Useful constants for a 1 GiB container**

```
total_blocks  = 262144
bitmap_bytes  = 32768
bitmap_blocks = 8
entry_blocks  = 160
data_start    = 169
data capacity = (262144 - 169) * 4096 = 1,073,213,440 bytes (~1023.7 MiB)
```

### 3.4 ID model

- Native HFS+ IDs are CNIDs (>= 2). The root is 2.
- Overlay IDs start at `0x40000000` and increment.
- The adapter stores either in `fat_file.firstcluster`, so the values must never collide.
- Overlay entries can refer to a native object through `base_id`; that is the only bridge.
- `next_id` is not persisted on every creation. On mount, `overlay_rebuild_indexes()` computes `max(id)` and sets `next_id` to `max + 1` (or wraps to `0x40000000` after `0x7fffffff`). The header value is therefore a hint, not authoritative.

### 3.5 Mount and recovery stages

`overlay_load()` exposes diagnostics through `hfsplus_overlay_last_stage()`, `..._last_error()` and `..._last_slot()`.

| Stage | Meaning |
|---:|---|
| 1 | Look up `/.rockpod-rw`. |
| 2 | Validate container type, inline extents, size and computed layout. |
| 3 | Read the header block. |
| 4 | Validate magic/version/block/count/totals/bitmap/data_start/next_id. |
| 5 | Read the allocation bitmap. |
| 6 | Validate that all metadata blocks are marked used. |
| 7 | Read one entry from the table. |
| 8 | Decode/validate one entry. The slot and decoded fields are exposed. |
| 9 | Overlay is ready. |
| 10 | Fresh initialization was attempted (or failed). |

A missing overlay fails at stage 1; that is not corruption, only “no writable overlay”. A container that does not meet the constraints fails at stage 2 and leaves the volume readable.

Stage 8 diagnostics are deliberately preserved before validation rejects a slot: `id`, `parent`, `flags`, `kind`, `name_len`, `data_start`, `data_blocks`. This distinguishes a bad on-device read from a validator assumption.

### 3.6 Allocation, growth and shrink

- `overlay_alloc_run()` scans from `alloc_hint`, wraps once, finds a contiguous free run, sets bits, and persists the bitmap range **before** updating `free_blocks`.
- On bitmap write failure, bits are rolled back.
- `overlay_growth_target()` starts at 8 blocks (32 KiB); each growth adds at least 8 blocks and approximately 50% of the current size. This avoids one-block allocations for config/database writes.
- `overlay_try_extend()` extends only a contiguous run at the end.
- `overlay_realloc()` allocates a new run, copies `size` bytes, then frees the old run. The caller persists the new `data_start`/`data_blocks` entry before exposing data.
- `overlay_shrink()` keeps up to 64 blocks (256 KiB) of slack and only shrinks when `needed * 2 < old`. This is deliberate hysteresis for repeated open/grow/close cycles.
- On close/fsync, `hfsplus_closewrite()` commits the exact logical `size` and shrinks if worthwhile.

### 3.7 Namespace merge

`hfsplus_readdir()` has two phases:

1. Native catalog entries for the backing parent, skipping entries hidden by an overlay tombstone.
2. Overlay entries whose `parent` equals the logical parent.

`hfsplus_open()` first looks up the ID in the overlay ID hash. If absent, it iterates the native parent’s catalog. A `BASE` entry exposes the native data fork until the first write needs to change it.

`overlay_hides_base()` is deliberately a linear scan over all 2,048 entries using `strcasecmp`; the comment says this is a correctness-first choice for database rebuilds. The parent hash index is maintained but **never read** (see §8.1).

### 3.8 Write lifecycle

```
open existing native file
  -> first write: overlay_shadow_base()
       create overlay BASE entry with new logical name
       create tombstone for the old native name
       remember base_id and native fork in fat_file
  -> first write that changes bytes:
       overlay_materialize_base()
         allocate overlay blocks
         copy the native fork
         clear BASE
         persist new backing run
  -> grow: overlay_ensure_capacity() -> extend or realloc
  -> write data at data_start*4096 + offset
  -> update in-memory size; do not rewrite the 320-byte entry per sector
  -> close/fsync: persist exact size and shrink
```

Creates, directory creates, deletes and renames are overlay-only. A native delete writes a tombstone; native data is never freed. A native rename writes a tombstone plus a new BASE entry. A native directory rename keeps `hfs_backing_id` so children remain reachable.

### 3.9 What the overlay is **not**

- It is not journaled.
- It has no checksum or sequence number per header/entry.
- It is not atomic per entry or per file. A power loss between the individual `storage_write_sectors()` calls can leave a torn entry.
- It does not protect concurrent access across volumes; each volume has its own mutex, but the adapter is written for one active HFS+ volume in practice.
- It does not replicate HFS+ case folding or Unicode normalization.

These are the primary reliability targets for future format work. See §8.1.

---

## 4. FAT adapter contract

`firmware/export/hfsplus_rb.h` is the public API. The FAT layer calls it only under `HAVE_HFSPLUS_RO`.

### 4.1 Function map

| FAT hook | HFS+ behavior |
|---|---|
| `fat_is_readonly()` | Returns true for any mounted HFS+ volume, regardless of overlay state. The name is historical; do not use it as “writes are impossible”. |
| `hfsplus_writable()` | True only when the volume is mounted **and** the overlay is ready. Gates create/remove/rename/modtime/truncate. |
| `hfsplus_can_write_existing()` | True for a regular file with a complete inline fork, with or without overlay. Enables the legacy same-size in-place write. |
| `hfsplus_mount()` | 0 on success, `HFSPLUS_NOT_HFS` (-100) if the signature is absent, other negative HFS+ errors otherwise. |
| `hfsplus_partitions()` | Returns `partinfo` entries in 512-byte sectors and a `multiplier` for iPod virtual-sector geometry. |
| `hfsplus_create_file()` | Overlay only; returns `HFS_RO_UNSUPPORTED` if the overlay is not ready. |
| `hfsplus_remove()` | Overlay only for data; native objects get a tombstone. |
| `hfsplus_rename()` | Overlay only; native objects are shadowed plus tombstoned. |
| `hfsplus_modtime()` | Overlay only; native mtime is never changed. |
| `hfsplus_truncate()` | Shadows a native file if needed and returns `1`; the physical shrink is deferred to `hfsplus_closewrite()`. |
| `hfsplus_closewrite()` | Commits exact size, shrinks if worthwhile, returns a synthetic `fat_direntry` for the overlay. |
| `hfsplus_open_root()` | Synthesizes root CNID 2. |
| `hfsplus_open()` | Overlay ID lookup first, then native parent iteration. |
| `hfsplus_readdir()` | Native phase then overlay phase; hides `.rockpod-rw` at the root. |
| `hfsplus_readwrite()` | Reads: native fork, overlay data, or a BASE native fork. Writes: shadow/materialize/grow then data. |
| `hfsplus_seek()` | Uses the overlay size when present; returns `FAT_SEEK_EOF` beyond the last sector. |
| `hfsplus_size()` | Size in KiB; free is `min(native_free, overlay_free)` when the overlay is ready. This is conservative, not a promise that the native free space is usable for overlay growth. |

### 4.2 Read semantics

- The file layer addresses by sector: `offset = lastsector * sector_size`.
- A request that ends inside the last sector is zero-padded in the caller’s buffer to a whole sector.
- `hfsplus_readwrite()` returns the number of sectors transferred.
- Reading a directory returns `HFS_RO_IS_DIR`.
- Entries with unsupported names, resources or sizes > `FAT_MAX_FILE_SIZE` are skipped by `readdir`.

### 4.3 Write semantics and error mapping

- Without overlay: only `offset + length <= native size` and a complete inline fork are writable. Otherwise `HFS_RO_UNSUPPORTED`.
- With overlay: full create/append/truncate/rename/delete/modtime support, subject to the 2,048-entry and 2 GiB limits.
- A write that extends the file allocates enough blocks for `offset + requested`, then updates the logical size. The final partial block is preserved; bytes beyond `size` are not exposed.
- `hfsplus_closewrite()` is the commit point for logical size. `hfsplus_readwrite()` persists allocation changes before data, and size last.
- The file layer maps adapter errors to `errno` codes and negative return values. `-83` is the development-era configuration-write failure seen when create/truncate were blocked because the overlay was disabled; check overlay state before interpreting it as a filesystem format bug.

### 4.4 Integration points

| File | HFS+ change |
|---|---|
| `firmware/common/disk.c` | `disk_init()` uses `hfsplus_partitions()`; `disk_mount()` tries HFS+ then FAT on `-100`. |
| `firmware/common/fat.c` | 26 `#ifdef HAVE_HFSPLUS_RO` hooks. All generic entry points branch to the adapter before FAT parsing. |
| `firmware/common/file.c` | Write-permission gates, close/fsync commit, rename/move gates, EROFS behavior. |
| `firmware/common/file_internal.c` | Create/remove/write gates. |
| `firmware/export/fat.h` | `hfs_data`, `hfs_backing_id`, `hfs_kind`, `hfs_overlay`, `hfs_overlay_index`, `hfs_removed` in `struct fat_file`; HFS+ fields in `struct fat_dirscan_info`. |
| `firmware/export/config.h` | Disables `HAVE_DIRCACHE` when `HAVE_HFSPLUS_RO`. |
| `firmware/SOURCES` | Adds the three HFS+ sources under `HAVE_HFSPLUS_RO`. |
| `bootloader/ipod-s5l87xx.c` | Includes `hfsplus_rb.h`; prints HFS+ mount diagnostics and “HFS+ read-only test”. |

### 4.5 Fresh-container initialization

If the first 7 bytes of `.rockpod-rw` are not `RPOVL11`:

1. zero the entry table;
2. mark all metadata blocks used;
3. write the bitmap;
4. rebuild indexes;
5. write a new header.

This is how a new `mkfile` container becomes usable. It is also why “zero the header to reset the overlay” destroys settings, playlists and databases. A damaged header and a fresh container are indistinguishable by magic alone. Any future format should add a checksum and a non-destructive recovery path.

---

## 5. Limits and known unsupported cases

| Limit | Value / consequence |
|---|---|
| Target | `ipod6g` only. Not Nano, Video, or other Rockbox targets. |
| Filesystem | HFS+ version 4. No APFS, HFSX, resource forks, symlinks, hardlinks, compression, encryption, HFS wrappers. |
| Clean state | Volume must have been cleanly unmounted. Pending journal transactions are not replayed; `-4`. |
| Extents | 8 inline extents. Extents-overflow trees are unsupported, including the overlay container. |
| Overlay size | 16 MiB minimum; logical capacity capped at 2 GiB (a larger container is clamped). |
| Overlay entries | 2,048 slots. |
| Overlay block | 4,096 bytes. |
| File size | `FAT_MAX_FILE_SIZE` (4 GiB - 1) is enforced by the adapter even though HFS+ supports larger. |
| Name length | `FAT_DIRENTRY_NAME_MAX` (255 bytes) for visible entries; overlay stores up to 260 bytes. Native names >255 are skipped. |
| Name matching | Exact UTF-8 bytes; no normalization or case folding. Overlay shadow checks use `strcasecmp`. |
| Directory index | 4,096 parent IDs; beyond that, fallback scans. |
| Database workflow | Host-built database is the documented path. On-device database initialization was unstable on the tested device. |
| Multi-volume | The adapter has per-volume state, but one active HFS+ volume is the tested configuration. |
| Hardware | One iPod Classic 6G tested by the project author. Other revisions/adapters are unverified. |

---

## 6. Build and test

### 6.1 Verified in this checkout (2026-11-01)

Environment: macOS, Apple clang 21, `arm-elf-eabi-gcc` 9.5.0, GNU ld 2.38, GNU Make 4.4.1, Python 3.14.

```sh
make -C tools/hfsplus -j"$(sysctl -n hw.ncpu)" test
```

Result: `api-test` passed; 19 reader tests passed; of 4 partition tests, 3 passed and `test_native_adapter` was skipped because no adapter binary was supplied. No on-device writes are performed by these tests.

The `sanitize` target **fails on macOS clang** because it passes `-no-pie`; the reader tests do pass under ASan/UBSan when `-no-pie` is removed:

```sh
make -C tools/hfsplus clean
make -C tools/hfsplus \
  CFLAGS='-O1 -g -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer' \
  LDFLAGS='-fsanitize=address,undefined' test
```

The firmware and Thumb bootloader built successfully:

```sh
mkdir -p build-ipod6g && cd build-ipod6g
../tools/configure --target=ipod6g --type=n
/opt/homebrew/bin/gmake -j8
/opt/homebrew/bin/gmake -j8 zip

mkdir -p ../build-ipod6g-bootloader && cd ../build-ipod6g-bootloader
../tools/configure --target=ipod6g --type=b --thumb
/opt/homebrew/bin/gmake -j8
```

Local results (not release hashes; timestamps and source paths affect them):

| Artifact | Size | SHA-256 |
|---|---:|---|
| `build-ipod6g/rockbox.ipod` | 788 KiB (806,560 B) | `50397eec3efab6d48d13b48e26058ca4f517a66913db5ae50e5e0dbd0b7549a3` |
| `build-ipod6g/rockbox.zip` | 19,086,683 B (~18.2 MiB) | `5ff003906c9ca5b1f54e4a1e5488cd73f4f1bc40681cae1364317d86ff9c6099` |
| `build-ipod6g-bootloader/bootloader-ipod6g.ipod` | 98,920 B (~96.6 KiB) | `ff8bcaca6f2287eda49370bed1d3af68c25f3cc3dfd578fd9f0917624f8c0d4a` |
| `build-ipod6g-database/database.ipod6g` | 184,464 B (Mach-O arm64) | `cb1ae2c9f8ece24dc03b3105c9672bdd59df5b34f44cf671ae546ba577cf2202` |
| `utils/mks5lboot/mks5lboot` | 69,832 B (Mach-O arm64) | `25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b` |

The Mac database host tool was also restored from the official Rockbox repository and builds successfully: `uisimulator/common/filesystem-sim.{c,h}` was added, `sim_ffilesize`/`os_ffilesize` were renamed to `sim_filesize`/`os_filesize` for this older tree, and `firmware/include/dir.h` gained the `dir_get_info` `FS_PREFIX` mapping used by upstream. A smoke test on a temporary iPod root produced a valid `database_idx.tcd` (`0x54434810`, one track). See `shipping.md` §7.

The bootloader ELF reports `_movestart = 0x22020800`, `_moveend = 0x22038a60`, about 98.7 KiB of move area. The `--thumb` requirement is real: the ARM-mode build exceeded `MOVE_AREA` by 3,552 bytes on ARM GCC 9.5.0.

`mks5lboot --mkdfu-inst` accepted the bootloader and produced a 107,056-byte DFU image reporting model `Classic 6G`, platform `ipod6g`, RB name `ip6g`, RB number 71.

The HFS+ reader was also checked against **real macOS-created images**, not only synthetic fixtures:

- non-journaled 64 MiB HFS+ image: `--scan info`, `ls` and `cat` worked;
- journaled (`HFS+J`) 64 MiB image: `journaled=1` and `cat` worked.

A temporary host adapter harness (custom stub headers, real HFS+ image, no repo changes) exercised the overlay write path. Basic lifecycle worked: init stage 9, create directory, create file, write 5,000 bytes, read back, rename, remove. The same harness confirmed the S0 findings HFS-001 (bitmap/entry overlap overwrite), HFS-003 (ASan long-name overflow) and HFS-004 (directory rename loses/reveals children). It is not committed; adding a clean version to `tools/hfsplus/tests/` is a required follow-up.

### 6.2 Not verified in this checkout

- `python3 tools/hfsplus/tests/check_integration.py` fails on macOS clang. It is a Linux/GCC-oriented harness: `-ffreestanding`, Mach-O section attributes and duplicate endian helpers. It is not part of `make test`.
- The adapter write path is not exercised by `make test`. `test_native_adapter` skips unless `HFS_RB_TESTS` points at host adapter executables. A temporary harness found S0 bugs; a committed write/fault-injection harness is still missing.
- No firmware was run on hardware in this session.
- No DFU install was performed.

### 6.3 Standard commands

```sh
# reader + partition host tests
make -C tools/hfsplus test

# inspect a real or synthetic image
./tools/hfsplus/hfsplus-inspect --scan IMAGE info
./tools/hfsplus/hfsplus-inspect --scan IMAGE ls /Music
./tools/hfsplus/hfsplus-inspect --scan IMAGE cat /Music/Artist/Album/track.m4a

# host integration/syntax (Linux/GCC; currently not macOS-clean)
python3 tools/hfsplus/tests/check_integration.py

# Mac database host tool: see shipping.md §7 for configure, sed and smoke test
```

The inspector refuses device paths and only reads regular image files. This is intentional.

---

## 7. Diagnostics playbook

### 7.1 On-device screens

- **Debug menu -> View disk info** shows `HFS+ overlay: READY/OFF`, overlay stage/error/slot, and stage-8 decoded fields.
- **Debug menu -> View album art diagnostics** shows the current album-art handle, source (`EMBEDDED` or `IMAGE FILE`), loaded/current dimensions, CRC at load/draw/now, pixel offset and the first four pixels.
- A healthy overlay reports:

```text
HFS+ overlay: READY
Overlay stage: 9
Overlay error: 0
Overlay slot: -1
```

- Stage 9/error 0/slot -1 is the only “ready” combination.
- A high `Overlay error` at stage 2 usually means the container is missing, too small, fragmented or not inline.
- Stage 3–6 errors point at the header or bitmap. Stage 7–8 errors point at the entry table; use the slot and decoded fields.

### 7.2 Host inspection

- Use `hfsplus-inspect` for the native volume. It never writes.
- For the overlay, use the read-only parser in `tools/rockpod/build_rockpod_coverflow_index.py` (`overlay_conflicts`) or the equivalent snippet in `README.md`. Both assume `RPOVL11`, version 1, block 4096, count 2048. Do not “fix” a damaged overlay by writing to it from a script.
- `diskutil verifyVolume` checks the native HFS+ filesystem only. It does not validate the overlay’s internal format.
- `diskutil info` is the authority for the actual mount point, size and filesystem.

### 7.3 Bootloader messages

- `HFS+ v8 scan error: N` means `hfsplus_last_stage() == 1` (partition scan).
- `HFS+ v8 mount error: N` means stage 2 (mount).
- The `v8` text is inherited diagnostic labeling, not proof of an old bootloader.
- `-4` means the volume did not indicate a clean unmount, is inconsistent, or the journal has pending transactions. Cleanly eject and retry; do not bypass.

### 7.4 Error codes

| Code | Meaning |
|---:|---|
| `-1` | I/O failure. |
| `-2` | Invalid/inconsistent HFS+ structure. |
| `-3` | Unsupported feature (extents overflow, HFSX, symlink, etc.). |
| `-4` | Dirty volume / consistency checking / journal replay required. |
| `-5` | Exact path not found. |
| `-6` | Path component is not a directory. |
| `-7` | Cannot read a directory as a file. |
| `-8` | Invalid argument or unmounted volume. |
| `-9` | Scratch/catalog buffer too small. |
| `-100` | Not HFS+; try FAT. |

---

## 8. Structural and code-quality review (thermo-nuclear lens)

This section applies the `thermo-nuclear-code-quality-review` standard: prioritize structural simplification, deletion of accidental complexity, and file-size discipline over cosmetic nits. The review below is deliberately blunt. There is no VCS diff in this snapshot, so the audit targets the custom HFS+ implementation and its integration points, not a branch delta.

The living, evidence-backed bug log is **`docs/hfsplus-audit.md`**. It contains the confirmed S0/S1/S2 findings, reproduction details, verified-good paths and test gaps. V1.1 applies the release-blocking subset (HFS-001–HFS-133 as marked in `BUGFIXES.md`); remaining OPEN findings are still real. The tables below are the high-level summary and must stay in sync with that log.

### 8.1 Presumptive blockers

| # | Finding | Evidence | Required direction |
|---:|---|---|---|
| 1 | **`hfsplus_rb.c` is 1,980 lines and mixes seven concerns.** | I/O bounce layer, overlay format, allocator, persistence, namespace merge, FAT adapter, diagnostics. | Decompose before adding features. Target modules in §8.2. |
| 2 | **Overlay format has no checksum, sequence number or atomic commit.** | Header magic is the only freshness test; entries are 320-byte in-place overwrites; power loss can tear both. A zeroed header triggers destructive re-init. | Add a format version with header/entry checksums and a non-destructive recovery path, or at minimum a commit sequence and duplicate header. Treat as format design work, not a local patch. |
| 3 | **Overlay does not validate entries against the allocation bitmap (HFS-001).** | A live entry’s blocks can be marked free; the allocator reuses them and silently overwrites the old file. Confirmed: `data.bin` changed in all 5,000 bytes after creating `second.bin`. | Add `overlay_validate()`: every used entry range within the data area, fully marked used, non-overlapping, unique IDs, `size <= data_blocks*4096`; fail closed (read-only) on inconsistency. |
| 4 | **Overlay frees blocks before persisting the entry change (HFS-002).** | Realloc, shrink and remove free old blocks before the new entry state is synced. Power loss or an I/O error leaves a dangling entry; combined with HFS-001 this causes overlap. | Persist new entry state before freeing old blocks; add `storage_flush()` at commit boundaries; use a write-ahead intent or checksum. |
| 5 | **Base-file long names overflow `fat_direntry` (HFS-003).** | `hfsplus_closewrite()` uses unbounded `strcpy`; a 765-byte valid HFS+ name triggered an ASan stack-buffer-overflow at `hfsplus_rb.c:1203`. | Reject names > `FAT_DIRENTRY_NAME_MAX` at open/create/rename/shadow; use `strlcpy` in every `fat_direntry` fill. |
| 6 | **Base-directory rename orphans overlay children/tombstones (HFS-004).** | Shadowing a base directory gives it a new overlay ID; pre-existing overlay entries still use the base CNID. Confirmed both directions: a deleted child reappears and a newly created child disappears after rename. | Introduce a stable logical-parent mapping or migrate child entries when a base directory is shadowed; apply consistently to readdir/open/remove/rename. |
| 7 | **Partition scanner can choose the wrong geometry or hide a valid table (HFS-006/007/008).** | 4,096-first MBR interpretation without an H+ probe; raw FAT probe before MBR parsing; GPT 4,096 attempt aborts on a malformed plausible header. All three were reproduced and ignored the real HFS+ partition. | Parse MBR/APM/GPT before raw probing; probe candidates; try both GPT unit sizes; add the four reproduction cases as tests. |
| 8 | **Non-overlay EOF extension writes a prefix and reports success (HFS-009).** | A 512-byte write to a 10-byte base file returned 1 sector, overwrote 10 bytes, then `closewrite` failed. | Reject `offset + requested > native_size` before writing. |
| 9 | **`hfsplus_mount()` does not unwind on fast-index failure (HFS-010).** | `hfs_ro_mount()` sets `mounted=1`; a later `hfs_ro_enable_fast_index()` failure leaves the volume marked mounted and blocks the next mount. | Unwind every partial mount or publish `mounted` only after the index succeeds. |
| 10 | **On-device PictureFlow index rebuild overflows (HFS-112/114).** | `get_tcs_search_res()` underflows `bufsz`; `create_album_index()` treats the `<Untagged>` name offset as an album count. | Check space before subtracting; separate untagged identity from `name_idx`; keep Mac-generated indices and disable on-device rebuild until tested. |
| 11 | **The bootloader uses overlay state for `/.rockbox/rockbox.ipod` (HFS-117/118/119).** | A stale tombstone can hide `.rockbox`/`rockbox.ipod`; a damaged header can trigger fresh overlay initialization. This matches the reported “cannot find .rockbox” failure. | Mount native-only in `BOOTLOADER` builds, or retry native-only when the boot file is missing. Preserve the overlay before any initialization. |
| 12 | **PictureFlow cache/index parsers trust corrupt files (HFS-082/083/103/116).** | `.pfraw` dimensions, index lengths/name offsets and NUL termination are not validated; a damaged cache can crash or overflow. | Validate dimensions/file sizes/read counts, checked size arithmetic, nonnegative `name_idx`, NUL-terminated pools; fail to empty slide/rebuild on Mac. |
| 13 | **`overlay_hides_base()` is O(2,048) per base entry.** | Linear scan with `strcasecmp` for every native directory entry. A 1,000-entry directory does ~2 million comparisons per scan. | Either use the existing parent hash correctly (and validate it at mount) or delete the hash and accept/document the cost. Do not maintain a write-only index. |
| 14 | **Overlay load failure silently becomes v10 native-write mode (HFS-123).** | A present-but-invalid overlay mounts with `overlay=0`; same-size native writes still succeed (confirmed: bad version → `mount=0 ovl=0`, 512-byte native overwrite, close=0). | Distinguish absent from invalid; fail closed on invalid overlay, surface the stage/error, and disable native writes. |
| 15 | **Overlay entry IDs are not validated against their namespace (HFS-124).** | A corrupt ID can collide with a native CNID; overlay lookup takes precedence. Confirmed: `other.bin` opened the 5,000-byte overlay file with the same ID. | Validate `0x40000000..0x7fffffff`, enforce uniqueness, and verify parent/name identity before accepting an overlay ID. |
| 16 | **Catalog key ordering is assumed, not validated (HFS-126).** | Changing `.rockbox`’s parent key from 2 to 3 made `ls /` return only `Music` with rc=0. One bad key can silently hide `.rockbox` and every entry after it. | Validate key parent/name monotonicity in `build_dir_index()`; fail the mount or full-scan on anomaly, never silently stop. |

The Mac Cover Flow index helper was **host-verified** during this review, not assumed: a temporary iPod root with a two-album database generated rows whose `album_data.seek`/`artist_seek` values exactly matched the tag-table record offsets (`12`/`36`), while `name_idx`/`artist_idx` used the new name-pool offsets. The helper’s tuple order `(name_idx, artist_idx, year, artist_seek, seek)` is correct. Hardware track-list verification is still recommended, but there is no known `seek` bug.

### 8.2 High-value code judo

| # | Move | Why it deletes complexity |
|---:|---|---|
| 1 | Split `hfsplus_rb.c` into `hfsplus_io.c`, `hfsplus_overlay_format.c`, `hfsplus_overlay_alloc.c`, `hfsplus_overlay_store.c`, `hfsplus_namespace.c`, `hfsplus_fat.c`, `hfsplus_diag.c`. | Each unit becomes testable in isolation. The 1,980-line file currently forces every reader to hold the whole format, allocator and FAT contract in their head. |
| 2 | Introduce one `overlay_validate()` that establishes all invariants once: metadata range, entry ID uniqueness, `size <= data_blocks*4096`, `data_start+data_blocks <= total_blocks`, bitmap/entry agreement, free-count agreement, parent/name validity. | Removes scattered ad-hoc checks in read/write paths and makes corrupt-entry behavior predictable. |
| 3 | Make the overlay parser a single canonical host tool (C or Python), and have the Python scripts shell out to it. | Removes three/four copies of `RPOVL11` layout knowledge (C, `build_rockpod_coverflow_index.py`, README snippet, debug display). |
| 4 | Extract shared endian/bounds helpers used by `hfsplus_ro.c`, `hfsplus_partition.c` and the overlay. | `be16/be32/be64/le32/le64` and range checks are duplicated. One checked-arithmetic header prevents subtle divergence. |
| 5 | Replace the `fat_is_readonly()` name with a capability query (`hfsplus_write_mode()`) and centralize the error mapping. | The current name lies; call sites must remember `fat_is_readonly && !hfsplus_writable`. A capability enum makes the three modes explicit. |
| 6 | Move the hard-coded `16510 * 4096` fallback and iPod virtual-sector policy out of `hfsplus_rb.c` into the `ipod6g` target layer. | Generic adapter code should not know one device’s USB LBA convention. This is a boundary leak. |
| 7 | Replace the static per-volume `states[NUM_VOLUMES]` with one allocated active HFS+ state (or lazy allocation). | `struct hfs_overlay` is ~733 KiB; four volumes reserve ~3 MiB of BSS for an optional feature. `rockbox-info.txt` reports ~5.7 MiB RAM usage. |

### 8.3 Correctness and robustness gaps

| # | Finding | Impact | Remedy |
|---:|---|---|---|
| 1 | `overlay_decode_entry()` validates `data_start`/`data_blocks` against `HFS_OVL_MAX_BLOCKS`, not the actual `total_blocks`. | A corrupt entry can point beyond the real container; reads fail late, writes reject late. | Validate against `o->total_blocks` and the data area. |
| 2 | `overlay_data_read()` checks `data_start < overlay.data_start` but not `data_start + data_blocks <= total_blocks`. | Inconsistent with the write path; relies on container-read bounds. | Add the same upper-bound check as `overlay_data_write()`. |
| 3 | `overlay_realloc()` copy loop is `while (copied < e->size && oldblocks)`; `oldblocks` never changes. | If `e->size > old capacity`, it can read past the old run into adjacent overlay data. | Bound by `min(e->size, oldblocks * 4096)`; reject `size > capacity` at validation. |
| 4 | `overlay_shadow_base()` rollback zeroes the entry but does not remove its ID/parent index entries. | Stale index state; `overlay_find_id()` checks `USED` so it is not fatal, but it is avoidable rot. | Add a proper `overlay_destroy_entry()` used by every rollback/delete path. |
| 5 | `overlay_sync_header()` is only called on fresh init; `generation` and `next_id` in the header are stale after normal use. | Misleading format fields; makes forensic analysis harder. | Either persist them on a defined sync boundary or remove them from the format and document the reconstruction. |
| 6 | `hfs_ro_find_id()` scans the entire catalog from `first_leaf` for every lookup. | Used by shadow/remove/closewrite paths; O(catalog) each. | Add a CNID→record index at mount, or reuse the catalog cache with a bounded search. |
| 7 | `node_record()` revalidates the whole offset table on every record access. | O(records²) per B-tree node. | Validate a node once on first use and cache a parsed record table. |
| 8 | Directory index cap 4,096. | Large libraries lose the fast path; every unresolved parent scans the catalog. | Grow the index dynamically or use a hash from parent ID to first record. |
| 9 | `hfsplus_partitions()` has a hard-coded `16510 * 4096` fallback and a 4096-first MBR heuristic. | Device-specific magic in generic code; raw FAT probe can misclassify unusual GPT/APM layouts. | Target-layer policy + explicit tests for 512/4096 APM/MBR/GPT/raw. |
| 10 | HFSX passes the `HX` probe then fails mount with `-3`, so `disk_mount()` does not try FAT. | A case-sensitive HFSX volume is reported as unsupported rather than “not HFS”. | Decide the boundary explicitly: either reject `HX` as not-HFS at probe or document the unsupported result. |
| 11 | No Unicode normalization/case folding. | Duplicate case variants and composed/decomposed names can diverge from macOS and create overlay duplicates. | Document as a hard limit; if fixed, implement HFS+ comparison rules in one canonical layer, not per call site. |
| 12 | Static global diagnostics (`last_error`, `overlay_diag_*`) are not per-volume. | Confusing with multiple volumes; debug screens assume volume 0. | Move diagnostics into `struct hfs_state`. |
| 13 | `hfsplus_open()` accepts a non-inline fork; with overlay ready, `hfsplus_can_write_existing()` also returns true for it. | The open succeeds, then `hfs_ro_pread()`/`overlay_materialize_base()` fail with `HFS_RO_UNSUPPORTED`. Error appears late and confusingly. | Reject non-inline forks at open, or expose `hfs_ro_inline_fork()` and gate the write capability on it. |
| 14 | Non-overlay writes clamp `writable` to the native size but report the full sector count as transferred. | A write that extends past EOF can modify the in-range prefix, report success, and only fail later in `hfsplus_closewrite()`. | Reject `offset + requested > native size` up front with `HFS_RO_UNSUPPORTED`; do not write a prefix. |
| 15 | `overlay_hides_base()`/`overlay_tombstone()` use `strcasecmp`, while native lookup is exact-byte and does not case-fold. | Case-only duplicates can behave inconsistently between the native namespace and the overlay namespace. | Make the comparison policy explicit and consistent; document it as part of the namespace contract. |

`docs/hfsplus-audit.md` contains the full HFS-001..HFS-133 table, including partition-scanner reproductions, long-name ASan evidence, overlay bitmap/entry overlap reproduction, the directory-rename namespace reproduction, the `.rockbox` disappearance case study, and the on-device PictureFlow index overflows. Keep severity and status there; this section is intentionally the short version.

Additional confirmed integration findings:

- **PictureFlow cache/index parsers trust their files** (HFS-082, HFS-083, HFS-084, HFS-087, HFS-103, HFS-104, HFS-116). `read_pfraw()` does not validate dimensions or reads; `load_album_index()` can overflow on corrupt size fields, does not reject a negative `name_idx`, and does not verify NUL-terminated name pools; `save_pfraw()`/`save_album_index()` ignore write failures; `empty_slide_hid` defaults to 0. A damaged `.pfraw`/index can crash or corrupt memory, and a failed save is treated as success.
- **HFS+ timestamps are not exposed** (HFS-085). Read/close paths leave `fat_direntry` DOS date/time zero, so `dir_get_info()` reports a bogus mtime; overlay `mtime` is stored but never surfaced.
- **Tombstones ignore the name and HFS+ CNIDs can be reused** (HFS-090). A new base object reusing a deleted CNID is hidden by the old tombstone; `kHFSCatalogNodeIDsReusedBit` is not checked.
- **`View disk info` returns early on HFS+ builds** (HFS-081). The HFS overlay block contains `return btn;`, so SSD/power/SMART/flush/PIO lines are skipped.
- **PictureFlow cache filenames can mismatch for missing Album Artist** (HFS-075). Tagcache stores `<Untagged>`; the Mac helper falls back to `artist`, so the `.pfraw` filename does not match what the firmware looks up.
- **On-device PictureFlow index rebuilding has two serious errors** (HFS-112, HFS-114). `get_tcs_search_res()` underflows `bufsz` on a full buffer and then overflows it; `create_album_index()` treats the untagged album’s name-pool offset as an album count and shifts/truncates the album index. Both are strong candidates for the reported on-device database/PictureFlow instability. Keep using the Mac-generated index and avoid on-device rebuilds until fixed.
- **Overlay ID hash can stop indexing after churn** (HFS-088, confirmed: after 5,000 create/remove cycles a new file was listed by readdir but `hfsplus_open()` returned not-found until remount), and adapter entry points trust `fat_file.volume` without bounds checks (HFS-089).
- **The bootloader also loads the overlay** (HFS-117/118/119). A stale tombstone can hide `/.rockbox` or `/.rockbox/rockbox.ipod` even though native HFS+ has them; a damaged overlay header can trigger fresh initialization at boot. The bootloader should mount native-only; this is the leading recovery fix for the reported “cannot find .rockbox” incident.
- **Overlay failures silently enable native writes** (HFS-123). If `.rockpod-rw` exists but fails validation, `hfsplus_mount()` reports success with `overlay=0` and same-size native writes still run. A bad overlay version was reproduced writing 512 bytes into a native file and closing successfully.
- **Overlay IDs are not checked against native CNIDs** (HFS-124). A corrupt entry ID can collide with a native directory entry; `overlay_find_id()` wins and opening the native name returns overlay data. Confirmed by patching an ID to a native CNID.
- **Unchanged BASE shadows leak container blocks** (HFS-125). Closing a renamed native file for write without writing allocates a run that is never materialized; the next write allocates a second run and leaks the first. Confirmed 32 KiB lost per close, another 32 KiB on first write.
- **Catalog key ordering is not validated** (HFS-126). A single out-of-order parent key made `ls /` silently return only `Music` instead of `.rockbox` + `Music` with rc=0. This is a plausible direct cause of a “missing” `.rockbox`.
- **PictureFlow duplicate removal skips the first artist** (HFS-127). `artist_idx > 0` treats the valid name offset 0 as “no artist”, so duplicate albums by the first artist remain.
- **The Cover Flow install checker only matches literal names** (HFS-128). A tombstone with a different name but matching `base_id` can hide the installed index while the script reports no conflict.
- **PictureFlow track-index locking leaks or deadlocks** (HFS-129). `create_track_index()` returns with `buf_ctx_mutex` held on its “same slide” and error paths; `free_borrowed_tracks()` unlocks unconditionally. Repeated track-list/context-menu transitions can deadlock the plugin.
- **`reselect()` hashes through a negative artist index** (HFS-130). `artist_idx == -1` is the valid “no artist” sentinel, but `reselect()` does `mfnv(artist_names + artist_idx)`; untagged or corrupt indexes read before the artist pool. Album `name_idx` is also unchecked.
- **The generic MBR scanner aborts on the first bad entry** (HFS-131). A malformed or overlapping primary partition makes `hfs_partition_scan()` stop before later valid entries, potentially hiding the HFS+ or FAT partition entirely.
- **Only the primary volume header is read** (HFS-132). A torn primary header makes the volume unmountable even when the HFS+ alternate header is intact; use a validated alternate for a read-only recovery mount.
- **Theme/settings persistence is not reliable** (HFS-133). On the device, `config.cfg.old` contains the SNARTYNEON theme settings but current `config.cfg` is minimal; there is no `config.cfg.new`. Rockbox commits changed settings lazily via `.new` and promotes it on boot through `hfsplus_rename`; the exact loss stage is still open. A controlled theme→idle→clean-reboot test is required.

### 8.4 Build and test debt

- `tools/hfsplus/Makefile` `sanitize` is Linux-only (`-no-pie`); make it platform-aware.
- `check_integration.py` is Linux/GCC-only and fails on macOS clang. Either make it portable or clearly label it in the test target.
- `test_native_adapter` is skipped by default; there is no committed write-path host harness. Add the in-memory overlay container harness used in this audit: create/write/grow/truncate/rename/tombstone/reopen, plus registry tests for invalid-overlay fail-closed (HFS-123), ID collision (HFS-124), BASE close leak (HFS-125), catalog key order (HFS-126), first-artist duplicates (HFS-127), mismatch-name tombstones (HFS-128), track-index lock lifecycle (HFS-129), no-artist/negative-index reselect (HFS-130), malformed-first-partition handling (HFS-131), alternate-header recovery (HFS-132) and theme persistence across a clean reboot (HFS-133).
- `tools/database` now builds via restored upstream `uisimulator/common/filesystem-sim.{c,h}` plus the compatibility edits listed in §6.2. Keep those files in sync with the matching Rockbox generation; the imported `HAVE_DIRCACHE` blocks contain upstream `xxxxxx` placeholders and are not compiled for `DBTOOL`. A smaller `DBTOOL`-only path shim would still be cleaner long-term.
- `make zip` packages `.DS_Store` from the source tree (observed in the local 18 MiB zip). Add a packaging exclusion or a pre-zip cleanup gate.
- The database tool build requires a manual `sed` on the generated Makefile after every configure. That is a build-system smell; add a proper host-tool target or a documented configure option.

### 8.5 Things that are already good

- The reader has a clear, small public API (`hfsplus_ro.h`) and returns specific negative errors.
- The reader validates bounds, overlap, journal state, node chains, record offsets, signatures and extents.
- The overlay format is documented in code and uses fixed-size, little-endian records.
- Allocation persists the bitmap before exposing data, and entry updates persist the new backing run before data writes.
- The adapter gates writes through explicit capability functions rather than scattering `if (hfs)` checks everywhere.
- Host tests are deterministic and include malformed-input mutation tests.
- The JPEG repair is targeted: it selects the quantization tables identified by the frame header instead of assuming tables 0/1.

---

## 9. Change recipes

### 9.1 Adding or changing overlay behavior

1. Decide whether the on-disk format changes. If yes, the magic/version must change and a migration or explicit refusal is required. Do not silently reinterpret `RPOVL11` v1.
2. Add the operation to the adapter, not to `fat.c` or `file.c`, unless the generic layer needs a new capability hook.
3. Persist in this order: allocation bitmap -> new extent/entry -> data -> logical size. Never expose a size that points at unpersisted data.
4. Update `overlay_validate()`/the canonical parser and the host inspector together.
5. Add a host test with a synthetic container: create, write, grow, truncate, rename, delete, remount, reopen.
6. Run `make -C tools/hfsplus test` and the ASan variant. Run `check_integration.py` on Linux if available.
7. Update `agents.md` §3 and `shipping.md` §2/§8.

### 9.2 Changing the HFS+ reader

1. Add or mutate a fixture in `tools/hfsplus/tests/test_images.py`.
2. Preserve the “read-only” property: `hfs_ro` must never call a write callback.
3. Validate every new field with explicit bounds; prefer returning `HFS_RO_FORMAT` over clamping.
4. Run the reader tests plus ASan/UBSan.
5. Test a real macOS-created HFS+ and HFS+J image with `hfsplus-inspect`.
6. Do not add a second copy of endian helpers; use the shared helper.

### 9.3 Changing partition detection

1. Add cases for APM, MBR, GPT, raw HFS+, raw FAT, 512-byte and 4096-byte units.
2. Keep the 4096-first iPod heuristic only behind a target-specific policy.
3. Test `hfspartition_scan` directly and through `hfsplus_partitions`.
4. Verify that a FAT32 volume still reaches `fat_mount()` via `HFSPLUS_NOT_HFS`.
5. Build both firmware and bootloader; the bootloader uses the same code.

### 9.4 Changing the bootloader

1. Build with `--thumb`. Check `_moveend` against `MOVE_AREA`.
2. Verify the output is scrambled by `tools/scramble` (`bootloader-ipod6g.ipod`).
3. Verify `mks5lboot --mkdfu-inst` accepts it.
4. Do not test a new bootloader on a device without a full backup and a recovery plan.
5. Never use `--single`.

### 9.5 Changing album art / JPEG

1. Reproduce with a host harness outside the firmware if possible.
2. Compare pixel CRCs before/after on conventional samples.
3. Run ASan on the decoder.
4. Verify both Now Playing (external and embedded) and PictureFlow (`.pfraw`) because they are separate paths.
5. On device, use **View album art diagnostics** and record load/draw/now CRCs.

### 9.6 Changing the database/Cover Flow helpers

1. Validate the database files are native, committed, non-empty and not `database_tmp.tcd`.
2. Check overlay shadowing for every output name.
3. Stage to a temporary directory; never write partially.
4. Read back every replacement.
5. Test the generated index against a real committed database, including album track lists (the `seek` fields).
6. Preserve `emptyslide.pfraw`, the index, `.pfraw` sidecars and `._*` files.

---

## 10. Agent operating rules

### 10.1 Before changing code

- Read the relevant section of this file and the actual source. Do not infer behavior from names.
- Identify whether the code is custom or inherited.
- Check whether an adapter hook already exists.
- Check the file size. If the change pushes a file past 1,000 lines, decompose first or justify it explicitly.
- Prefer deleting a layer over polishing it. Prefer a typed capability over scattered booleans.

### 10.2 While changing code

- Keep native HFS+ metadata immutable.
- Keep the overlay’s persist-before-expose ordering.
- Use checked arithmetic for offsets, sizes and block counts.
- Return specific errors; do not silently clamp unless the API contract says so.
- Keep comments about invariants, not restatements of code.
- Do not reformat unrelated code or mass-rename symbols.
- Do not add a dependency to firmware. Host tools may use Python stdlib and FFmpeg only as documented.

### 10.3 Before claiming success

- Run the smallest relevant test, then the full `make -C tools/hfsplus test`.
- Build firmware and bootloader when the change touches shared firmware code.
- Record what was actually tested and what was not.
- Distinguish “host test passed”, “ARM link passed”, “booted on hardware”, and “played music on hardware”.
- Update `agents.md` and `shipping.md` if behavior, format, limits or commands changed.

### 10.4 Hard stops

Stop and ask the user when:

- a destructive device operation is proposed;
- the overlay layout is unknown or damaged;
- a backup cannot be verified;
- a build artifact does not match its manifest;
- the device is in an unexpected mode (DFU `WTF`, mount `-4` before install, wrong volume);
- a test failure could indicate data loss;
- the only proposed fix is “format”, “restore”, “delete the overlay” or “try another image”.

---

## 11. Repository map

| Path | Role |
|---|---|
| `firmware/hfsplus/hfsplus_ro.{c,h}` | Read-only HFS+ volume/catalog reader, directory index, pread. |
| `firmware/hfsplus/hfsplus_partition.{c,h}` | APM, MBR, GPT and raw-volume detection. |
| `firmware/hfsplus/hfsplus_rb.c` | FAT adapter, overlay format/allocator/persistence, namespace merge, diagnostics. |
| `firmware/export/hfsplus_rb.h` | Public adapter API. |
| `firmware/export/fat.h` | HFS+ fields added to `fat_file`/`fat_dirscan_info`. |
| `firmware/common/{disk,fat,file,file_internal}.c` | Integration points. |
| `firmware/export/config/ipod6g.h` | `HAVE_HFSPLUS_RO`, target feature set. |
| `firmware/export/config.h` | `HAVE_DIRCACHE` disable for HFS+. |
| `bootloader/ipod-s5l87xx.c` | Classic bootloader and HFS+ diagnostics. |
| `utils/mks5lboot/` | DFU scanner/installer and dual-boot installer. |
| `tools/hfsplus/` | Host inspector and reader/partition tests. |
| `BUGFIXES.md` | Actionable bugfix list, fix order and acceptance criteria. |
| `docs/hfsplus-audit.md` | Evidence-backed bug audit log; keep it current while fixing. |
| `docs/ipod-device-investigation.md` | Read-only snapshot of the connected iPod: native fsck, overlay parse, `.rockbox` visibility and next safe tests. |
| `docs/release-v1.1.md` | Release notes, fixed audit IDs and verification checklist for V1.1. |
| `tools/database/` | Mac/PCTOOL tagcache database builder. |
| `uisimulator/common/filesystem-sim.{c,h}` | Restored upstream host filesystem glue used by `DBTOOL`; not part of the ARM firmware. |
| `tools/rockpod/` | Mac Cover Flow index and `.pfraw` cache builders. |
| `apps/recorder/jpeg_load.c` | JPEG decoder with the quant-table repair. |
| `apps/buffering.c`, `apps/playback.c` | Album-art buffering and diagnostics. |
| `apps/debug_menu.c` | Disk/overlay and album-art diagnostic screens. |
| `apps/plugins/pictureflow/pictureflow.c` | Cover Flow UI, index and `.pfraw` reader. |
| `apps/tagcache.c` | Music database format (`0x54434810`). |
| `README.md` | User-facing installation/usage guide. |
| `shipping.md` | Build, release, install and verification runbook. |

---

## 12. Glossary

- **APM**: Apple Partition Map.
- **CNID**: Catalog node ID; HFS+’s stable object identifier. Root is 2.
- **Catalog B-tree**: The HFS+ tree of file/folder/thread records.
- **Extent**: A contiguous run of allocation blocks.
- **Inline extents**: The eight extents stored directly in a fork descriptor.
- **Extents overflow**: A separate B-tree for files with more than eight extents. Unsupported here.
- **Fork**: A file’s data stream. Only data forks are supported.
- **Overlay**: The internal namespace stored inside `/.rockpod-rw`.
- **Tombstone**: An overlay entry that hides a native object at a given parent/name.
- **BASE**: An overlay entry that still refers to a native object and has not been materialized.
- **Materialize**: Copy the native fork into overlay blocks so it can be modified.
- **Journal**: HFS+ metadata journal. Read for cleanliness, never replayed.
- **`RPOVL11`**: Internal overlay magic/format identifier, not the public release version.
