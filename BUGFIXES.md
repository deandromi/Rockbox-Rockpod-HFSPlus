# HFS+ / HFSPlus Rockboxed iPod bugfix list

Status: **V3.0: host/build verified and device boot confirmed; six documented format limits remain. Power-loss/power matrix remains tracked in issue #77.**
Companion evidence log: `docs/hfsplus-audit.md` (HFS-001..HFS-133).
Architecture/invariants: `agents.md`.
Release/install gates: `shipping.md`.

Snapshot 2026-11-02: 109 unique findings (numbers have gaps). The code-fix
tables below are kept as history; V3.0 Beta adds the adapter decomposition, v3
overlay format, persistent index cache and the fixes recorded in the V3.0
section. Do not use old status prose as a current release claim.

This file is the actionable fix list. The audit log is the evidence; this list is the order of work, the intended fix and the required test for each finding.

Status legend:

- **OPEN** — confirmed or code-confirmed; not fixed.
- **HYPOTHESIS** — plausible but needs device/overlay evidence.
- **FIXED** — code changed and covered by host/build tests; device-only checks are called out in the text.
- **DOCUMENTED** — an unavoidable format/compatibility limitation; the reader rejects unsafe cases and the limit is documented.
- **DONE** — completed and verified in this workspace.
- **VERIFIED-CORRECT** — investigated and found not to be a bug.

Severity:

- **S0** — silent data loss, overlapping allocations, memory safety, namespace corruption.
- **S1** — wrong partition/file selected, unbootable state, memory safety with a narrower trigger, data is changed before a failed commit.
- **S2** — validation/robustness gap, performance limit or misleading behavior with a clear trigger.
- **S3** — low-risk robustness, diagnostics, documentation or future-proofing.

## V1.1 fixed in this release

The following findings were fixed in the V1.1 maintenance branch and are marked **FIXED** below: HFS-001, 002, 003, 004, 005, 006, 007, 008, 009, 010, 011, 014, 021, 022, 025, 039, 047, 048, 069, 075, 082, 083, 084, 087, 090, 094, 102, 103, 104, 106, 108, 109, 112, 113, 114, 116, 117, 118, 120, 123, 124, 125, 126, 127, 129, 130, 131, 133.

Host/build verification for this release: `tools/hfsplus` reader tests (19 reader + 7 partition tests), firmware build, Thumb bootloader build, database host tool build, mks5lboot build, and the temporary adapter harness (overlay validation/commit, long-name rejection, directory-rename namespace, duplicate shadows, bitmap corruption fail-closed, ID collision, BASE close leak, EOF, and mounted fast-index unwind). The theme/settings fix (HFS-133) still requires the controlled clean-reboot test on the iPod; see `docs/ipod-device-investigation.md`.

## V3.0

V3.0 adds the required v3 overlay format, persistent index cache and remaining fixes. Existing v1/v2 overlays are backup/recovery only; they are not migrated.

- `overlay_validate()` now returns a `struct hfs_overlay_validation`
  (reason + failing slot) from `hfsplus_overlay_validate.c`; the loader applies
  it to diagnostics. The old 2,797-line `hfsplus_rb.c` is gone, replaced by
  focused I/O, format, allocation, store, loader, namespace, FAT, mount, state,
  diagnostics and time modules.
- Debug → Disk Info shows `Overlay reason: <n> <name>` and the failing entry
  fields (id, parent, flags, kind/name length, data range).
- The host adapter harness has a `diag` mode; 30 adapter tests now run,
  including v3 init/cache, v2 compatibility, timestamps and seven stage-11
  reason/slot tests.
- Overlay visibility reads now go through one `overlay_entry_visible()`
  predicate; the validator still computes the legacy shadow set.
- `write_container_inline()` trusts the stage-2 container validation instead of
  rereading the native allocation bitmap on every overlay write. This removes
  read amplification from the write path; native in-place writes still validate.
- No on-disk format change: v1 and v2 overlays remain readable, and no
  existing reason or field is renumbered.
- v3 overlay format: resumable `INIT` marker, high-water entry bound, 256 KiB
  reserved directory/CNID index cache, checksummed extended header. v1/v2 remain
  readable; existing v2 containers are not force-migrated.
- Fresh initialization scans all metadata/cache blocks; overlay timestamps use
  Unix seconds; duplicate-ID validation is independent of shadowing; v1 backup
  probing works with a corrupt primary magic.
- Host verification: 20 reader + 7 partition + 30 adapter tests, including
  ASan/UBSan on macOS. Firmware, Thumb bootloader, `rockbox.zip`, DFU and
  `mks5lboot` build.
- Efficiency audit and remaining power work:
  [`docs/efficiency.md`](docs/efficiency.md).

## V2.1.1 hotfix

V2.1.1 fixes the remaining legacy overlay compatibility case found on hardware: an old shadow entry stored a native `base_id` without the `HFS_OVL_BASE` flag. V2.0/V2.1 rejected it at overlay stage 11 (`-2`).

- Non-`BASE` entries with a native backing CNID below `0x40000000` are accepted as legacy shadows.
- Their data path remains the overlay entry; they are not treated as unmaterialized native BASE shadows.
- A backing CNID in the overlay ID range is still rejected.
- Adapter coverage is now 17 tests.

The audit status is unchanged: 103 fixed, 6 documented limits.

## V2.1 hotfix

V2.1 is a compatibility and startup hotfix on top of V2.0. It does not change the audit status: 103 findings remain fixed and six remain documented limits.

