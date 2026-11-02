# HFS+ adapter efficiency and power notes

Status: V3.0 Beta review, 2026-11-02. Host/build verified only; no power
measurement or hardware run has been recorded.

The dominant cost on an iPod is keeping the disk awake. CPU work matters at
boot and during large directory scans, but every avoidable read, write or ATA
flush is more expensive than a few thousand ARM instructions. This file records
what was fixed and what still needs a design or hardware decision.

---

## 1. Implemented: container writes no longer revalidate the native bitmap

### Before

Every overlay container write went through:

```text
overlay_container_write()
  -> write_existing_inline()
     -> hfs_ro_fork_writable()
        -> bitmap_range_allocated()
           -> read the native HFS+ allocation bitmap
```

`hfs_ro_fork_writable()` re-checked the container fork's extents, metadata
overlap and native allocation bitmap on every call. That meant one or more
256-byte native allocation-bitmap reads for every 320-byte entry write, every
bitmap sync and every data chunk. The container layout cannot change while it
is mounted, so this was repeated validation of an immutable fact.

### After

```text
overlay_container_write()
  -> write_container_inline()
     -> map logical offset through the validated extent list
        -> device_write()
```

The container is validated once, at overlay stage 2:

- file kind, inline extents, size and layout;
- `hfs_ro_fork_writable()` against the native allocation bitmap;
- metadata/container isolation.

After stage 2, `write_container_inline()` performs only bounds and extent
mapping. It still fails closed if the container entry is malformed or the write
is out of range. The native in-place write path
(`write_existing_inline()`) keeps full validation because it can touch catalog
forks that are not immutable.

Safety argument:

- the overlay never creates, grows or reallocates the container;
- stage 2 rejects an unallocated, sparse, metadata-overlapping or
  non-inline container before any write;
- all container writes still go to the extents captured at mount;
- the existing corruption tests still fail closed at stage 2.

This removes read amplification from the write path. It does not change commit
ordering.

V3.0 also adds:

- a persistent v3 directory/CNID index cache, so ordinary boots avoid the
  full catalog scan;
- a persisted entry high-water bound, so small overlays do not read all 2,048
  entry slots;
- removal of a redundant entry sync after realloc;
- live ATA read/write/flush counters in Debug → Disk Info.

---

## 2. V3.0 address: firmware mount's full catalog index

V3.0 stores the result of `build_dir_index()` in the overlay's reserved cache
area when the container is v3. A validated generation match avoids the full
catalog walk; a miss or stale cache rebuilds and rewrites it.

The remaining v2 path still calls `hfs_ro_enable_fast_index()` and walks the
complete catalog leaf chain. `build_dir_index()` reads every catalog node and
parses every record to build:

- the 16,384-parent directory index;
- the 4,096-entry CNID index.

The bootloader deliberately skips this. On a large library this is the largest
boot I/O cost: it reads the catalog proportional to the number of files, even
if the user only starts playback from an existing database.

Candidate fixes, in order of preference:

1. **Persist the index in the overlay**, keyed by a native volume/catalog
   generation. Invalidate on any native catalog change. This removes the scan
   on every boot but needs a trusted generation source and a cache format.
2. **Build lazily**, on the first operation that needs lookup/readdir, rather
   than unconditionally at mount. This improves boot time but not total power
   for a browsing session.
3. **Build in the background after boot.** Only viable if Rockbox can tolerate
   the extra disk activity without disturbing playback.

Do not simply disable the index: `hfs_ro_find_id()` and directory iteration
fall back to full catalog scans, which is worse for normal use.

---

## 3. V3.0 address: overlay entry-table reads

v3 stores `entry_high_water` and reads only that many slots. v1/v2 still read
all 2,048 entries in 96-entry chunks (640 KiB per mount).

A safe early-stop needs persistent knowledge of which slots can be used. Ideas:

- a used-slot bitmap (256 bytes) or high-water mark in a new overlay header;
- a new `RPOVL13` version with a checksum covering the whole header and the
  slot summary;
- v1/v2 images keep the current full scan; only new images use the summary.

Persistence rule: the summary must become durable **before** an entry above the
old bound is exposed, otherwise a power loss can make the next mount skip that
entry. Updating the summary on every high-water growth adds a header flush, so
this is a trade-off between mount reads and create-time writes, not a free win.

Do not infer the bound from `next_id` alone: a legacy or repaired image may have
a used slot above the number of allocated IDs.

---

## 4. Remaining cost: ATA flushes and commit groups

`overlay_sync_entry()`, `overlay_sync_bitmap_range()` and
`overlay_sync_header()` each end with `storage_flush()` when
`HAVE_STORAGE_FLUSH` is enabled. A single create/write/close can therefore
produce several ATA flushes.

This is not accidental. The commit order is:

```text
bitmap -> entry/extent -> data -> logical size
```

The bitmap flush before the entry write prevents an entry from referencing
blocks that are not yet marked allocated. Removing the intermediate flush
without a WAL or commit-group format can turn a power loss into an entry that
points at unallocated blocks.

Safe directions:

- a commit-group API that writes bitmap and entry, then flushes once, with a
  WAL/intent record that makes the order recoverable;
- skip entry syncs when nothing changed (requires a dirty bit or cached encoded
  entry);
- measure actual flush counts with the new debug counters before changing
  anything.

Any change here requires the power-loss matrix in
[`hardware-validation.md`](hardware-validation.md) before it can be trusted.

---

## 5. Lower-priority CPU and memory observations

- `overlay_validate()` resolves legacy duplicate names in an O(U²) loop, where
  U is the number of used entries. With 2,048 used entries that is about two
  million comparisons. A two-pass key/winner table would reduce this, but the
  current legacy rules are subtle; treat it as a separate, test-heavy change.
- `overlay_rebuild_indexes()` scans every overlay block to recompute the free
  count (up to 524,288 bits). This is cheap compared with disk I/O but could be
  folded into the bitmap read.
- `overlay_hides_base()` walks parent chains for every native entry during
  readdir. Overlay entries are normally few, so this is not urgent. A
  per-directory hidden-name cache is the obvious fix if profiles show it.
- `hfs_states` is about 1 MiB of BSS, and init/load zero large parts of it.
  This is RAM/CPU only, not disk; trimming it should wait until the format and
  mount flow are stable.

---

## 6. Measurement checklist

Before claiming a power improvement on hardware, record:

1. boot time to first UI frame, with and without a large library;
2. bytes read and number of ATA requests during mount;
3. ATA flush count for create, append, close, rename, delete and settings save;
4. write amplification for a 1 MiB overlay file;
5. idle current and disk spin-up count over a fixed playback/browse script;
6. the same script on a second adapter/card if available.

Host tests cannot prove power. They can prove that the adapter no longer
revalidates the immutable container and that the failure cases still fail
closed.