- Legacy `USED|DELETED`/duplicate live names created by older firmware no longer make the whole overlay fail at stage 11. The newest entry is kept and older duplicates are hidden in RAM; their data is not deleted.
- Duplicate BASE/tombstone entries are resolved the same way.
- `next_id` includes shadowed IDs so a new ID cannot collide with a hidden legacy entry.
- The overlay entry table is read in 96-entry chunks instead of one 320-byte request per slot, reducing mount time.
- The bootloader skips the full directory/CNID index build; it only needs the firmware path. This is the main power-on speed fix.
- Host adapter coverage is now 16 tests.

Hardware status: V2.0 was installed on the test iPod and the duplicate-name overlay failure was reproduced. V2.1 is host/build-verified; the final boot/theme acceptance still needs confirmation on the device.

## V2.0 fixed in this release

V2.0 addresses every remaining finding tracked after V1.1. The implementation adds native HFS+ allocation/metadata validation, alternate-volume-header recovery, overlay ownership/checksum/backup and per-volume diagnostics, ENOSPC mapping, a committed host adapter harness, timestamp exposure, partition-probe hardening, PictureFlow index/config durability, and the remaining adapter/app bounds checks.

Statuses in this pass: HFS-012, 013, 020, 023, 024, 026, 027, 028, 029, 030, 031, 032, 033, 034, 035, 036, 038, 040, 041, 043, 045, 046, 060, 061, 062, 063, 064, 065, 066, 067, 068, 070, 081, 085, 086, 088, 089, 091, 092, 093, 096, 097, 098, 100, 105, 107, 110, 111, 115, 119, 121, 122, 128, 132, and 133 are **FIXED**.

HFS-042/099 (logical extent reordering without a checksum), HFS-044 (Unicode normalization/case folding), HFS-049 (`/`↔`:` name mapping), HFS-095 (extended/logical MBR traversal), and HFS-101 (volume labels/create dates/permissions not exposed) are **DOCUMENTED** limits. Unsafe cases are rejected rather than accepted silently.

V2.0 host verification: 19 reader + 7 partition + 15 adapter tests; the full reader/partition/adapter suite passes under ASan/UBSan; macOS `check_integration.py` host-adapter mode passes; firmware, Thumb bootloader, database host tool, `mks5lboot`, DFU and `rockbox.zip` build. Release hashes are in `docs/release-v2.0.md`. V1.1 was installed and booted on the tested iPod; V2.0 itself was host/build-verified in this pass, and the controlled theme/clean-reboot check remains the hardware acceptance test.

## 0. Historical immediate blockers (all fixed)

| ID | Status | Why it blocks | Required fix | Required test |
|---|---|---|---|---|
| HFS-001 | FIXED | Overlay mounts READY while live entries are absent from the bitmap; allocator reuses live blocks and overwrites files. Reproduced: 5,000/5,000 bytes overwritten. | Add `overlay_validate()`: every used entry range inside the data area, fully marked used, non-overlapping, `size <= data_blocks*4096`, unique IDs, valid parents. On failure, keep the volume read-only and report the stage/slot. Never reinitialize automatically. | Host harness: corrupt bitmap/entry range, remount, verify overlay refuses; create/write must not touch the live file. |
| HFS-002 | FIXED | Realloc/shrink/remove free blocks before the new entry state is durable. Fault injection produced a dangling entry and a 5,000-byte overwrite after remount. | Persist new entry/backing run **before** freeing old blocks; persist smaller/cleared entry before freeing tail; add `storage_flush()` at commit boundaries; use a WAL/intent record or checksummed double-buffer for the format. | Fault injection at every write in realloc/shrink/remove/close: power-loss model must remount to either old or new complete state, never overlap. |
| HFS-003 | FIXED | Base-file long name causes an ASan stack-buffer-overflow in `hfsplus_closewrite()` (`strcpy` at `hfsplus_rb.c:1203`). | Reject names > `FAT_DIRENTRY_NAME_MAX` at open/create/rename/shadow; use `strlcpy` for every `fat_direntry` fill; never copy an unbounded HFS+ name. | ASan host test with 255-, 256-, 765- and 1,020-byte names through open/close/readdir. |
| HFS-004 | FIXED | Renaming a base directory orphans overlay children/tombstones: deleted children reappear, new overlay children disappear. | Introduce a stable logical-parent mapping (base CNID ↔ overlay ID) used by readdir/open/remove/rename, or migrate every child entry when a base directory is shadowed. Do not patch only readdir. | Host test: delete child, rename parent; create child, rename parent; move parent; reopen after remount. |
| HFS-112 | FIXED | PictureFlow on-device index rebuild underflows `bufsz` before `strcpy`, causing a heap/buffer overflow when the index buffer is nearly full. | Check `*bufsz >= data_size` before subtracting; validate every remaining-space calculation; return `ERROR_BUFFER_FULL` cleanly. | Host/simulation test with an undersized index buffer and many artists/albums; ASan. |
| HFS-114 | FIXED | `create_album_index()` treats the `<Untagged>` album’s name-pool byte offset as an album count; pointer shift and `uint16_t` count underflow can make qsort walk outside the buffer. | Track untagged albums by count/index separately from `name_idx`; remove them with an explicit stable representation; add bounds checks before every pointer/count adjustment. | Host/simulation test with named + `<Untagged>` albums at different name-pool offsets; verify album count/pointer and ASan. |

All S0/S1 findings are fixed. Device installs still require the normal backup, artifact-manifest and DFU gates in `shipping.md`.

## 1. Overlay crash consistency and format hardening

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-001 | S0 | FIXED | Bitmap/entry inconsistency not detected; live blocks reused. | `overlay_validate()` fail-closed; see §0. |
| HFS-121 | S1 | FIXED | Native/container extents are not checked against the HFS+ allocation bitmap or metadata ranges; corrupt catalog/fork can direct writes into HFS+ structures. | Parse allocation file; reserve header/alternate/catalog/extents/attributes/startup/container; reject unsafe writable extents. |
| HFS-122 | S1 | FIXED | `.rockpod-rw` is not required to be fully allocated in HFS+; sparse/cloned container blocks are treated as owned. | Validate container extents against the native allocation bitmap; reject unallocated/reserved blocks; keep `mkfile` gate. |
| HFS-123 | S1 | FIXED | Overlay load failure is silently ignored; mount succeeds with `overlay=0` and still permits same-size native writes. Confirmed: bad version → `mount=0 ovl=0 stage=4 err=-2`, native 512-byte overwrite, close=0. | Distinguish missing container (read-only fallback) from present-but-invalid (fail closed, no writes, explicit diagnostic). Never silently fall back to native writes. |
| HFS-124 | S1 | FIXED | Overlay entry IDs are not range-checked or checked against native CNIDs; `overlay_find_id()` gives them precedence and `firstcluster` is signed. Confirmed: ID 20 collided with `other.bin`; opening `other.bin` returned the overlay file’s 5,000 bytes. | Validate `HFS_OVL_ID_BASE..0x7fffffff`, uniqueness and native collisions at load/create; verify parent/name identity in `hfsplus_open()` (HFS-069). |
| HFS-125 | S2 | FIXED | `hfsplus_closewrite()` allocates a backing run for an unchanged BASE shadow without materializing it; the next write materializes and leaks the first run. Confirmed: 32 KiB lost on close, another 32 KiB lost on first write after renaming a native 5,000-byte file. | Skip allocation for unchanged BASE entries or materialize before allocating; add close-without-write and write-after-rename free-space/bitmap tests. |
| HFS-002 | S0 | FIXED | Free-before-sync ordering; dangling entries. | Persist entry before free; flush; WAL/checksum; see §0. |
| HFS-005 | S1 | FIXED | Entry `size` not bounded by `data_blocks*4096`; reads/realloc can exceed allocation. | Validate `size <= data_blocks*4096` and `size <= FAT_MAX_FILE_SIZE` at load/create/close; bound realloc copy by old capacity. |
| HFS-011 | S2 | FIXED | Free count/bitmap/entries can drift; duplicates/orphans accepted. | Fold all cross-checks into `overlay_validate()`; reject or mark read-only. |
| HFS-012 | S2 | FIXED | `overlay_free_run()` can free blocks not owned by the entry. | Validate entry ownership against the bitmap/range map before freeing; use a single allocation ownership map. |
| HFS-013 | S2 | FIXED | Duplicate overlay IDs and duplicate BASE shadows accepted. | Enforce unique IDs and unique `(parent, base_id, name)` for BASE entries; reject duplicates at load/create/shadow. |
| HFS-014 | S2 | FIXED | Header `next_id=0xffffffff` produces invalid ID (confirmed). | Accept only `0x40000000..0x7fffffff`; normalize/reject otherwise; persist next IDs safely. |
| HFS-020 | S2 | FIXED | `overlay_data_read()` has no upper data-area bound. | Check `data_start + data_blocks <= total_blocks` and the actual range before reading. |
| HFS-021 | S2 | FIXED | `overlay_decode_entry()` uses `HFS_OVL_MAX_BLOCKS` instead of the volume’s `total_blocks`. | Pass the actual volume to the decoder/validator. |
| HFS-022 | S2 | FIXED | `overlay_realloc()` copy loop uses constant `oldblocks`; corrupt `size` copies past the old run. | Bound copy by `min(e->size, oldblocks*HFS_OVL_BLOCK)`; reject invalid size. |
| HFS-023 | S2 | FIXED | `overlay_shadow_base()` rollback leaves ID/parent hash links; slot reuse can corrupt the parent chain; `overlay_parent_remove()` has no cycle bound. | Add `overlay_destroy_entry()` used by all rollback/delete paths; add a traversal bound; preferably delete the unused parent hash entirely. |
| HFS-026 | S2 | FIXED | `strcasecmp` overlay matching vs exact native lookup; Unicode case folding absent. | Define one namespace comparison policy; use Unicode-aware HFS+ rules if implementing case-insensitive matching. |
| HFS-030 | S2 | FIXED | `overlay_hides_base()` O(2,048) per base entry; parent hash maintained but never read. | Use a validated parent index (with `overlay_validate()`), or delete the dead parent hash and document the linear scan. |
| HFS-034 | S2 | FIXED | Per-volume static HFS state ~733 KiB + caches; four volumes reserve roughly 3 MiB BSS. | Allocate the overlay/state lazily for the mounted volume, or reduce `NUM_VOLUMES` for HFS. |
| HFS-035 | S2 | FIXED | Global `last_error`/`overlay_diag_*` state is not per-volume. | Move diagnostics into `struct hfs_state`; debug screen uses the current volume. |
| HFS-038 | S2 | FIXED | Header `generation`/`next_id` are stale after normal use. | Either write a well-defined sync point or remove the fields and document reconstruction. |
| HFS-039 | S2 | FIXED | No `storage_flush()` at overlay commit points; ATA cache can reorder/lose bitmap→entry→data. | Flush after durable metadata commit; add an explicit sync contract to close/fsync; test power-loss ordering. |
| HFS-040 | S2 | FIXED | No checksum/sequence/backup header; magic mismatch triggers destructive fresh init. | Add a checksummed format version with a backup header and non-destructive recovery; never auto-zero on magic mismatch. |
| HFS-041 | S2 | FIXED | Parent-child relationships/orphans are not validated at load. | Validate parents in `overlay_validate()`; reject orphan/cycle/entry-under-file states. |
| HFS-067 | S2 | FIXED | Overlay-full returns `HFS_RO_BUFFER` (-9); file layer maps only `FAT_RC_ENOSPC` (-10) to ENOSPC, so users see EIO. Confirmed at 2,048 entries. The same mapping gap affects `hfsplus_remove()` tombstones, `hfsplus_rename()` shadows and `hfsplus_closewrite()` capacity growth. | Map `HFS_RO_BUFFER` to `FAT_RC_ENOSPC`/ENOSPC in create/write/close/remove/rename; add distinct “entry table full” diagnostics. |
| HFS-088 | S1 | FIXED | ID hash tombstones fill; after 5,000 create/remove cycles a listed file was not openable (`-5`). | Rehash/rebuild the ID hash when tombstones exceed a threshold; insert into tombstones even when no `NONE` is found; add stress test. |
| HFS-090 | S1 | FIXED | Tombstones match by `parent + base_id` only; names ignored; HFS+ CNID reuse is not checked (volume attribute bit 12). A new file/.rockbox can be hidden by an old tombstone. | Match tombstone by base_id **and** name/generation; check `kHFSCatalogNodeIDsReusedBit`; treat CNID reuse as unsafe for tombstones; add recovery rules. |
| HFS-091 | S2 | FIXED | Multiple shadows/tombstones for one base object possible. | Enforce uniqueness in `overlay_shadow_base`/`overlay_tombstone`; reuse an existing BASE entry. |
| HFS-092 | S3 | FIXED | `.rockpod-rw` can be created as an overlay entry in root. | Reserve the name at create/rename; never expose an overlay entry with the container name. |
| HFS-099 | S2 | DOCUMENTED | Extents are used in array order without validating logical order; swapped extents produced wrong data with success. HFS+ has no data checksum. | At minimum reject non-monotonic extent arrays as a heuristic; document that native extent corruption cannot be recovered without backup/fsck. |
| HFS-100 | S3 | FIXED | Journal info `flags != 1` is reported unsupported. | Accept/define supported internal-journal flag combinations; reject pending/external states explicitly. |

## 2. Namespace identity and file metadata

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-003 | S0 | FIXED | Long-name `strcpy` overflow. | Bounded copy + name limit; see §0. |
| HFS-004 | S0 | FIXED | Base-directory rename orphans children/tombstones. | Stable logical-parent mapping/migration; see §0. |
| HFS-024 | S2 | FIXED | Non-inline forks are accepted at open; failure appears later. | Reject/mark unsupported at open or expose `hfs_ro_inline_fork()` and gate read/write capabilities. |
| HFS-025 | S1 | FIXED | Base vs overlay identity mismatch; two handles create duplicate shadows with divergent data. | Make `fat_file_is_same`/`fat_dir_is_parent` compare a stable logical identity (base id + overlay id mapping); reuse existing shadow in `overlay_shadow_base`. |
| HFS-036 | S2 | FIXED | `hfsplus_closewrite()` ignores `hfs_ro_find_id()` failure when filling a base `fat_direntry`. | Propagate the error; never return success with an empty fatent. |
| HFS-037 | S2 | FIXED | Writes/rename/modtime can operate on `USED|DELETED` entries. | Central `overlay_file_entry()` policy: read/write behavior for deleted-but-open files must be explicit; reject metadata operations on deleted entries. |
| HFS-047 | S3 | FIXED | Base names >255 bytes are hidden by readdir but accepted by `hfsplus_open`. | Reject at open or define a supported long-name path; never pass them to `fat_direntry` copies. |
| HFS-048 | S3 | FIXED | Overlay names up to 260 bytes exceed `FAT_DIRENTRY_NAME_MAX` (255). | Enforce 255 at create/rename/shadow or extend the FAT contract consistently. |
| HFS-069 | S3 | FIXED | `hfsplus_open()` does not verify an overlay entry’s stored parent against the argument. | Check `oe->parent == parent->firstcluster` (or mapped parent); return not-found/format. |
| HFS-070 | S3 | FIXED | Base `fatent` fill ignores lookup failure. | Same as HFS-036. |
| HFS-085 | S2 | FIXED | HFS+ timestamps are not exposed; `dir_get_info()` shows a zero/bogus mtime. | Parse HFS+ create/content-modify dates, convert to DOS date/time; store overlay mtime consistently; make base-file `modtime()` create a shadow or return an explicit unsupported error; test filetree/properties. |
| HFS-108 | S2 | FIXED | `fat_dir_is_parent()` uses raw firstcluster; base/overlay IDs diverge after shadow/rename. | Use the same stable logical identity as HFS-025; fix cache invalidation and prefix checks. |
| HFS-109 | S2 | FIXED | `fileobj_mgr` can create separate bindings for base and overlay views of one file. | Stable identity comparison; reuse existing binding/shadow; add a two-handle test. |
| HFS-119 | S1 | FIXED | `.rockbox` can disappear when a tombstone matches its `parent + base_id` (CNID reuse/torn entry). | Inspect overlay root entries; fix HFS-090 and the boot path (HFS-117); add native-CNID/tombstone validation. |
| HFS-126 | S1 | FIXED | Catalog key ordering is not validated; indexed iteration stops at the first `key_parent > parent`. Confirmed: changing `.rockbox`’s parent key from 2 to 3 made `ls /` return only `Music` with rc=0. | Validate parent/name monotonicity while building the directory index; fail the mount or fall back to a full validated scan on any anomaly. Never silently truncate a directory. |

## 3. Bootloader and update safety

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-117 | S1 | FIXED | Bootloader uses overlay state to find `/.rockbox/rockbox.ipod`; a bad tombstone can hide `.rockbox`; a damaged header can trigger destructive fresh init. | In `BOOTLOADER` builds, mount native HFS+ without loading/initializing the overlay. If overlay support is ever needed for boot, add a native-only retry when the boot file is missing. |
| HFS-118 | S1 | FIXED | Overlay can shadow the boot firmware `rockbox.ipod`; a Mac update may not take effect. | Bootloader prefers the native boot file; firmware updates verify the actual source; document the overlay shadow rule. |
| HFS-119 | S1 | FIXED | `.rockbox` hidden by a stale tombstone/CNID reuse. | Fix HFS-090/117; preserve and inspect `.rockpod-rw` before repair. |
| HFS-093 | S3 | FIXED | Bootloader prints an HFS+ error for a plain FAT disk failure; “read-only test” label is stale. | Print HFS+ diagnostics only when an HFS+ candidate was probed; update the label. |
| HFS-098 | S3 | FIXED | `hfsplus_mount()` leaves stale last-error/stage/diagnostic values on success. | Reset diagnostics at mount start; report per-volume success. |

## 4. Partition/disk selection

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-006 | S1 | FIXED | 4,096-byte MBR heuristic can select the wrong geometry and never try the real 512-byte partition. Confirmed. | Probe the 4,096-unit candidate for H+/HX before accepting; fall through to the storage-sector interpretation; test real-512 + false-4096. |
| HFS-007 | S1 | FIXED | Raw FAT-like boot sector is probed before MBR; valid MBR partition table ignored. Confirmed. | Parse MBR/APM/GPT first; raw-probe only when no usable partition table exists. |
| HFS-008 | S1 | FIXED | GPT 4,096-first attempt returns FORMAT/0 on a plausible false header and never tries 512. Confirmed. | Try both units; accept only a valid GPT with the expected HFS partition; add false-EFI-PART test. |
| HFS-031 | S2 | FIXED | Hard-coded `16510*4096` fallback in generic adapter code. | Move to `ipod6g` target policy; document and test. |
| HFS-032 | S2 | FIXED | HFSX passes `HX` probe, mount returns `-3`, no FAT fallback. | Decide the boundary: reject HFSX as not-HFS or report unsupported consistently. |
| HFS-033 | S2 | FIXED | HFS+ build compiles out the legacy FAT partition scanner; FAT depends entirely on `hfsplus_partitions()`. | Keep a shared/canonical partition scanner; test FAT32 fallback through the HFS path. |
| HFS-068 | S2 | FIXED | Direct MBR path clamps an overlong partition instead of rejecting it. | Reject geometry that extends past the disk; do not mount a truncated volume. |
| HFS-095 | S3 | DOCUMENTED | Extended/logical MBR partitions are skipped. | Document or implement logical partition traversal. |
| HFS-131 | S2 | FIXED | Generic MBR/APM/GPT scans abort on the first malformed/overlapping entry and never try later valid partitions. | Continue past invalid entries, remember the error, and accept a valid later partition; abort only on buffer-full/policy. |
| HFS-096 | S3 | FIXED | One sector multiplier for all partitions; mixed unit sizes cannot be represented. | Detect conflicting units and reject/choose per target policy. |
| HFS-097 | S3 | FIXED | A false H+/HX signature suppresses FAT fallback. | Only treat a validated HFS+ volume as HFS; otherwise allow FAT. |

## 5. Mount/unmount and adapter boundary

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-009 | S1 | FIXED | Non-overlay EOF extension writes a prefix, reports success, then close fails. Confirmed. | Reject `offset + requested > native_size` before writing; return a true short/unsupported error. |
| HFS-010 | S1 | FIXED | Fast-index failure leaves `volume.mounted=1`; next mount returns ARGUMENT. Confirmed. | Unwind `hfs_ro_unmount()`/overlay state on every failure; publish mounted only after index success. |
| HFS-132 | S2 | FIXED | Only the primary HFS+ volume header is read; the alternate header near the end of the volume is ignored. A torn primary sector makes the volume unmountable despite an intact alternate. | Validate both headers; allow a validated alternate only for a read-only recovery mount with an explicit diagnostic; reserve both ranges in HFS-121. |
| HFS-027 | S2 | FIXED | Directory index capped at 4,096 parents; unresolved lookups scan the catalog. | Grow dynamically/hash map; test >4,096 directories. |
| HFS-028 | S2 | FIXED | `hfs_ro_find_id()` scans the entire catalog; used by shadow/remove/close. | Add CNID index or bounded search; measure. |
| HFS-029 | S2 | FIXED | `node_record()` revalidates the offset table per record (O(n²)). | Validate a node once and cache its record offsets. |
| HFS-042 | S2 | DOCUMENTED | Extent logical order not validated; silent wrong data. | Reject non-monotonic arrays as a heuristic; document checksum limitation. |
| HFS-043 | S3 | FIXED | Journal flag combinations other than 1 rejected. | Define supported states; keep pending/external rejection. |
| HFS-044 | S3 | DOCUMENTED | No Unicode normalization/case folding; exact-byte reader. | Document; implement HFS+ comparison rules centrally if required. |
| HFS-045 | S3 | FIXED | `hfsplus_size()` returns `min(native, overlay)` free; underreports overlay space. | Report both native and overlay free separately; UI. |
| HFS-046 | S3 | FIXED | README claims parent-directory RAM indexes are used; parent hash is dead. | Fix docs or implement/delete the index. |
| HFS-049 | S3 | DOCUMENTED | `/`↔`:` name mapping and `:` rejection are compatibility limits. | Document; test POSIX/HFS name edge cases. |
| HFS-089 | S2 | FIXED | Adapter trusts `fat_file.volume` without bounds checks. | Validate volume in every public entry point; return ARGUMENT. |
| HFS-101 | S3 | DOCUMENTED | HFS+ volume labels/create dates/permissions not exposed. | Add metadata fields only if UI needs them; otherwise document. |
| HFS-102 | S2 | FIXED | PictureFlow album-index accesses lack a common bounds check. | Add `album_index`/`album_ct` accessors; clamp center/slide indices. |
| HFS-111 | S3 | FIXED | Sector/cluster-size helpers index states without volume bounds. | Same as HFS-089. |

## 6. PictureFlow, database, album art and settings

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-075 | S2 | FIXED | Artwork cache falls back to track artist when Album Artist is absent; tagcache/PictureFlow use `<Untagged>`. | Match the tagcache value exactly (`<Untagged>`), not a display fallback; test missing/empty Album Artist. |
| HFS-082 | S1 | FIXED | `read_pfraw()` trusts header dimensions, ignores header/data read counts; corrupt `.pfraw` can cause small allocation + oversized read. | Validate width/height bounds, exact file size, read results; reject and fall back to empty slide. |
| HFS-083 | S1 | FIXED | `load_album_index()` can overflow `name_sz`, read huge lengths, and overflow after alignment. | Use checked size arithmetic; validate each pool against file size and remaining buffer before reading; use `size_t`, not `int`. |
| HFS-084 | S1 | FIXED | `save_pfraw()` ignores write/close failures and returns true; partial file treated as valid. | Check every write, final size and close; remove/reject partial files; test disk-full injection. |
| HFS-087 | S2 | FIXED | `save_album_index()` ignores write returns; partial index treated as saved. | Check writes and total size; write temp + fsync + rename + readback. |
| HFS-103 | S1 | FIXED | `load_album_index()` accepts negative `name_idx`. | Require `name_idx >= 0` and `< album_len`; artist_idx may be `-1`. |
| HFS-104 | S2 | FIXED | `empty_slide_hid` default 0; missing empty slide can look like valid handle 0. | Initialize to `-1`; fail load explicitly; never return a sentinel handle. |
| HFS-105 | S3 | FIXED | `configfile_save()` return ignored. | Report/splash on failure; retry policy. |
| HFS-106 | S3 | FIXED | `save_pfraw()` does not verify final file size. | Add size check; see HFS-084. |
| HFS-107 | S3 | FIXED | `incremental_albumart_cache()` removes old `.pfraw` before saving new. | Save temp then replace; keep old on failure. |
| HFS-112 | S0 | FIXED | `get_tcs_search_res()` `bufsz` underflow + overflow. | Check before subtract; see §0. |
| HFS-113 | S2 | FIXED | `create_album_untagged()` dereferences album index with `album_ct==0`. | Reject zero albums before helper; add bounds checks. |
| HFS-114 | S0 | FIXED | `<Untagged>` name offset used as album count; pointer/count underflow. | Separate untagged identity from `name_idx`; see §0. |
| HFS-115 | S3 | FIXED | `uint16_t` album/artist counts wrap at 65,535. | Check before increment; return buffer/limit error. |
| HFS-116 | S1 | FIXED | Name pools are not validated for NUL termination. | Validate every referenced name terminates inside the pool; reject otherwise. |
| HFS-120 | S2 | FIXED | On-device index builder derives `struct artist_data *`/`struct album_data *` from an unaligned end pointer; initial stores can be unaligned on ARM. | Align `bufsz` down before deriving the end pointer; add odd-size/alignment tests. |
| HFS-127 | S3 | FIXED | PictureFlow duplicate removal uses `artist_idx > 0`; the first artist’s name offset is legitimately 0, so duplicate albums by that artist are not removed. | Treat `artist_idx == 0` as valid; use `>= 0` or a separate no-artist sentinel, and compare full `(artist_idx, seek)` identity. |
| HFS-133 | S2 | FIXED | Theme/skin changes apply but revert to cabbiev2 after reboot. Device snapshot: minimal `config.cfg`, themed `config.cfg.old`, no `config.cfg.new`. Rockbox saves lazily to `.new` and promotes on boot through the overlay rename path; exact failure is not isolated. | Controlled test: theme → wait for storage idle → verify `.new` → clean power-off → reboot. Instrument/repair config commit (write/flush/rename/readback) and overlay rename uniqueness. |
| HFS-129 | S1 | FIXED | `create_track_index()` leaks `buf_ctx_lock` on early return (`same slide`) and on all `goto fail` paths; callers do not always call `free_borrowed_tracks()`. `free_borrowed_tracks()` also unlocks unconditionally. | Release the lock on every exit, or narrow the critical section; guard `free_borrowed_tracks()` with `buf_ctx_locked`; test repeated track-list/context-menu transitions and tagcache failure. |
| HFS-130 | S2 | FIXED | `reselect()` hashes `artist_names + artist_idx` without handling the valid `-1` no-artist sentinel; untagged/corrupt indexes can read before the pool. Album `name_idx` is also unchecked. | Use bounded accessors, skip artist hashing for negative indexes, and reject negative/out-of-range `name_idx` at load. |
| HFS-086 | S1 | FIXED | On-device database/PictureFlow instability is plausibly caused by HFS-001/002 plus HFS-112/114. | Keep host database/index workflow; test on-device only after S0 fixes and a backup. |

## 7. Debug/diagnostics

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-081 | S2 | FIXED | `dbg_disk_info()` returns inside the HFS block; SSD/SMART/flush/PIO lines are skipped on HFS+ builds. | Move `return btn;` after the remaining lines; add a regression screenshot/manual test. |
| HFS-085 | S2 | FIXED | HFS+ mtime not exposed (see §2). | Parse/convert dates; test filetree/properties. |
| HFS-093 | S3 | FIXED | Misleading HFS+ error on FAT failures; stale “read-only test” label. | Conditional diagnostics; update label. |
| HFS-094 | S3 | FIXED | Catalog `keyCompareType` not validated. | Require the expected HFS+/HFSX value; reject others. |
| HFS-098 | S3 | FIXED | Stale mount diagnostics on success. | Reset diagnostics per mount. |
| HFS-099 | S2 | DOCUMENTED | Silent wrong data from non-monotonic extents. | Heuristic rejection; document no data checksum. |
| HFS-110 | S3 | FIXED | `buffering.c` short path `strlen(path)-4` underflow. | Check length before suffix test. |

## 8. Build, test and tooling

| ID | Sev | Status | Finding | Fix |
|---|---:|---|---|---|
| HFS-060 | S2 | FIXED | `tools/hfsplus` `sanitize` passes `-no-pie`; Apple Clang rejects it. | Platform-aware flags; add a macOS `test-sanitize` target. |
| HFS-061 | S2 | FIXED | `check_integration.py` fails on macOS clang (freestanding/Mach-O/endian duplicates). | Make host harness portable; add clear Linux/macOS modes. |
| HFS-062 | S2 | FIXED | No committed host overlay write/fault-injection harness; `test_native_adapter` skips. | Add the harness used in this audit, with deterministic HFS+ images and failure injection. |
| HFS-063 | S2 | FIXED | `make zip` packages `.DS_Store`. | Exclude macOS metadata or add a pre-zip cleanup gate. |
| HFS-064 | S3 | FIXED | Database tool needs manual `sed` after configure; restored simulator glue needs compatibility edits. | Add a proper host-tool build target or a scripted macOS build; keep `sim_filesize`/`os_filesize` and `dir_get_info` notes. |
| HFS-065 | S3 | FIXED | Artwork helper removes old `.pfraw` before install and does not check overlay shadowing. | Temp/replace, backup, reuse the overlay-conflict parser. |
| HFS-066 | S3 | FIXED | Cover Flow index install does not fsync the directory. | fsync parent directory after replace. |
| HFS-128 | S2 | FIXED | Cover Flow install conflict checker compares literal entry names only; it misses tombstones that hide a target via `base_id`/parent (HFS-090) and does not validate entry ranges/IDs. | Reuse the reader’s tombstone/CNID matching and overlay validation before install; abort on any possible hiding entry/tombstone. |

## 9. Completed / verified in this workspace

| Item | Status | Notes |
|---|---|---|
| Database host tool restore | **DONE** | Added `uisimulator/common/filesystem-sim.{c,h}` from official Rockbox; adapted `sim_filesize`/`os_filesize`; added `dir_get_info` mapping in `firmware/include/dir.h`; built `database.ipod6g` (Mach-O arm64, 184,464 B) and smoke-tested (magic `0x54434810`, one track). |
| Cover Flow index `seek` investigation | **VERIFIED-CORRECT** | Host test with two albums: `seek`/`artist_seek` matched tag-table offsets (12/36); name pools resolved correctly. The earlier suspected bug was false. |
| Firmware build | **DONE** | `build-ipod6g/rockbox.ipod` 806,560 B; `rockbox.zip` 19,086,683 B; host tests pass. |
| Bootloader build | **DONE** | Thumb build, 98,920 B; `_moveend` within `MOVE_AREA`. |
| `mks5lboot` | **DONE** | Built; `--mkdfu-inst` accepts the bootloader; 107,056-byte DFU image. |
| HFS+ reader real-image checks | **DONE** | HFS+ / HFS+J / 512-block / 32K-node images read correctly. |
| Host adapter lifecycle | **DONE (test harness)** | Basic create/write/read/rename/remove works; faults were then injected to confirm HFS-001/002/003/004/025/088/114. |

## 10. `.rockbox` disappearing — investigation plan

**Device snapshot 2026-11-01:** the connected iPod's native HFS+ volume passed `diskutil verifyVolume`; `/.rockbox` exists (CNID 190) and the preserved overlay has no root entry, `.rockbox` tombstone or `rockbox.ipod` shadow. All used overlay ranges are bitmap-owned and non-overlapping. `.rockbox` is, however, hidden by Rockbox's default `Show Files: Supported` dotfile filter (and by Finder's dotfile hiding). Full evidence: `docs/ipod-device-investigation.md`. A controlled boot test is still needed to distinguish a historical transient from the benign file-browser filter.

The bootloader and firmware both use `hfsplus_mount()` and therefore the overlay. A native `/.rockbox` can be hidden by overlay metadata.

0. **Rule out the display filter.** In Rockbox use **Settings → General Settings → File View → Show Files → All**; in Finder use `Cmd+Shift+.`. The default `Show Files: Supported` hides every dotfile, including `.rockbox`. The 2026-11-01 device snapshot has no root tombstone, so this benign filter is currently the most likely explanation for a file-browser report.
1. **Preserve first.** Copy `.rockpod-rw` (read-only) and `.rockbox` while in Apple Disk Mode. Do not delete/recreate either.
2. **Record native identity.** Use `hfs_ro_lookup`/`hfsplus-inspect` to obtain `.rockbox`’s native CNID; check volume attribute bit 12 (`kHFSCatalogNodeIDsReusedBit`).
3. **Inspect overlay root.** Parse entries with `parent == 2`; list `flags`, `base_id`, `name`, `kind`. A `TOMBSTONE` (`flags & 8`) with `base_id == .rockbox CNID` is the direct suspect (HFS-090/119).
4. **Check boot file.** Look for overlay entries/tombstones named `rockbox.ipod` (HFS-118).
5. **Check native.** `diskutil verifyVolume`; if no overlay explanation, inspect the native catalog/root; HFSPlus Rockboxed iPod never writes native metadata. Validate leaf-key parent/name ordering (HFS-126): a single out-of-order key can hide `.rockbox` while every individual record still parses.
6. **Recovery.** The low-risk fix is to make the bootloader mount native-only (HFS-117), then diagnose/repair the overlay read-only. Do not format or zero the header. If the catalog itself is inconsistent, use a Mac-side backup/repair path before booting HFSPlus Rockboxed iPod.

## 11. Recommended fix order

1. **Overlay integrity + commit ordering** (HFS-001/002/005/011/012/013/014/020/021/022/039/040/067/088/090/121/122/123/124/125): fail closed, validate, persist-before-free, flush, ID/bitmap/entry consistency, allocation-file/metadata-range protection, and no silent native-write fallback.
2. **Names and identity** (HFS-003/004/024/025/026/036/037/047/048/069/070/108/109/126): bounded copies, stable logical-parent mapping, duplicate-shadow prevention, and catalog key-order validation.
3. **Boot/recovery** (HFS-117/118/119/132): native-only bootloader mount, boot-file preference, alternate volume-header read-only recovery; preserve overlay before any init.
4. **PictureFlow safety** (HFS-075/082/083/084/087/102/103/104/105/106/107/112/113/114/115/116/120/127/129/130/133): indexed accessors, checked sizes, NUL validation, alignment, error-checked writes, duplicate identity, no on-device rebuild.
5. **Partition/disk selection** (HFS-006/007/008/031/032/033/068/095/096/097/131): order, dual-unit probing, canonical scanner.
6. **Metadata/diagnostics** (HFS-081/085/093/094/098/099/101/110): timestamps, early-return fix, honest errors.
7. **Performance/hardening** (HFS-023/027/028/029/030/034/035/038/041/042/043/044/045/046/049/089/111): decompose, indexes, lazy state, document limits.
8. **Tooling/tests** (HFS-060–066/128): committed write/fault harness, macOS sanitizer, zip hygiene, DB build target, overlay-aware install checks.

## 12. Acceptance criteria for “bug-free enough to try”

- No open S0/S1 in `docs/hfsplus-audit.md`.
- Host tests include: overlay create/write/grow/realloc/shrink/truncate/rename/delete/remount; bitmap/entry fault injection; duplicate shadows; directory rename; long names; ID churn; partition false-positive/dual-unit; non-overlay EOF; PictureFlow malformed `.pfraw`/index; missing Album Artist; `.rockbox` tombstone/CNID.
- New regression tests: invalid overlay header must fail closed with no native write (HFS-123); colliding overlay ID must be rejected and `other.bin` must not open overlay data (HFS-124); rename → close-without-write → write must not leak blocks (HFS-125); a patched catalog parent key must not silently hide `.rockbox` (HFS-126); a malformed first MBR entry must not hide a valid later partition (HFS-131); a damaged primary volume header with an intact alternate must mount read-only with a diagnostic (HFS-132); duplicate albums by the first artist must be removed (HFS-127); a mismatched-name tombstone must block a Cover Flow install (HFS-128); track-list open/context-menu transitions must not leak/deadlock `buf_ctx_mutex` (HFS-129); a no-artist or negative-index album must not hash before the name pools (HFS-130); a selected theme must survive a clean reboot after the settings idle flush (HFS-133).
- ASan/UBSan clean on the host harness.
- Firmware and bootloader link.
- A supervised device run on a **backed-up** iPod passes `shipping.md` §16 (overlay init, settings persistence, Database, Cover Flow, Now Playing art, clean eject, update).
- A bootloader-native fallback test: hide `.rockbox` in the overlay on a copy and verify the bootloader still loads the native firmware.
- Only then consider an autonomous install.

## 13. Audit ID checklist

Every ID currently in the audit appears here or in the tables above:

`001 002 003 004 005 006 007 008 009 010 011 012 013 014 020 021 022 023 024 025 026 027 028 029 030 031 032 033 034 035 036 037 038 039 040 041 042 043 044 045 046 047 048 049 060 061 062 063 064 065 066 067 068 069 070 075 081 082 083 084 085 086 087 088 089 090 091 092 093 094 095 096 097 098 099 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120 121 122 123 124 125 126 127 128 129 130 131 132 133`

V1.1 applies the fixes listed in the V1.1 section above. The database-tool host glue restore and the `dir_get_info` compatibility mapping are also included. Remaining OPEN findings are intentionally not claimed fixed in this release.
