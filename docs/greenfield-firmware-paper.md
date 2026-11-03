# Helikon-OS: A Native HFS+ Bootloader and Firmware for the iPod Classic 6G

### A line-audited design study of the HFSPlus Rockboxed iPod tree

Status: research/design paper, 2026-11-03.  
Scope: every tracked subsystem in this repository, with custom modules audited at
file and function level and inherited Rockbox subsystems audited at module level.  
Purpose: decide what to keep, modify, rewrite or exclude if the project became a
completely new bootloader and firmware rather than a continuation of this fork.

Working name: **Helikon-OS**. It is used for the project, bootloader, firmware,
recovery environment, UI and host tools. This paper uses “the new firmware” and
“Helikon-OS” interchangeably.

**Revision 2 design decision.** Helikon-OS does **not** use a namespace overlay.
Native HFS+ is the writable filesystem. The donor `/.rockpod-rw` format is
retained only as migration material and as a source of validation ideas. The
heart of Helikon-OS is a real HFS+ read-write transaction engine, not an
overlay.

Revision 2 changes the paper in five places:

- the overlay is removed as the live write path;
- native HFS+ catalog, bitmap, extents and journal writes become the defining
  priority;
- the roadmap, validation gates, assembly order and non-coder path are rebuilt
  around a native write engine;
- the donor overlay is reclassified as migration/export material only;
- `fsck_hfs` compatibility and crash consistency become release blockers.

## Contents

- [0. Abstract](#0-abstract)
- [1. Mandate and constraints](#1-mandate-and-constraints)
- [2. Method and decision taxonomy](#2-method-and-decision-taxonomy)
- [3. Repository census](#3-repository-census)
- [4. The thesis in one sentence](#4-the-thesis-in-one-sentence)
- [5. Boot chain and bootloader](#5-boot-chain-and-bootloader)
- [6. S5L8702 HAL and target drivers](#6-s5l8702-hal-and-target-drivers)
- [7. Storage, ATA and power](#7-storage-ata-and-power)
- [8. Partition scanner and HFS+ reader](#8-partition-scanner-and-hfs-reader)
- [9. Native HFS+ read-write](#9-native-hfs-read-write)
- [10. Namespace, VFS and file cache](#10-namespace-vfs-and-file-cache)
- [11. Settings, diagnostics and debug UI](#11-settings-diagnostics-and-debug-ui)
- [12. Database, tagcache and browsing](#12-database-tagcache-and-browsing)
- [13. Playback, audio and codecs](#13-playback-audio-and-codecs)
- [14. UI, themes, fonts and assets](#14-ui-themes-fonts-and-assets)
- [15. Plugins](#15-plugins)
- [16. USB, DFU and updates](#16-usb-dfu-and-updates)
- [17. Build system, toolchain and host tools](#17-build-system-toolchain-and-host-tools)
- [18. Documentation and process](#18-documentation-and-process)
- [19. Proposed greenfield architecture](#19-proposed-greenfield-architecture)
- [20. Risk register](#20-risk-register)
- [21. What I would absolutely not include](#21-what-i-would-absolutely-not-include)
- [22. Migration from the current tree](#22-migration-from-the-current-tree)
- [23. Phased roadmap](#23-phased-roadmap)
- [24. Validation strategy](#24-validation-strategy)
- [25. Open design decisions](#25-open-design-decisions)
- [26. A non-coder's build path](#26-a-non-coders-build-path)
- [27. Assembly order and handoff checklist](#27-assembly-order-and-handoff-checklist)
- [Appendix A — Custom file ledger](#appendix-a--custom-file-ledger)
- [Appendix B — Inherited hotspot ledger](#appendix-b--inherited-hotspot-ledger)
- [Appendix C — Verdict summary](#appendix-c--verdict-summary)
- [Appendix D — Chosen name: Helikon-OS](#appendix-d--chosen-name-helikon-os)
- [Appendix E — Plain-language component catalogue](#appendix-e--plain-language-component-catalogue)
- [Appendix F — Glossary](#appendix-f--glossary)
- [Appendix G — FAQ](#appendix-g--faq)
- [Appendix H — Build worksheet](#appendix-h--build-worksheet)

---

## 0. Abstract

The repository is large: roughly **7,462 tracked files and 2.8 million lines**
across `apps/`, `firmware/`, `lib/`, `bootloader/`, `tools/`, `themes/`, `wps/`
and assets. Most of that is inherited Rockbox. The project-specific value is
concentrated in a much smaller set of modules: the S5L8702 board support, the
HFS+ reader, the format validators, the bootloader
integration, the host tools and the test fixtures.

The central recommendation of this paper is:

> **Copy the oscilloscope work and the on-disk validators; rewrite the operating
> system around them.**

A new firmware should keep the hard-won hardware sequences and the HFS+ format
knowledge, but it should not inherit the FAT-shaped VFS, the 5,700-line tagcache,
the 4,300-line playback state machine, the 946,000-line plugin tree, the legacy
overlay engine or the “copy all of Rockbox because it boots” architecture.

The target architecture is **recovery-first, native HFS+ writable, transactional
at every metadata change**:

- a tiny native-HFS+ bootloader that verifies and loads firmware and never
  writes the filesystem;
- a firmware built from a clean HAL, block layer, HFS+ read-write engine, VFS,
  host-built database, first-party player/UI and read-only USB;
- a real HFS+ journal/transaction engine, not an overlay;
- explicit power states, flush barriers and errors;
- no destructive repair actions and no hidden native-write fallback.

The priority order is deliberate:

1. native HFS+ read/write correctness;
2. crash consistency and `fsck_hfs` compatibility;
3. read-only playback and library browsing;
4. database, UI and first-party features;
5. migration, host tools and optional plugins.

If a feature conflicts with priority 1 or 2, the feature waits. Straight
writable HFS+ is the defining promise of Helikon-OS, not a later optimization.

---

## 1. Mandate and constraints

The exercise assumes a new bootloader and firmware for the iPod Classic 6G,
possibly expanding to other S5L8702 iPods later. The hardware facts are fixed:

| Constraint | Value |
|---|---|
| SoC | Samsung S5L8702, ARM926EJ-S |
| RAM | 64 MiB |
| Storage | ATA/LBA48, 512-byte logical sectors, 4096-byte virtual/physical support |
| Display | 320×240, 16-bit colour |
| Input | click wheel + buttons + hold switch |
| Audio | CS42L55 codec, lineout, headphone |
| Power | PMU, battery, charging, backlight, ATA sleep |
| Boot | Apple NOR path, DFU, dual-boot bootloader, `MOVE_AREA` limit |
| Filesystem | HFS+ v4 native; FAT fallback for non-HFS+ volumes |
| Host | macOS for installation, database and artwork tools |

Non-negotiable safety rules carried into the new design:

1. native HFS+ is the writable namespace, but every catalog, bitmap, extents,
   volume-header and journal change must go through a validated transaction;
2. a journaled volume with an invalid journal mounts read-only. A valid dirty
   journal is replayed only by the write engine, before the first write; a
   read-only mount never modifies it;
3. no format, erase, restore or repartition as a software repair;
4. the Apple NOR boot path is never destroyed;
5. every metadata write must remain compatible with macOS `fsck_hfs`;
6. a failed transaction fails closed and preserves the last consistent state;
7. no hardware or power claim without a recorded device measurement.

**Scope note.** This paper describes Helikon-OS, not the current V3.2 release.
V3.2 remains overlay-based. Its `/.rockpod-rw` is user state and must not be
deleted, truncated or reformatted. Nothing in this paper authorizes a
destructive migration of an existing device.

---

## 2. Method and decision taxonomy

Every tracked module was classified with one primary verdict:

| Tag | Meaning |
|---|---|
| **K — Keep** | Copy nearly verbatim; proven hardware/format knowledge. |
| **M — Modify** | Keep the concept or data format, but refactor the implementation or boundary. |
| **R — Replace** | Rewrite the module; the current design is the wrong shape for a new product. |
| **X — Exclude** | Do not ship in the new firmware; may remain in the donor tree or host tooling. |
| **D — Defer** | Valuable later, but not in the first bootable product. |

Three further rules were applied:

- **Safety first:** native metadata writes are allowed only inside a validated
  transaction. Release builds require journaling; a non-journaled writer is a
  disposable-volume prototype. Code that bypasses the transaction engine is
  excluded.
- **Complexity budget:** a module must justify its lines by user value or
  irreplaceable hardware knowledge.
- **One source of truth:** one HFS+ write engine, one VFS, one database format;
  no live overlay and no legacy database formats in the same core.

The audit is file-by-file for custom code and module-by-module for inherited
Rockbox. “Every line” is represented by the line-count ledgers in Appendices A
and B; the prose explains the decision for each cohesive block.

---

## 3. Repository census

At the audit date:

| Area | Files | Lines | Nature | Primary verdict |
|---|---:|---:|---|---|
| `apps/` | 3,029 | 1,790,709 | Rockbox applications, GUI, plugins, recorder | Replace / exclude most |
| `firmware/` | 2,252 | 475,073 | kernel, drivers, target, HFS+ adapter | Selective keep |
| `lib/` | 1,266 | 363,177 | codecs, DSP, supporting libraries | Selective keep |
| `tools/` | 247 | 70,170 | host tools, tests, inspectors | Keep / rewrite |
| `bootloader/` | 41 | 12,371 | bootloader and shared boot code | Selective keep |
| `utils/` | 18 | 4,283 | mks5lboot and utilities | Keep / harden |
| `themes/` | 133 | 55,636 | theme assets | Select one |
| `wps/` | 245 | 80,427 | Rockbox WPS/skin files | Exclude most |
| `fonts/` | 90 | large (binary/BDF) | font assets | Select two or three |
| `icons/` | 58 | 21,566 | icon assets | Select a set |
| `docs/` | 49 | 11,505 | design, audit, release, validation | Keep |

The inherited application and plugin tree is more than **60% of the entire
repository**. A new firmware that copies it has not reduced complexity; it has
renamed the fork.

The custom core is much smaller:

| Custom group | Lines |
|---|---:|
| HFS+ reader, partition and adapter (`firmware/hfsplus/`) | ~5,792 |
| iPod 6G target drivers (`firmware/target/arm/s5l8702/ipod6g/`) | ~3,357 |
| Bootloader integration (`bootloader/ipod-s5l87xx.c`) | 1,016 |
| Host HFS+ tools and tests (`tools/hfsplus/`) | ~2,958 |
| Host database/artwork tools (`tools/rockpod/`) | ~1,036 |
| DFU installer (`utils/mks5lboot/`) | ~3,382 |

That is the part worth preserving first.

---

## 4. The thesis in one sentence

A new firmware should be **a small, recovery-first system with a real HFS+
read-write engine**, a clean VFS and a first-party player/UI, rather than a
general-purpose Rockbox distribution with an HFS+ adapter bolted into the FAT
layer. There is no namespace overlay in Helikon-OS.

---

## 5. Boot chain and bootloader

### 5.1 Current shape

`bootloader/ipod-s5l87xx.c` (1,016 lines) performs:

1. CPU/clock/PMU/GPIO/I2C/SPI/LCD/button/power bring-up;
2. battery safety trap and charging checks;
3. `storage_init()`;
4. `filesystem_init()` and `disk_mount_all()`;
5. HFS+ partition scan/mount;
6. `load_firmware()` from `/.rockbox/rockbox.ipod` into DRAM;
7. jump to the loaded image at a fixed address.

The bootloader shares the HFS+ adapter. It links only the read-only HFS+
reader, not the donor overlay or any write path. The firmware image is not
authenticated; `load_firmware()` trusts the file it reads.

`utils/mks5lboot/` (3,382 lines) builds DFU installers, scans DFU devices,
installs/uninstalls the dual bootloader, and contains the button-map and NOR
knowledge. `--single` destroys the Apple NOR boot path and must never be used.

### 5.2 Verdict

**Modify the boot flow; keep the hardware and DFU knowledge.**

| Component | Verdict | Action |
|---|---|---|
| CPU/clock/PMU/GPIO/I2C/SPI bring-up | **K** | Keep, wrap in boot HAL. |
| LCD/button/backlight init for recovery UI | **K/M** | Keep minimal text/recovery UI only. |
| `storage_init()`/ATA bring-up | **K/M** | Keep transport; remove policy. |
| `filesystem_init()`/`disk_mount_all()` | **M** | Replace with native-only mount. |
| HFS+ partition scan | **K** | Reuse scanner. |
| HFS+ native mount | **K** | Reuse read-only reader. |
| Filesystem write code in bootloader | **X** | Do not link at all. |
| `load_firmware()` fixed-address jump | **M/R** | Add manifest, hash, size and board checks; load to validated address. |
| DFU installer and dual-boot knowledge | **K/M** | Keep, add image verification and recovery gates. |
| `--single` | **X** | Never expose. |
| Verbose error screens | **K/M** | Keep codes, add recovery instructions. |

### 5.3 Proposed bootloader

The new bootloader should be a **rescue loader**, not a small OS:

- native HFS+ read-only only;
- no overlay parsing, no filesystem writes, no plugins, no audio, no database;
- verify a firmware manifest: magic, board ID, version, load address, size,
  SHA-256 and signature;
- load `/.firmware/current.img` or a fallback image;
- offer Apple / firmware / recovery / USB modes;
- keep the Apple NOR path and DFU recovery intact;
- enforce a code-size/MOVE_AREA budget in the build;
- expose a recovery screen with error codes and a safe power-off.

A boot counter or A/B fallback is desirable. It can be an ordinary native HFS+
file, `/.helikon/bootstate`, with two checksummed slots. The firmware writes it
only through the HFS+ transaction engine; the bootloader reads it read-only and
never writes the filesystem. If the file is missing, fails its checksum, or the
volume cannot be read consistently, the bootloader falls back to the default
image and button-selected recovery. No reserved NOR variable and no hidden
container header is needed.

---

## 6. S5L8702 HAL and target drivers

### 6.1 Current shape

The target directory contains the reverse-engineered hardware support:

| File | Lines | Role |
|---|---:|---|
| `storage_ata-6g.c` | 1,406 | ATA PIO/DMA, LBA48, CE-ATA, power/sleep, flush |
| `pmu-6g.c` | 492 | PCF5063x PMU, charging, backlight, interrupts |
| `power-6g.c` | 237 | power off/reboot, charger detection |
| `lcd-6g.c` | 266 | LCD controller and panel |
| `serial-6g.c` | 284 | serial/debug |
| `audio-6g.c` | 140 | I2S/audio routing |
| `piezo-6g.c` | 125 | clicker |
| `adc-6g.c` | 117 | battery/accessory ADC |
| `rtc-6g.c` | 75 | real-time clock |
| `powermgmt-6g.c` | 75 | battery curves |
| `backlight-6g.c` | 74 | PMU backlight registers |
| `cscodec-6g.c` | 66 | codec control |
| shared `s5l8702/` | 15,616 | clocking, GPIO, I2C, SPI, DMA, timers, etc. |

`firmware/backlight.c` (1,090 lines) and `firmware/storage.c` (852 lines) are
shared policy layers above these drivers.

### 6.2 Verdict

**Keep the hardware sequences; replace the policy layers.**

- **K:** register definitions, clock/PLL setup, GPIO/I2C/SPI, PMU voltage and
  charger sequencing, LCD init, click-wheel scanning, audio codec init.
- **M:** split each driver into “hardware primitive” and “policy.” The HAL
  should expose explicit operations, not global state.
- **R:** the ATA power/sleep/flush policy, backlight policy and storage idle
  policy. The V3.2 beta cycle proved that ad-hoc state machines here can freeze
  the device or leave it dead after sleep.
- **X:** any periodic early-boot callback, implicit sleep from the backlight
  path, and cached PMU state without invalidation.

A new HAL should define:

```c
struct block_device {
    int  (*read)(void *ctx, uint64_t lba, unsigned int count, void *buf);
    int  (*write)(void *ctx, uint64_t lba, unsigned int count, const void *buf);
    int  (*flush)(void *ctx);
    int  (*power)(void *ctx, enum power_state state);
    struct device_info info; /* logical/physical sector size, capacity, flags */
};
```

No application or filesystem should touch ATA registers, PMU registers or GPIO.

---

## 7. Storage, ATA and power

### 7.1 Current shape

`storage_ata-6g.c` is the largest target driver. It handles:

- ATA identification, LBA28/LBA48, PIO and DMA;
- CE-ATA command path;
- 512-byte and 4096-byte logical/physical sector translation;
- power-up, clock gate, deep sleep, standby, flush;
- SSD/HDD detection and storage-mode switches;
- storage event handling (`Q_STORAGE_*`).

The V3.2 history shows why this area is dangerous:

- an early-boot idle-notify callback froze the device at 4–6 seconds;
- an explicit SSD sleep path left the device dead after hold/inactivity sleep;
- flush errors were historically swallowed;
- backlight/PMU caching without invalidation made wake behavior fragile.

### 7.2 Verdict

**Keep the transport; rewrite the state machine.**

- **K:** command encoding, timing loops, DMA/PIO transfer, sector translation.
- **M:** error taxonomy, flush result propagation, power-state transitions.
- **R:** sleep policy, idle notifications, mode switching, backlight coupling.
- **X:** periodic early-boot callbacks, implicit sleep, unmeasured defaults,
  cache-without-invalidation.

The new power model should be explicit:

```text
ACTIVE -> IDLE -> STORAGE_GATED -> STORAGE_DEEP -> SYSTEM_SLEEP -> OFF
```

Every transition names its wake source and its required restore steps. A write
is durable only after a successful flush/barrier; a flush error is returned to
the caller. No “return 0 because the device is probably fine.”

---

## 8. Partition scanner and HFS+ reader

### 8.1 Partition scanner

`hfsplus_partition.c` (296 lines) scans APM, MBR, GPT and raw volumes at 512 or
4096 bytes, handles extended/logical MBR, and chooses the HFS+ candidate.

**Verdict: K.** Keep, expand malformed-table tests, never write the partition
table. The USB MBR relabel is a RAM-only presentation detail and must remain
RAM-only.

### 8.2 HFS+ read-only core

`hfsplus_ro.c` (1,274 lines) and `hfsplus_ro.h` (163 lines) implement:

- primary/alternate volume-header validation;
- block-size, allocation and catalog bounds;
- journal presence and dirty-volume refusal;
- catalog B-tree header/link/offset validation;
- directory/CNID indexes and fast lookup;
- fork/extent parsing, including read-only extents-overflow support;
- UTF-16BE name decoding and exact-byte lookup;
- record parsing for folders, files and threads.

**Verdict: K/M.** Keep the parser and validation; split it into smaller modules:

```text
hfs_volume.c     header, journal, allocation
hfs_btree.c      generic B-tree node/offset validation
hfs_catalog.c    record parsing and directory index
hfs_extents.c    inline + overflow extents
hfs_unicode.c    UTF-16BE -> UTF-8 policy
hfs_read.c       fork reads and block mapping
```

The reader is the strongest part of the repository. It should become a
standalone library with fuzzing, property tests and a stable error taxonomy.

### 8.3 Policy changes

- **Never hide unsupported entries.** The current namespace skips entries marked
  `unsupported`, which made large plugin data files appear missing. A new
  firmware should list the entry and return `UNSUPPORTED` with a reason.
- **Normalization must be explicit.** HFS+ stores UTF-16; macOS may present
  decomposed forms. The donor’s exact-byte lookup is a read-only behavior, not
  a writable policy. The write engine must implement Apple’s case-folding
  comparison and a documented normalization rule, and test both against
  macOS-created names.
- **Hardlinks, symlinks, compression, resource forks:** implement read-only
  support or reject with a specific code. Do not silently skip.
- **Extents overflow:** keep the read-only path, raise or remove the fixed
  extent cache limit only with a measured design, and expose the limit.
- **Journal:** the read-only reader never modifies the journal. A dirty journal
  mounts read-only with a specific `JOURNAL_DIRTY` code. The write engine owns
  replay and must complete it before the first write (§9.3).

---

## 9. Native HFS+ read-write

**This is the defining priority of Helikon-OS.** If the write engine is not
correct, the product is not Helikon-OS; it is a read-only player with a different
name. Everything else in this paper is downstream of this section.

### 9.1 Decision

Helikon-OS has **no namespace overlay**. Native HFS+ is the writable
filesystem. There is no live `/.rockpod-rw` (the donor path, sometimes called
`.rockbox.rw`), no hidden container and no "shadow namespace." Files created,
renamed, truncated or deleted by the device are ordinary HFS+ files.

This is the largest single change from the donor design. The donor overlay is
retained only for two purposes:

1. migration of existing user data into native HFS+;
2. a reference corpus for validation and diagnostics.

The donor overlay is not the live write path, not the settings store, not the
database store and not the update mechanism.

### 9.2 What direct HFS+ writes actually require

Straight writable HFS+ is not a flag. It is a full filesystem write
implementation. The minimum surface is:

| Area | What must be implemented |
|---|---|
| Catalog B-tree | Insert, update and delete folder/file records; thread records; node split/merge; free-node map; leaf links; key ordering; CNID allocation. |
| Allocation bitmap | Mark blocks used/free; update the volume header free-block count; keep allocation and extents consistent. |
| Extents overflow | Insert, update and delete overflow records for files with more than eight extents. |
| Volume header | Write count, modify date, free blocks, next CNID and alternate-header consistency. |
| Journal | Append and replay HFS+ journal transactions, or refuse writes on journaled volumes. |
| File data | Allocate, write, zero-fill, truncate and synchronize data forks. |
| Metadata | Timestamps, link counts, permissions, Finder info and flags. |
| Unicode | Implement HFS+ case-folding comparison and a normalization policy for create, lookup and rename. |
| fsck compatibility | Every metadata change must remain something macOS `fsck_hfs` can validate; Helikon-OS does not repair. |

This is the hard part of Helikon-OS. The overlay was small because it never had
to understand HFS+ B-tree mutation.

### 9.3 Journal policy

HFS+ volumes are normally journaled. A journaled volume cannot be safely written
by simply ignoring the journal.

Two honest paths exist:

**A. Journaled HFS+ writes (recommended for release).**
Implement the HFS+ journal as a block transaction log:

- reserve/append journal blocks;
- write transaction records with sequence numbers and checksums;
- commit the journal header;
- apply the metadata blocks;
- replay incomplete transactions on mount;
- mark the journal clean.

This is the only path that is safe for a journaled volume and compatible with
macOS.

**B. Non-journaled HFS+ writes (prototype only).**
Allow writes only on a volume explicitly formatted without a journal. Mount
journaled volumes read-only. This is simpler but still requires catalog,
bitmap and extents writes, and a power loss may require `fsck_hfs`.

Helikon-OS must never silently disable journaling, never delete the journal and
never pretend a dirty journal is clean.

### 9.4 Transaction and crash-consistency model

- one writer at a time, serialized at the filesystem boundary;
- a transaction groups all catalog, bitmap, extents and volume-header changes;
- data is written before the metadata that exposes it;
- flush/barrier before commit and after commit;
- replay is idempotent;
- any journal or checksum failure mounts read-only;
- no auto-repair, no zeroing, no "try again and hope."

The power-loss matrix must cover create, append, close, rename, delete,
truncate, mkdir, settings save, database install and artwork install.

### 9.5 Donor overlay verdict

| Donor component | Verdict | Use in Helikon-OS |
|---|---|---|
| `hfsplus_overlay.h` | **K** | Read the old format for migration. |
| `hfsplus_overlay_format.c` | **K** | Parse v1/v2/v3 containers in the host exporter. |
| `hfsplus_overlay_validate.c` | **K** | Validate old entries before export. |
| `hfsplus_overlay_load.c` | **X** | No runtime overlay loader. |
| `hfsplus_overlay_alloc.c` | **X** | No overlay allocator. |
| `hfsplus_overlay_store.c` | **X** | No overlay entry store. |
| `hfsplus_index_cache.c` | **M** | Keep the cache idea for native directories. |
| `hfsplus_diag.c` | **M** | Keep stage/reason vocabulary for migration. |
| `/.rockpod-rw` | **X** | Not part of Helikon-OS. |

### 9.6 Native filesystem API

A clean API should make transactions explicit:

```c
int  hfs_begin(struct hfs_txn *tx);
int  hfs_catalog_insert(struct hfs_txn *tx, ...);
int  hfs_catalog_update(struct hfs_txn *tx, ...);
int  hfs_catalog_delete(struct hfs_txn *tx, ...);
int  hfs_bitmap_alloc(struct hfs_txn *tx, uint32_t blocks, ...);
int  hfs_bitmap_free(struct hfs_txn *tx, uint32_t start, uint32_t blocks);
int  hfs_extents_insert(struct hfs_txn *tx, ...);
int  hfs_journal_commit(struct hfs_txn *tx);
int  hfs_txn_abort(struct hfs_txn *tx);
```

The VFS sees ordinary files. There is no "native view" and "overlay view" to
merge.

### 9.7 Feasibility and phasing

Direct HFS+ writes are a multi-stage project:

| Phase | Scope | Gate |
|---|---|---|
| 9A | Native HFS+ read-only, journaled and non-journaled | Mount, browse, play; no writes. |
| 9B | Non-journaled HFS+ write prototype | Create/write/rename/delete on a disposable test volume; `fsck_hfs` reports zero repairs. |
| 9C | Journaled HFS+ write engine | Journal append/replay; `fsck_hfs` zero repairs after power cuts. |
| 9D | Full metadata parity | Permissions, timestamps, extents overflow, Finder info. |
| 9E | Release hardening | Adapter matrix, 100-cycle resume, power-loss matrix. |

The honest schedule is months to years, not a beta cycle. If that scope is not
acceptable, the alternative is to keep Helikon-OS read-only and use macOS Disk
Mode for writes. That is a product decision, not a code shortcut.

## 10. Namespace, VFS and file cache

### 10.1 Current shape

| File | Lines | Role |
|---|---:|---|
| `hfsplus_namespace.c` | 686 | donor native + overlay merge, readdir, rename/remove |
| `hfsplus_fat.c` | 377 | FAT/VFS adapter boundary |
| `hfsplus_fat.h` | 109 | error mapping, diagnostics helpers |
| `firmware/common/fat.c` | 3,187 | FAT, HFS+ dispatch, directory scans, cache |
| `firmware/common/file.c` | 1,433 | POSIX-like file API, stream cache |
| `firmware/common/file_internal.c` | 792 | path walk, open/create/remove/rename |
| `firmware/common/fileobj_mgr.c` | 451 | shared file objects and caches |
| `firmware/common/disk.c` | 744 | partition/mount integration |

The current VFS is FAT-shaped. HFS+ is dispatched through `fat_is_readonly()`
inside FAT functions; path resolution scans directories with `readdir`; file
handles carry FAT-specific state; dircache is disabled for HFS+.

### 10.2 Verdict

**There is no namespace merge in Helikon-OS.** Native HFS+ is the only
namespace. Replace the VFS; reuse the donor’s identity rules and selected cache
ideas.

- **K:** parent-CNID plus name identity, catalog iterator discipline, bounded
  name cache, read-ahead concept, explicit error taxonomy.
- **M:** `hfsplus_namespace.c` becomes `hfs_lookup.c` and `hfs_iterate.c` with
  no overlay branch.
- **M:** `hfsplus_fat.c` becomes a thin `fs_ops` adapter, not the center of the
  world.
- **R:** `fat.c`, `file.c`, `file_internal.c`, `fileobj_mgr.c`.
- **X:** overlay visibility rules, tombstones, shadowing, legacy duplicate
  resolution, `force_readonly` fallback, `WRITE_OVERLAY`, `READ_ONLY_NATIVE`.

The donor’s rename/remove logic is not carried over as a separate namespace
policy. In Helikon-OS a rename is a catalog record update and a remove is a
catalog record delete; there is no hidden object to reconcile.

### 10.3 Proposed VFS

```c
struct fs_ops {
    int (*mount)(struct fs *fs, struct block_device *bd);
    void (*unmount)(struct fs *fs);
    int (*lookup)(struct fs *fs, const struct dir *parent,
                  const char *name, struct dirent *out);
    int (*iterate)(struct fs *fs, const struct dir *parent,
                   struct dir_iter *it, struct dirent *out);
    int (*open)(struct fs *fs, const struct dirent *de,
                unsigned int flags, struct file **out);
    int (*create)(struct fs *fs, const struct dir *parent, const char *name,
                  unsigned int flags, struct file **out);
    ssize_t (*read)(struct file *f, void *buf, size_t n);
    ssize_t (*write)(struct file *f, const void *buf, size_t n);
    int (*truncate)(struct file *f, uint64_t size);
    int (*sync)(struct file *f);
    int (*close)(struct file *f);
    int (*rename)(struct fs *fs, struct file *f, const char *name);
    int (*remove)(struct fs *fs, struct file *f);
    int (*mkdir)(struct fs *fs, const struct dir *parent, const char *name);
    int (*rmdir)(struct fs *fs, const struct dir *parent, const char *name);
    int (*stat)(struct fs *fs, const struct dirent *de, struct stat *st);
    int (*flush)(struct fs *fs);
};
```

Capabilities are explicit:

```text
READ, WRITE, DIRECTORY, SEEKABLE, RESOURCE_FORK, SYMLINK
```

`WRITE` means a native HFS+ transaction. It does not mean “write to an overlay”
and it does not mean “direct sector write.” `RESOURCE_FORK` and `SYMLINK` are
optional capabilities; a file that has one is reported as `UNSUPPORTED` until
the corresponding writer exists, never silently hidden.

Errors are specific: `NOT_FOUND`, `NOT_DIR`, `IS_DIR`, `EXISTS`, `UNSUPPORTED`,
`IO`, `NO_SPACE`, `READ_ONLY`, `CORRUPT`, `JOURNAL_DIRTY`, `NAME_INVALID`.
No errno guessing at the boundary.

### 10.4 Namespace rules for a writable volume

Reading exact bytes is not enough once the device writes. A writable HFS+
implementation must reproduce Apple’s catalog key ordering, or macOS and
`fsck_hfs` will disagree with the device about where a name belongs.

- **Catalog comparison:** implement Apple’s HFS+ case-folding comparison
  (`FastUnicodeCompare`) and use it for B-tree key order, lookup, create,
  rename and duplicate detection.
- **Normalization:** HFS+ stores decomposed UTF-16. The writer must either
  decompose names before insertion or explicitly refuse a name that would
  create two catalog keys that normalize to the same string. The policy is
  tested against macOS-created fixtures.
- **Length and characters:** reject names over 255 UTF-16 units, embedded NUL,
  `/`, and the reserved `.`/`..` forms. Decide and test the `:`/`/` mapping
  against macOS; the donor maps `/` to `:` and rejects a literal `:`.
- **CNIDs:** allocate monotonically, never reuse a live CNID, and keep the
  volume header’s next-CNID field consistent with the catalog.
- **Metadata preservation:** preserve BSD info, Finder info, flags and
  timestamps when updating an existing record. Do not rewrite a record with a
  zeroed field the donor parser did not understand.
- **Unsupported objects:** hardlinks, symlinks, compressed files and resource
  forks are listed and return a specific error. They are not omitted from
  `readdir`.

### 10.5 Caches

- one bounded B-tree node cache per volume, invalidated by node number on every
  metadata write;
- one bounded name cache keyed by `(parent CNID, name bytes)`, invalidated on
  create, rename and delete;
- a bounded write-behind data cache, flushed at transaction commit;
- no dircache, no global path cache, no unbounded `core_alloc`;
- every cache has a maximum, a hit/miss counter and an eviction policy.

The cache is an optimization. Correctness must not depend on a cache hit.

---

## 11. Settings, diagnostics and debug UI

### 11.1 Settings

`apps/settings.c` (1,511 lines) and `apps/settings_list.c` (2,468 lines) form a
large table-driven settings system with runtime status files, CRC tracking,
config promotion and callbacks.

**Verdict: M/R.** Keep the user-facing settings and the table-driven idea; rewrite
the storage layer:

- versioned settings schema;
- ordinary native files under `/.helikon/settings/`, not an overlay record;
- two redundant config slots with checksums and atomic promotion;
- explicit defaults and migration;
- no settings write from arbitrary callbacks;
- no “deferred until idle” behavior that can hide a failed save.

### 11.2 Diagnostics

`hfsplus_diag.c` (161 lines), `apps/debug_menu.c` (3,141 lines) and `logf` are
valuable. **K/M:** keep the counters and stage/reason/slot diagnostics; split
the debug menu into a small diagnostic panel and a host-readable event log.

Persistent logs are ordinary native HFS+ files under `/.helikon/logs`. They are
written only through the HFS+ transaction engine, only when the volume is
mounted writable and the transaction engine is healthy. If the filesystem is
read-only, dirty or failing, logs stay in a bounded RAM ring and are exported
over serial or read-only USB. Diagnostics must never be the reason a failing
filesystem takes one more write.

---

## 12. Database, tagcache and browsing

### 12.1 Current shape

| File | Lines | Role |
|---|---:|---|
| `apps/tagcache.c` | 5,756 | database build/load, RAM cache, changelog, commands |
| `apps/tagtree.c` | 2,975 | browser tree, chunk loading, cursors |
| `apps/playlist.c` | 4,154 | playlist storage and control |
| `apps/playlist_viewer.c` | 1,341 | playlist UI |
| `apps/recorder/albumart.c` | 435 | album-art probing/cache |

The tagcache has a RAM cache, dircache fallbacks, an on-disk changelog, a
command queue, a persistent filename fd and a browser cursor. For 10,000 tracks
it is a major source of boot time and complexity.

### 12.2 Verdict

**Replace the database; keep the host-builder idea and the album-art cache
policy.**

- **R:** tagcache/tagtree. Use an immutable, host-built database with a
  documented binary schema and sorted string pools.
- **R:** playlist storage; a simple versioned playlist format, not the
  control-file machinery.
- **M:** album-art probing: one directory result cache, explicit invalidation,
  no silent negative caching across changes.
- **D:** on-device full database rebuild. Keep it host-side until proven safe.
- **X:** dircache, changelog command queue, legacy tagcache formats.

A new database might be:

```text
header (magic, version, counts, checksum)
string pool (UTF-8, length-prefixed)
track records (path offset, metadata offsets, numeric fields)
indexes: path hash, artist/album sorted, title sorted
runtime stats log: play counts, ratings, resume points (native file, transactional)
```

Built on macOS, validated on mount, memory-mapped/read-only on device.

Because Helikon-OS can now create and delete native files, the database is a
cache, not the source of truth. Every path is validated against the catalog when
it is opened. A missing or stale entry falls back to a directory scan and
reports `DB_STALE`; it is never presented as “file missing.” A full on-device
rebuild stays deferred until the host-built path and the write engine are
proven.

---

## 13. Playback, audio and codecs

### 13.1 Current shape

`apps/playback.c` (4,363 lines) is the playback/buffering state machine. The
codec and DSP code lives in `lib/rbcodec` (350,947 lines) and the audio drivers
in `firmware/drivers/audio`.

### 13.2 Verdict

**Keep the codecs and DSP; rewrite the player state machine.**

- **K:** selected codecs (MP3, AAC/M4A, ALAC, FLAC, WAV, etc. as product
  requires), DSP, sample-rate conversion, replaygain primitives.
- **R:** buffering, track transitions, resume, gapless, event handling.
- **M:** album art, metadata, playlist integration.
- **X:** codecs/plugins the product does not need; carrying every Rockbox codec
  is a size and attack-surface decision, not a feature.

Audio quality is a first-class acceptance criterion: no clicks, correct
sample rates, gapless where supported, lineout/headphone switching, safe volume.

---

## 14. UI, themes, fonts and assets

### 14.1 Current shape

- `apps/gui/` — 20,127 lines of viewport, skin, list, statusbar and widget code;
- `wps/` — 80,427 lines of WPS/skin definitions;
- `themes/` — 55,636 lines across 133 theme files;
- `fonts/` — 90 font assets (large BDF/binary footprint);
- `icons/` — 58 icon assets;
- `apps/recorder/` — 13,696 lines of recorder-specific UI.

### 14.2 Verdict

**Build a new first-party UI; keep a small asset set.**

- **K:** selected fonts (system + one UI font), a small icon set, the 320×240
  layout knowledge.
- **M:** viewport/layout concepts, click-wheel acceleration.
- **R:** screen stack, menus, list rendering, theme engine.
- **X:** the full WPS/skin language, 133 themes, recorder UI.
- **D:** user themes; define a new simple theme format after the core UI is
  stable.

A new UI should be retained-mode and declarative:

```text
screen -> widgets -> layout -> renderer
input -> focus -> action
```

with one theme format for colours, fonts, spacing and backgrounds.

---

## 15. Plugins

### 15.1 Current shape

`apps/plugins/` is 2,602 files and **946,251 lines**: games, viewers, tools,
emulators, demos and a plugin API that exposes raw hardware, storage and
firmware functions.

### 15.2 Verdict

**Exclude the plugin tree from the new firmware.**

This is the largest single complexity and security decision in the paper. A new
firmware should ship first-party features only. If plugins return, they need:

- a capability-based API (read file, draw, play sound), not raw pointers;
- memory/CPU quotas;
- no raw ATA, PMU, NOR or flash access;
- a signature or explicit trust model;
- a crash boundary that can restart the plugin without the firmware.

Until that exists, “plugins” means “first-party features.” PictureFlow, album
art and the database browser should be product features, not plugins.

---

## 16. USB, DFU and updates

### 16.1 Current shape

HFS+ firmware exposes a read-only USB mass-storage LUN and relabels the data
partition as `Apple_HFS` in RAM so macOS probes it. Apple Disk Mode is the write
path. `utils/mks5lboot` handles DFU install/uninstall and the dual-boot NOR
layout.

### 16.2 Verdict

**Keep read-only USB and DFU; redesign update verification.**

- **K:** read-only LUN, RAM-only MBR relabel, DFU transport, dual-boot button
  map, Apple NOR preservation.
- **M:** USB descriptors and error paths; explicit “read-only” reporting.
- **R:** firmware update flow: manifest, hash, signature, rollback policy.
- **X:** writeable USB mass storage on HFS+; host writes that bypass the HFS+
  transaction engine; `--single`.

Updates should be a first-class protocol:

```text
firmware.img + firmware.manifest
  magic, board, version, load address, size, SHA-256, signature
```

The bootloader verifies before jumping. The firmware writes the new image and
manifest to ordinary staging files under `/.helikon/update/` through the HFS+
transaction engine, flushes them, reads them back and verifies the hash. The
bootloader then reads the manifest and image from native HFS+ read-only. The
update is not “installed” until the bootloader has verified it and the firmware
has committed a boot-state record under `/.helikon/bootstate`. If verification
fails, the bootloader keeps the previous image and offers recovery. Bootloader
changes themselves remain a DFU operation with their own verification; they are
not staged on HFS+. This is an open design decision; see §25.

---

## 17. Build system, toolchain and host tools

### 17.1 Current shape

- Rockbox `tools/configure` + recursive Makefiles;
- ARM `arm-elf-eabi-gcc` 9.5.0;
- `tools/scramble`, `bmp2rb`, `utils/mks5lboot`;
- `tools/hfsplus` inspector and tests;
- `tools/rockpod` host database, artwork and overlay-migration tools;
- `tools/database` host DB builder;
- `tools/xerj` optional local semantic index.

### 17.2 Verdict

- **K:** cross toolchain, scramble, `bmp2rb`, DFU installer, HFS+ tests.
- **M:** build system. Keep Rockbox make for bring-up, but add:
  - reproducible builds;
  - size budgets and map assertions;
  - signed manifests;
  - host test and fuzz targets in CI;
  - deterministic asset pipeline.
- **R:** host database/artwork tools for the new schema.
- **X:** `tools/xerj` from the firmware repo (optional research tool, not
  product).

A clean build could move to CMake/Ninja once the bootloader and firmware
interfaces are stable, but not before the hardware bring-up works.

---

## 18. Documentation and process

Keep:

- `agents.md` invariants and release rules;
- `shipping.md` recovery/install gates;
- `docs/hfsplus-audit.md` as a risk register;
- `docs/efficiency-paper.md` as measurement discipline;
- `docs/hardware-validation.md` as the device matrix;
- release notes and hashes.

Modify:

- rename the product consistently;
- replace “Rockbox feature list” docs with a product spec;
- keep a live risk register and decision log.

Exclude:

- claims that host tests equal device validation;
- release notes that hide unexecuted stress gates;
- instructions that treat format/erase as a first response.

---

## 19. Proposed greenfield architecture

```text
Apple NOR boot ROM
   |
   +--> Apple firmware (untouched)
   |
   +--> Helikon-OS rescue bootloader (NOR)
          - native HFS+ read-only
          - manifest/hash/signature verification
          - boot-state read
          - Apple / Helikon-OS / recovery / USB menu
          - load validated image to DRAM
                 |
                 v
          Helikon-OS firmware
            board HAL
              block device
                HFS+ read/write engine
                  catalog B-tree mutation
                  allocation bitmap
                  extents + overflow
                  journal / transaction commit
                VFS
                  database (immutable, host-built)
                  player/audio
                  UI
                  USB read-only
                  diagnostics
```

### 19.1 Module boundaries

| Layer | Owns | Must not own |
|---|---|---|
| Board HAL | registers, clocks, PMU, ATA, LCD, codec | filesystems, UI, policy |
| Block | sector I/O, flush, power state | paths, files, caches |
| HFS+ core | volume/header, B-tree parse, catalog read, extents read | writes, UI, policy |
| HFS+ write engine | catalog/bitmap/extents mutation, CNID allocation, metadata parity | journal format, UI, boot |
| Journal/transaction | log append, commit, replay, abort, barriers | product policy, UI |
| VFS | lookup, handles, capabilities, path rules | hardware registers |
| Database | immutable metadata and index | playback state |
| Player | buffer, decode, output | filesystem internals |
| UI | screens, input, rendering | storage policy |
| USB | LUN, descriptors | filesystem writes |
| Diagnostics | events, counters, recovery | product policy |

There is no overlay layer, no namespace merge and no second write path. Every
mutation goes through the HFS+ write engine and the journal/transaction layer.

### 19.2 Memory budget

64 MiB must be treated as a hard budget:

- bootloader: code + stack inside `MOVE_AREA`;
- kernel/HAL: small, no large BSS;
- HFS+ read cache: bounded B-tree node cache and name cache;
- HFS+ write engine: one transaction’s dirty metadata blocks, not a full
  catalog copy;
- journal: bounded log buffer and replay window;
- database: read-only mapped pages, not a full 10k-track RAM copy unless
  measured;
- audio: codec + buffer;
- UI: one screen + one theme;
- reserve for recovery/diagnostics.

Every cache needs a maximum and an eviction policy. No unbounded `core_alloc`.
A transaction that cannot fit its dirty set must fail with `NO_SPACE` before it
changes anything.

### 19.3 Security model

- signed firmware manifest; verify before jump;
- boot-state file read-only from the bootloader and checksummed;
- read-only USB by default;
- native metadata written only through the transaction engine, with ordering
  and checksums;
- journal replay is bounded and idempotent;
- path names length-checked, no traversal;
- plugins only with capabilities, or not at all;
- DFU installer verifies the image and refuses `--single`;
- no secrets on the device; no network.

### 19.4 Update and recovery

- keep Apple firmware and DFU path;
- firmware image staged under `/.helikon/update/`, flushed and verified;
- explicit “install update” action, never silent;
- bootloader verifies the manifest before jumping;
- recovery screen: read-only mount, export logs, rollback, DFU instructions;
- no auto-format, no journal deletion, no metadata “repair” as a first response.

---

## 20. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| NOR/DFU brick | Device unusable | Keep Apple path, never `--single`, recovery plan, hardware test. |
| HFS+ parser corruption | Data loss / crash | Standalone parser, fuzzing, bounds checks. |
| B-tree split/merge bug | Catalog structural corruption, lost files | Model-based B-tree tests, differential tests against macOS, `fsck_hfs` after every crash image. |
| Catalog key order / case-folding bug | Duplicate names, failed lookup, macOS disagreement | Implement Apple’s `FastUnicodeCompare`, test with macOS-created fixtures. |
| Journal append/replay bug | Replayed garbage or lost committed metadata | Checksums, sequence numbers, idempotent replay, crash matrix at every boundary. |
| fsck divergence | macOS sees a damaged volume | Treat `fsck_hfs` as an oracle; any required repair is a release blocker. |
| Power loss during commit | Torn metadata | Write-ahead journal, barriers, replay on mount, read-only fallback. |
| Low-battery write | Power loss mid-transaction | Battery/charger gate before a transaction; refuse writes below threshold. |
| Storage resume failure | Dead device | Explicit power state machine; 100-cycle matrix. |
| Memory exhaustion | Audio/UI failure | Hard budgets, bounded caches, no full DB RAM copy by default. |
| Audio regression | Poor product | Codec/DSP tests, device audio matrix. |
| Migration data loss | User state lost | Host exporter, backups, verify-before-switch. |
| Security issue | Malicious file/firmware | Signed manifests, validation, read-only USB, plugin sandbox. |
| Legal/licensing | Distribution issue | GPL compliance, attribution, trademark review. |
| Scope creep | Never ships | First-party feature list, no plugin tree, phased roadmap. |

The two new high-severity rows are the honest cost of native writes: a B-tree
bug and a journal bug are both data-loss bugs. The donor overlay could fail
without touching the music library. Helikon-OS cannot.

---

## 21. What I would absolutely not include

These are not stylistic preferences. Each is a hard exclusion for the new
product unless a later, measured design explicitly reverses it.

1. **Non-transactional native metadata writes.** No catalog, bitmap, extents
   or volume-header change outside a validated transaction. A non-journaled
   writer is allowed only on a disposable prototype volume.
2. **Silent journal bypass.** Never disable, delete or ignore a journaled
   volume’s journal to make a write fit.
3. **A live namespace overlay.** No `/.rockpod-rw`, no shadow namespace, no
   hidden container as the write path.
4. **Format/erase/restore/repair as a firmware action.**
5. **Filesystem writes in the bootloader.** The bootloader is read-only.
6. **`--single` bootloader installation.** It destroys the Apple NOR path.
7. **Writeable USB mass storage on HFS+.** Read-only LUN only.
8. **Zeroing catalog, bitmap or journal metadata to “reset.”** A failed
   transaction must preserve the last consistent state.
9. **Hiding unsupported files from `readdir`.** Expose them and return a
   specific error.
10. **v1/v2/v3 overlay formats in the new core.** A host exporter only.
11. **The legacy tagcache/dircache stack.** New immutable database.
12. **The full `apps/plugins/` tree and raw-hardware plugin API.**
13. **The full Rockbox WPS/theme surface and 133 theme assets.** One new theme
    format, a small default set.
14. **Unsigned firmware or plugin execution.**
15. **Periodic early-boot power callbacks and implicit sleep.**
16. **Cached PMU/backlight state without invalidation.**
17. **Unmeasured overclocking, undervolting or charging changes.**
18. **Battery/power claims without a recorded measurement.**
19. **APFS/HFSX, Apple proprietary blobs, or a FAT32-primary design.**
20. **The entire Rockbox application layer copied “because it works.”**

21. **Writing to a volume that `fsck_hfs` reports dirty or damaged.** The
    volume mounts read-only and asks the user to run macOS repair first.

The full-Rockbox rule is the most important cultural rule. A new firmware that
keeps all of Rockbox has not simplified anything; it has inherited twenty years
of features that were designed for different hardware and different
constraints.

---

## 22. Migration from the current tree

A new firmware must not strand existing users. The migration is from the donor
overlay to ordinary native HFS+ files, not from one overlay format to another.

1. **Read-only audit:** mount the existing v3 overlay with the donor validator
   and produce a manifest of entries, flags, sizes and checksums. Tombstones and
   deleted entries are recorded as such and are not exported as files. `BASE`
   entries point at existing native files and are verified rather than
   duplicated.
2. **Backup:** copy `/.rockpod-rw`, `.rockbox`, settings, playlists and
   database files. Verify the copies.
3. **Export:** a host tool reads the v3 overlay and writes the user files into
   a staging directory on the Mac-visible volume. It never overwrites an
   existing native file silently; a conflict is reported and resolved by the
   user. This is a host-side write, so the donor firmware is not modifying
   native metadata.
4. **Verify:** compare every exported file hash, name and size with the
   manifest. Re-run `diskutil verifyVolume` and `fsck_hfs` on the volume.
5. **Dry run:** boot Helikon-OS read-only and compare the native namespace with
   the export manifest.
6. **Switch:** only after the user confirms boot, playback, settings and a
   reboot, remove the old overlay from the backup workflow.
7. **Rollback:** retain the old firmware and the old container until the
   migration window closes. Never delete the only copy.

The exporter is a host tool, not a bootloader feature. It must run only on a
volume that `fsck_hfs` reports clean, and it must be tested with the same
fixture corpus as the donor C validator.

---

## 23. Phased roadmap

| Phase | Deliverable | Exit criteria |
|---|---|---|
| 0 | Rescue bootloader + native HFS+ read-only firmware skeleton | Boots, loads a verified image, Apple path intact, recovery menu, zero writes. |
| 1 | Read-only player: HAL, HFS+, audio, UI, database | Plays music, browses the library, survives reboot, volume hash unchanged. |
| 2 | Native HFS+ write prototype, non-journaled disposable volume | Create/write/rename/delete/mkdir/truncate; `fsck_hfs` clean; macOS mounts. |
| 3 | Journaled HFS+ write engine | Journal append/replay; power-cut matrix; `fsck_hfs` reports no repairs. |
| 4 | Metadata parity and user state | Timestamps, BSD/Finder info, settings, playlists, resume state; all transactional. |
| 5 | Migration + host tools | Donor overlay exported to native files; hashes match; macOS verifies volume. |
| 6 | USB read-only + diagnostics + update flow | macOS mounts read-only; logs export; update verified and rollback works. |
| 7 | First-party features (PictureFlow, album art) | Feature parity with the product spec, not Rockbox. |
| 8 | Optional plugin sandbox | Capability model, quotas, crash isolation. |
| 9 | Power/thermal/battery tuning | Measured matrix; no unmeasured defaults. |

Phase 2 is a prototype on a volume you can throw away. Phase 3 is the first
phase that may touch a real library, and only after the power-cut matrix passes.
Phases 2–4 are the critical path; UI and feature work must not consume the
testing time they need.

---

## 24. Validation strategy

Host:

- unit tests for every parser, writer and state machine;
- fuzzing for HFS+ B-trees, extents, names and journal records;
- model-based tests for B-tree insert/split/merge/delete;
- property tests for allocation, ordering and commit atomicity;
- crash simulation that cuts power at every journal and metadata boundary;
- differential tests against macOS: create the same tree on both, compare
  catalog order, names, extents and `fsck_hfs` output, and check CNID
  monotonicity and uniqueness rather than exact CNID values;
- deterministic fixtures shared by C and Python validators;
- ASan/UBSan on every host target.

Device:

- cold/warm boot, battery-disconnect boot;
- HDD, iFlash SD, iFlash CF, SSD;
- 100-cycle sleep/wake and resume;
- power-cut during create/append/close/rename/delete/mkdir/truncate/settings/
  database/artwork install;
- after every cut: boot, mount, `fsck_hfs -n`, and a metadata comparison;
- journal replay from a copied disk image before and after mount;
- audio matrix, click wheel, charging, lineout;
- DFU install/uninstall and recovery;
- update/rollback and migration.

Release gate:

- `fsck_hfs` must report **zero repairs** after every crash and power-cut test;
- a single catalog or bitmap inconsistency blocks the release;
- no stable release without a recorded device run;
- no “host tests passed” claim presented as hardware validation;
- no unexecuted stress gate hidden in the notes.

The donor project’s unexecuted P0 power-loss, native-integrity and 100-cycle
gates are prerequisites for the first Helikon-OS write release, not optional
polish.

---

## 25. Open design decisions

1. **Boot image verification:** signature scheme and key storage; how to avoid
   bricking if a key is lost.
2. **Boot state and fallback:** `/.helikon/bootstate` two-slot record vs
   button-only recovery; who is allowed to clear it.
3. **Journal strategy:** journaled HFS+ writes for release vs a non-journaled
   prototype only. The paper recommends journaled writes for any volume that
   contains music the user cares about.
4. **Journal ownership:** replay in Helikon-OS vs refuse writes and require
   macOS to replay. The former is required for a standalone player; the latter
   is a safer first prototype.
5. **B-tree node cache size and transaction dirty-set limit:** how much of the
   64 MiB budget the writer may take before audio and UI suffer.
6. **Unicode policy:** exact bytes vs canonical equivalence; normalization
   table size; how to match macOS case folding exactly.
7. **Database location:** host-built only vs optional on-device build.
8. **Plugin model:** none, capability API, or bytecode sandbox.
9. **FAT fallback:** read-only recovery only, or full read/write fallback.
10. **Theme format:** minimal declarative format vs retained-mode layouts.
11. **Update transport:** USB read-only + macOS staging vs DFU-only.
12. **Product identity:** Helikon-OS is chosen; see Appendix D for the naming
    rule and clearance checklist.

Decision 3 and 4 are the ones that determine whether Helikon-OS is a real
standalone player or a read-only player with a macOS-assisted write mode.
They must be recorded before phase 2 starts, not discovered during phase 3.

---

## 26. A non-coder's build path

This section is written for someone who wants to build, test and own
Helikon-OS without being a C programmer. It is honest about the boundary.

**You cannot compile Helikon-OS today, because Helikon-OS does not exist yet.**
There is no source tree, no bootloader image and no installer. What exists is
the donor tree described in this paper. A non-coder can still do four valuable
things:

1. build and test the donor firmware, so the toolchain and hardware are proven;
2. prepare and verify backups, recovery media and a test device;
3. run the acceptance tests that determine whether a developer's build is safe;
4. manage the assembly plan and refuse a release that skips a gate.

Writing the new kernel, VFS, HFS+ write engine, database or UI is developer work.
With an AI coding agent or a developer, a motivated non-coder can follow this
plan, but should not pretend that copying files is the same as building a
system.

### 26.1 The two tracks

| Track | Who does it | What it proves |
|---|---|---|
| **Donor track** | You, following commands | The Mac, toolchain, device, bootloader and test harness work. |
| **Helikon-OS track** | Developer/AI, reviewed by you | The new modules exist and pass acceptance tests. |

Never mix the two on the same device without a backup and a recovery plan.

### 26.2 What you need

- A Mac with Xcode Command Line Tools and Homebrew.
- The Rockbox ARM toolchain (`arm-elf-eabi-gcc` 9.5.0 or the version the
  project pins).
- Python 3, `make`, `git`, `gh` and a text editor.
- A **test iPod Classic 6G** that you can afford to recover, not your only
  device.
- A known-good Apple firmware/DFU recovery path.
- A verified backup of the music library and `/.rockpod-rw`.
- An iFlash SD/CF, SSD or original HDD, recorded by model.
- Patience. This is a months-scale project, not an afternoon install.

### 26.3 Stage 0 — build the donor, touch nothing

Goal: prove the toolchain and the host tests.

```sh
git clone https://github.com/deandromi/hfsplus-rockboxed-ipod.git
cd hfsplus-rockboxed-ipod
make -C tools/hfsplus test
make -C tools/hfsplus sanitize
python3 tools/hfsplus/tests/check_integration.py
```

Then build the firmware and bootloader in separate directories, following the
donor `shipping.md`. Do not install anything yet. The proof is that the host
tests pass and the images link, not that a device boots.

**You can verify:** test counts, sanitizer output, file sizes, map-file
`MOVE_AREA` headroom.

**A developer must verify:** that the image is the intended commit, that the
bootloader is Thumb, and that no local changes are present.

### 26.4 Stage 1 — recovery and inventory

Goal: make the test device recoverable before any new image is installed.

Checklist:

- record the exact iPod model, revision, capacity and storage adapter;
- record the battery condition and whether the device is chargeable;
- confirm Apple Disk Mode and DFU entry;
- confirm the Apple firmware can be restored if needed;
- back up the music library, `.rockbox`, `/.rockpod-rw` and database files;
- hash the backup and read one file back;
- write down the current firmware and bootloader versions.

**Stop if:** the device is the only copy of the music, the donor overlay is
damaged (it still holds your current settings), the native volume is dirty or
`fsck_hfs` reports damage, or you cannot enter Disk Mode/DFU reliably.

### 26.5 Stage 2 — read-only Helikon-OS prototype

Goal: boot a firmware that can read the native HFS+ library, play audio and
show a UI, with **no writes at all**.

What to copy from the donor:

- S5L8702 HAL and target drivers;
- HFS+ partition scanner and reader;
- selected codecs;
- a minimal UI and input layer;
- serial debug or a read-only log export channel.

What must be new or disabled:

- no overlay;
- no settings writes;
- no database writes;
- no native writes of any kind;
- no plugins.

**Acceptance:** the device boots, lists the library, plays a track, reboots, and
the native volume is unchanged by a before/after hash of the whole disk outside
the bootloader/firmware files.

### 26.6 Stage 3 — native HFS+ write prototype (disposable volume)

Goal: prove catalog, bitmap and extent writes on a volume you can throw away.

**This stage is not allowed on your music library.** Use a spare iPod, a spare
iFlash card or a test volume that contains no unique data. Format that
disposable volume as HFS+ with journaling off in macOS Disk Utility. This is the
only formatting step in the plan, and it is test setup on a volume with no
unique data—not a repair action.

What to use as reference:

- the donor HFS+ reader and its validators;
- the donor B-tree node checks and error taxonomy;
- the donor stage/reason style of diagnostics;
- the macOS `fsck_hfs` and `diskutil verifyVolume` oracles.

What must be new:

- catalog record insert, update and delete;
- B-tree node split, merge and key ordering (`FastUnicodeCompare`);
- CNID allocation and next-CNID update;
- allocation bitmap set/clear and free-block accounting;
- inline and overflow extent allocation;
- file data block allocation, zero-fill and truncate;
- volume-header write count, modify date and alternate-header consistency;
- a non-journaled write path, explicitly marked “prototype only”.

**Acceptance:** on the disposable volume, create, append, close, rename, delete,
mkdir and truncate files. After each operation, unmount, run `fsck_hfs -n`, and
confirm it reports **zero repairs**. macOS must mount the volume and see the same
tree. Reboot the iPod and confirm the tree is still there.

**Stop if:** `fsck_hfs` wants to repair anything, macOS shows a different tree,
or the device needs a second attempt to mount. A prototype that “mostly works”
is not a prototype that is safe to extend.

### 26.7 Stage 4 — journaled HFS+ write engine

Goal: make writes safe on a real journaled library.

What to use as reference:

- the donor journal-presence and dirty-volume checks;
- Apple’s HFS+ journal layout;
- the donor power-loss test discipline, even though the donor used an overlay.

What must be new:

- journal header parse and validation;
- transaction records with sequence numbers and checksums;
- commit, barrier and replay;
- idempotent replay after a cut at every boundary;
- a battery/charger gate before a transaction;
- settings, playlists and resume state stored as ordinary native files,
  written through the same transaction engine.

**Acceptance:** for create, append, close, rename, delete, mkdir and truncate,
cut power at every write boundary. On the next boot the journal replays, the
volume mounts, and `fsck_hfs -n` reports zero repairs. Copy the disk image
before and after replay; the file tree and metadata must match the committed
state. Repeat at least 100 times per operation.

**Stop if:** replay is not idempotent, a cut leaves a directory that macOS
cannot read, or the journal is ever silently ignored.

### 26.8 Stage 5 — database, player and UI

Goal: a usable music player without the legacy app layer.

What to copy:

- codecs and DSP;
- album-art and metadata concepts;
- one font and one icon set.

What must be new:

- immutable database schema and host builder;
- playlist format;
- playback state machine;
- retained UI and theme format.

**Acceptance:** 10,000-track library browses, searches and plays. Boot time,
memory and battery are measured, not guessed. Play counts, ratings and resume
points survive a reboot and are written through the transaction engine.

### 26.9 Stage 6 — USB, updates and migration

Goal: read-only USB, verified firmware updates, and a one-way export from the
donor overlay to native files.

What to copy:

- read-only LUN and RAM-only MBR relabel;
- DFU/NOR knowledge;
- donor overlay format/validator concepts for the exporter.

What must be new:

- signed manifest and update flow;
- host exporter from the donor overlay into native HFS+ directories;
- boot-state record and rollback;
- recovery menu and logs.

**Acceptance:** macOS mounts the volume read-only. A corrupted firmware image is
rejected. The exported library has the same names, sizes and hashes as the
donor manifest, `fsck_hfs` is clean, and the library survives a reboot and a
rollback.

### 26.10 Stage 7 — hardening and power

Goal: prove the system on more than one adapter and more than one revision.

- 100-cycle sleep/wake and resume matrix;
- power-loss matrix for every metadata operation;
- native HFS+ `diskutil verifyVolume` and catalog/bitmap/journal comparison;
- journal replay from copied images;
- audio quality and click-wheel tests;
- thermal and charging checks;
- low-battery write refusal.

No stable release before this stage.

### 26.11 What a non-coder should refuse

- A build with no recorded test run.
- A bootloader install with `--single`.
- A “fix” that formats, erases, restores or deletes the journal or metadata.
- A firmware that writes native metadata outside a transaction.
- A build that disables journaling on a library you care about.
- A write test on the only copy of the music.
- A release that hides unexecuted stress gates.
- A plugin or theme bundle from an untrusted source.

## 27. Assembly order and handoff checklist

This is the order in which the modules should be brought up. Each step has a
proof that does not require reading the code.

| Order | Module | Source | Proof before moving on |
|---:|---|---|---|
| 1 | Toolchain and donor build | donor | Host tests, sanitizer, firmware link, bootloader link. |
| 2 | Recovery and backup | owner | Disk Mode, DFU, verified backup, recovery plan. |
| 3 | Bootloader native HFS+ read-only | donor bootloader + HFS+ reader | Loads a known image, Apple path intact, no write code linked. |
| 4 | HAL and block layer | donor target | Storage read/write/flush on the test device. |
| 5 | HFS+ read-only namespace | donor reader | Library lists, names resolve, unsupported files report a reason. |
| 6 | Minimal player and UI | new + codecs | Playback, seek, next/previous, volume, screen sleep. |
| 7 | HFS+ allocation and catalog writer, non-journaled | new | Disposable volume: create/write/rename/delete/mkdir/truncate; `fsck_hfs` zero repairs; macOS agrees. |
| 8 | Journal and transaction engine | new | Power cut at every boundary; idempotent replay; `fsck_hfs` zero repairs. |
| 9 | Metadata parity and user state | new | Timestamps, BSD/Finder info, settings, playlists, resume survive reboot. |
| 10 | Database and browsing | new | 10k tracks, search, album art, playlists. |
| 11 | USB read-only and logs | donor concepts | macOS mounts read-only, logs export, no host writes. |
| 12 | Update and migration | new | Signed image, rollback, donor overlay export with matching hashes. |
| 13 | Hardening | all | Adapter matrix, power-loss matrix, 100-cycle resume, battery gate. |

**Handoff package for each step:** interface document, test fixture, expected
error codes, known limitations, and a rollback method. A module without a
rollback method is not ready.

## Appendix A — Custom file ledger

Line counts are from the audit tree; they will drift.

| File | Lines | Verdict | Keep / change / drop |
|---|---:|---|---|
| `bootloader/ipod-s5l87xx.c` | 1,016 | M | Keep bring-up/DFU; rewrite boot flow. |
| `firmware/hfsplus/hfsplus_ro.c` | 1,274 | K/M | Keep parser; split; add fuzz/normalization/extents policy. |
| `firmware/hfsplus/hfsplus_ro.h` | 163 | K/M | Keep contract; narrow. |
| `firmware/hfsplus/hfsplus_partition.c` | 296 | K | Keep scanner; expand malformed tests. |
| `firmware/hfsplus/hfsplus_io.c` | 220 | K/M | Keep sector I/O; one block API. |
| `firmware/hfsplus/hfsplus_overlay.h` | 145 | K | Keep v3 layout for migration/export only. |
| `hfsplus_overlay_format.c` | 351 | K/M | Keep v3 parse for the host exporter; no runtime writer. |
| `hfsplus_overlay_load.c` | 256 | X | Donor only; no runtime overlay loader. |
| `hfsplus_overlay_alloc.c` | 399 | X | Donor only; no overlay allocator. |
| `hfsplus_overlay_store.c` | 367 | X | Donor only; no overlay entry store. |
| `hfsplus_overlay_validate.c` | 225 | K/M | Keep validator for the exporter; share fixtures. |
| `hfsplus_namespace.c` | 686 | R | Rewrite as native-only lookup/iterate; delete the overlay merge. |
| `hfsplus_fat.c` | 377 | M | Thin fs_ops adapter. |
| `hfsplus_fat.h` | 109 | M | Error/capability mapping. |
| `hfsplus_mount.c` | 374 | M | Native-only bootloader; transaction-aware mount. |
| `hfsplus_index_cache.c` | 62 | M | Keep the cache idea for native directories; no persisted overlay index. |
| `hfsplus_diag.c` | 161 | K/M | Keep counters; new event log. |
| `hfsplus_state.h` | 108 | M | New state model. |
| `hfsplus_time.c` | 40 | K | Keep conversion. |
| `tools/hfsplus/tests/*` | ~2,958 | K | Keep and extend. |
| `tools/rockpod/*` | ~1,036 | M/R | New schema/exporter tools. |
| `utils/mks5lboot/*` | ~3,382 | K/M | Keep DFU; harden verification. |

### A.1 Complete HFS+ module ledger

| File | Lines | Verdict | Exact treatment |
|---|---:|---|---|
| `hfsplus_ro.c` | 1,274 | K/M | Keep parser/validators; split into volume, btree, catalog, extents, unicode, read modules. |
| `hfsplus_ro.h` | 163 | K/M | Keep data contracts; reduce public surface. |
| `hfsplus_partition.c` | 296 | K | Keep APM/MBR/GPT/raw scan; expand malformed tests. |
| `hfsplus_partition.h` | 16 | K | Keep. |
| `hfsplus_io.c` | 220 | K/M | Keep sector I/O; route through new block API. |
| `hfsplus_io.h` | 20 | K/M | Keep as internal contract only. |
| `hfsplus_overlay.h` | 145 | K | Keep v3 layout for the host exporter and diagnostics only. |
| `hfsplus_overlay_format.c` | 351 | K/M | Keep v3 parse/sync/checksum for the exporter; no live writer. |
| `hfsplus_overlay_format.h` | 31 | K/M | Keep v3 entry encode/decode contract for the exporter. |
| `hfsplus_overlay_load.c` | 256 | X | Donor only; no runtime overlay loader. |
| `hfsplus_overlay_load.h` | 11 | X | Do not carry into the new core. |
| `hfsplus_overlay_alloc.c` | 399 | X | Donor only; no overlay allocator. |
| `hfsplus_overlay_alloc.h` | 19 | X | Do not carry into the new core. |
| `hfsplus_overlay_store.c` | 367 | X | Donor only; no overlay entry store. |
| `hfsplus_overlay_store.h` | 26 | X | Do not carry into the new core. |
| `hfsplus_overlay_validate.c` | 225 | K/M | Keep reason/slot model for the exporter; share fixtures. |
| `hfsplus_namespace.c` | 686 | R | Native-only lookup/iterate; never skip unsupported entries. |
| `hfsplus_namespace.h` | 12 | M | New namespace API. |
| `hfsplus_fat.c` | 377 | M | Thin fs_ops adapter; keep diagnostics/error mapping. |
| `hfsplus_fat.h` | 109 | M | Capability/error helpers; remove policy leakage. |
| `hfsplus_mount.c` | 374 | M | Native-only bootloader path; transaction-aware firmware mount. |
| `hfsplus_index_cache.c` | 62 | M | Keep the cache idea for native directories; no overlay index/checkpoint. |
| `hfsplus_index_cache.h` | 10 | M | New API. |
| `hfsplus_diag.c` | 161 | K/M | Keep counters/stage reasons; new event log. |
| `hfsplus_diag.h` | 11 | K/M | Extend, do not renumber. |
| `hfsplus_state.c` | 10 | M | New per-volume state model. |
| `hfsplus_state.h` | 108 | M | Split bootloader/firmware state; no overlay state in boot. |
| `hfsplus_time.c` | 40 | K | Keep HFS+/FAT timestamp conversion. |
| `hfsplus_time.h` | 13 | K | Keep. |

### A.2 Complete iPod 6G target ledger

| File | Lines | Verdict | Exact treatment |
|---|---:|---|---|
| `storage_ata-6g.c` | 1,406 | K/M | Keep transport/timing; replace power/sleep/flush policy. |
| `pmu-6g.c` | 492 | K | Keep PMU/charger/backlight register sequences. |
| `power-6g.c` | 237 | K | Keep power-off/reboot/charger detection. |
| `lcd-6g.c` | 266 | K | Keep panel/controller init. |
| `serial-6g.c` | 284 | K | Keep debug serial. |
| `audio-6g.c` | 140 | K | Keep I2S/routing. |
| `piezo-6g.c` | 125 | K | Keep clicker. |
| `adc-6g.c` | 117 | K | Keep battery/accessory sampling. |
| `powermgmt-6g.c` | 75 | K | Keep battery curves; new policy above. |
| `rtc-6g.c` | 75 | K | Keep RTC. |
| `backlight-6g.c` | 74 | K/M | Keep PMU writes; new state/invalidation policy. |
| `cscodec-6g.c` | 66 | K | Keep codec control. |
| `adc-target.h` | 43 | K | Keep. |
| `backlight-target.h` | 31 | K | Keep. |
| `button-target.h` | 76 | K | Keep key map. |
| `lcd-target.h` | 35 | K | Keep. |
| `mmcdefs-target.h` | 340 | K | Keep. |
| `piezo.h` | 36 | K | Keep. |
| `pmu-target.h` | 130 | K | Keep. |

### A.3 Bootloader and tooling ledger

| Group | Files | Lines | Verdict | Treatment |
|---|---:|---:|---|---|
| `bootloader/ipod-s5l87xx.c` | 1 | 1,016 | M | Keep bring-up/DFU; rewrite boot flow. |
| Other `bootloader/*` | 40 | 11,355 | K/X | Keep shared loader/format helpers; exclude non-iPod targets. |
| `utils/mks5lboot/` | 15 | 3,382 | K/M | Keep DFU/NOR knowledge; harden verification. |
| `tools/hfsplus/` | 20 | 2,958 | K | Keep inspector/tests/fixtures. |
| `tools/rockpod/` | 6 | 1,036 | M/R | New schema and donor-overlay exporter. |
| `tools/database/` | 3 | 137 | R | New database builder. |
| `tools/scramble`, `bmp2rb` | — | — | K | Keep build/image tools. |
| `tools/xerj/` | — | — | X | Optional research index, not product. |

### A.4 Inherited module ledger, grouped

| Group | Files | Lines | Verdict | Treatment |
|---|---:|---:|---|---|
| `apps/plugins/` | 2,602 | 946,251 | X | No plugin tree in v1. |
| `lib/rbcodec/` | 1,193 | 350,947 | K/M | Select codecs/DSP; drop unused. |
| `apps/` core hot spots | — | ~30,000 | M/R | New player/browser/UI. |
| `apps/gui/` | 52 | 20,127 | M/R | New retained UI. |
| `apps/recorder/` | 19 | 13,696 | M/R | Keep audio routing concepts; new UI. |
| `firmware/common/` | 39 | 18,420 | M/R | New block/VFS; keep disk/mount knowledge. |
| `firmware/drivers/` | 110 | 34,679 | K/M | Keep audio/storage primitives; new policy. |
| `firmware/kernel/` | 20 | 4,739 | K | Keep scheduler/queues/mutexes; simplify. |
| `firmware/target/arm/s5l8702/` | 85 | 15,616 | K | Keep HAL. |
| `themes/` | 133 | 55,636 | X/select | One default theme. |
| `wps/` | 245 | 80,427 | X | No legacy skin language. |
| `fonts/` | 90 | large | K/select | Two or three fonts. |
| `icons/` | 58 | 21,566 | K/select | One icon set. |
| `docs/` | 49 | 11,505 | K/M | Keep invariants, audit, validation. |

### A.5 Line-budget projection

These are design targets, not promises:

| New component | Target lines | Donor | Note |
|---|---:|---|---|
| Bootloader | 1,200–1,800 | `ipod-s5l87xx.c`, shared boot | Smaller, verified, native-only. |
| HAL/target | ~3,500 | `ipod6g/` | Keep hardware, new interfaces. |
| HFS+ reader | ~2,500 | `hfsplus_ro.c` | Split and fuzz. |
| HFS+ catalog/bitmap/extents writer | ~4,000–6,000 | new; donor validators | Catalog, bitmap, extents, CNID, metadata parity. |
| Journal/transaction + boot state | ~1,500–2,500 | new; journal format | Append, commit, replay, abort, barriers, boot record. |
| VFS/block | ~1,500 | `fat.c`, `file.c` | Clean interfaces. |
| Database | ~1,500 | host builder concept | Immutable, no dircache. |
| Player/audio | ~3,000 | selected Rockbox codecs | New state machine. |
| UI | ~4,000 | new | First-party screens. |
| USB/diagnostics | ~1,500 | USB/diag concepts | Read-only, structured logs. |
| Host tools | ~5,000 | `tools/hfsplus`, `tools/rockpod` | New schema/exporter. |
| **Product core** | **~30,000–37,000** | — | Excludes selected codecs and assets. |

The current inherited app/plugin tree is roughly **2.7 million lines**. A product
core in the tens of thousands of lines is the difference between a new firmware
and a renamed fork.

### A.6 Function-level audit: `hfsplus_ro.c`

| Function group | Verdict | Treatment |
|---|---|---|
| Volume-header parse, alternate-header fallback, journal detection | K | Keep; split into `hfs_volume.c`; add fuzz. |
| Fork/extent parse, inline check, overlap/bounds checks | K | Keep; expand extents-overflow tests. |
| B-tree node descriptor/offset-table validation | K | Keep; make a generic, fuzzable `hfs_btree.c`. |
| Catalog record parse (folder/file/thread), name decode | K/M | Keep; make unsupported reasons explicit. |
| Directory index build and binary search | K/M | Keep concept; new index cache format. |
| CNID index and `find_id` | K/M | Keep; split from catalog iteration. |
| Iterator init/next and leaf-chain validation | K | Keep; no silent skipping. |
| `hfs_ro_lookup` path walk | M | Reuse exact-byte semantics; move to VFS lookup API. |
| Extents-overflow collection/read | K/M | Keep read-only; raise/remove 128 limit only with measurement. |
| `hfs_ro_pread` | K/M | Keep bounds/clipping; new block API; explicit errors. |
| Error strings | K | Keep, extend. |

### A.7 Function-level audit: `bootloader/ipod-s5l87xx.c`

| Function group | Verdict | Treatment |
|---|---|---|
| CPU/clock/PMU/GPIO/I2C/SPI bring-up | K | Keep. |
| LCD/button/backlight recovery UI | K/M | Keep minimal text UI. |
| Battery trap / charger checks | K | Keep; add safe power-off path. |
| `storage_init`, `filesystem_init`, `disk_mount_all` | K/M | Keep storage; native-only mount. |
| HFS+ partition scan/mount | K | Reuse. |
| `load_firmware`/image jump | M/R | Add manifest/hash/board/size checks. |
| DFU/USB bootloader mode | K/M | Keep recovery; no write path. |
| `fatal_error` screens | K/M | Keep codes; add recovery instructions. |
| norboot/Apple selector | K | Preserve; never `--single`. |
| Overlay and write references | X | Remove entirely; the bootloader is read-only. |

### A.8 Function-level audit: `storage_ata-6g.c`

| Function group | Verdict | Treatment |
|---|---|---|
| ATA identify, LBA28/LBA48 setup | K | Keep. |
| PIO/DMA transfer, CE-ATA command path | K | Keep. |
| 512/4096 logical/physical translation | K | Keep; test both. |
| Power-up, clock gate, deep sleep, standby | K/M | Keep primitives; new explicit state machine. |
| `ata_spin`, `ata_sleep`, `ata_sleepnow` | M/R | New policy; no implicit sleep, no early callbacks. |
| Flush/cache handling | K/M | Keep command; return real errors/barriers. |
| Storage event handling | M | New event queue tied to state machine. |
| SSD/HDD detection and mode switch | K/M | Keep detection; new safety gates. |

### A.9 New modules with no donor file

These are the modules Helikon-OS must write from scratch. They have no donor
implementation because the donor never wrote native HFS+.

| New module | Target lines | Responsibility |
|---|---:|---|
| `hfs_btree_write.c` | 1,500–2,200 | Node split/merge, key insert/delete, free-node map, leaf links. |
| `hfs_catalog_write.c` | 1,000–1,500 | Folder/file/thread records, CNID allocation, rename/delete. |
| `hfs_bitmap.c` | 500–800 | Allocation bitmap, free-block accounting, volume-header counts. |
| `hfs_extents_write.c` | 700–1,000 | Inline and overflow extent allocation and truncation. |
| `hfs_txn.c` | 600–900 | Dirty-block set, ordering, abort, commit. |
| `hfs_journal.c` | 900–1,400 | Journal header, records, checksums, replay, barriers. |
| `hfs_bootstate.c` | 150–250 | Two-slot boot-state record, read-only from the bootloader. |
| `hfs_fsck_compat.c` | 300–500 | Invariant checks that mirror `fsck_hfs` rules and explain failures. |

These are the real cost of “straight writable HFS+.” The donor overlay engine was
roughly two thousand lines; the native writer is several times that, plus the
journal.

## Appendix B — Inherited hotspot ledger

| Path | Lines | Verdict | Reason |
|---|---:|---|---|
| `apps/plugins/` | 946,251 | X | Enormous surface, raw hardware API, not product core. |
| `lib/rbcodec/` | 350,947 | K/M | Select codecs; keep DSP; drop unused. |
| `apps/tagcache.c` | 5,756 | R | Legacy database/queue/dircache complexity. |
| `apps/playback.c` | 4,363 | R | New player state machine. |
| `apps/playlist.c` | 4,154 | R | New playlist format. |
| `apps/debug_menu.c` | 3,141 | M | Keep diagnostics; split UI. |
| `firmware/common/fat.c` | 3,187 | R | FAT-shaped VFS. |
| `apps/tagtree.c` | 2,975 | R | New browser. |
| `apps/settings_list.c` | 2,468 | M/R | Keep settings, new storage. |
| `apps/settings.c` | 1,511 | R | New config engine. |
| `firmware/common/file.c` | 1,433 | R | New VFS/stream cache. |
| `firmware/target/.../storage_ata-6g.c` | 1,406 | K/M | Keep transport, new power policy. |
| `apps/playlist_viewer.c` | 1,341 | R | New UI. |
| `firmware/backlight.c` | 1,090 | M | New policy/state machine. |
| `firmware/storage.c` | 852 | M | New state/event model. |
| `apps/main.c` | 833 | R | New init sequence. |
| `firmware/common/file_internal.c` | 792 | R | New path walk. |
| `firmware/common/disk.c` | 744 | M | New mount integration. |
| `firmware/common/fileobj_mgr.c` | 451 | R | New handle/cache model. |
| `apps/recorder/albumart.c` | 435 | M | Keep cache idea; new policy. |
| `apps/gui/` | 20,127 | M/R | New retained UI. |
| `wps/` | 80,427 | X | Legacy skin language. |
| `themes/` | 55,636 | X | Select one default theme. |
| `fonts/` | large | K/select | Two or three fonts. |
| `icons/` | 21,566 | K/select | One icon set. |

## Appendix C — Verdict summary

**Keep:** S5L8702 HAL, ATA transport, partition scanner, HFS+ reader and
validators, donor overlay format/validator for migration and export only,
DFU/NOR knowledge, host tests, selected codecs, one font/icon/theme set,
documentation invariants.

**Modify:** boot flow, HFS+ namespace (remove the overlay merge), FAT adapter,
diagnostics, settings storage, USB descriptors, debug UI, build system,
migration exporter.

**Replace:** VFS/file layer, tagcache/tagtree, playlist storage, playback state
machine, UI framework, database schema. **Build new:** the HFS+ catalog/bitmap/
extents writer, journal and transaction engine.

**Exclude:** the overlay as a live write path, journal bypass or silent disable,
native metadata writes outside a transaction, overlay or write code in the
bootloader, `--single`, writeable USB, zeroing metadata to reset, silent
unsupported-file hiding, live legacy overlay formats, tagcache/dircache, the
full plugin tree, the raw plugin hardware API, legacy WPS/themes, unsigned
updates, periodic early-boot power callbacks, unmeasured power/charging changes,
and the full Rockbox application copy.

## Appendix D — Chosen name: Helikon-OS

**Decision: Helikon-OS. Use it for everything.**

Helikon is the Greek *Ἑλικών*: Mount Helicon, the mountain of the Muses and the
Hippocrene spring, the classical source of poetic and musical inspiration.
“Helicon” is also a brass instrument, so the name is musical twice over. “OS”
makes the system identity explicit and leaves no ambiguity about what the
project is.

The name is used consistently for:

- the project and repository identity;
- the bootloader and recovery environment;
- the firmware and player;
- the UI, theme and documentation;
- host tools, manifests, package names and release titles.

No separate component codenames are needed. “Helikon-OS” is the product; a
component is described as “the Helikon-OS bootloader”, “the Helikon-OS HFS+
engine” or “the Helikon-OS host tools.”

Before public use, verify:

- trademark and domain availability;
- GitHub organization/repository availability;
- package and binary naming;
- boot-screen and manifest strings;
- that no existing audio or operating-system product has a conflicting claim.

The name must not inherit “Rockpod”, “Rockbox” or “HFS+” in public product
identity. Those remain upstream/technical lineage terms where attribution
requires them, not the name of the new system.

---

## Appendix E — Plain-language component catalogue

This appendix is the “what do I actually do with this file?” list. It is
organized by action rather than by directory, so a non-coder can follow it.

**Legend**

| Tag | Meaning |
|---|---|
| **Copy** | Take the code or data as-is. It encodes hardware or format knowledge that is expensive to rediscover. |
| **Copy then adapt** | Keep the logic, but change the interface, policy or storage model. |
| **Reference** | Do not copy the code. Use it to understand the behaviour or format, then write a new implementation. |
| **Remove** | Do not put it in Helikon-OS. It is unsafe, obsolete or product-wrong. |

### E.1 Green list — copy exactly

| Component | What it does in plain English | Current files | Why copy | If omitted |
|---|---|---|---|---|
| S5L8702 clock and PLL setup | Wakes the CPU and sets its operating frequencies. | shared `s5l8702/` clocking | Wrong values can hang or damage the device. | Device may not boot reliably. |
| GPIO, I2C and SPI | Talks to the PMU, codec, LCD and other chips. | shared `s5l8702/` | Board-specific pin and bus knowledge. | No display, audio, charging or buttons. |
| ATA transport | Reads and writes disk sectors. | `storage_ata-6g.c` | LBA48, timing, DMA and CE-ATA details. | No filesystem. |
| PMU and charger | Controls voltages, charging, backlight and power events. | `pmu-6g.c`, `power-6g.c` | Wrong sequencing can brown out or damage the device. | No safe charging or power-off. |
| LCD panel init | Brings up the 320×240 display. | `lcd-6g.c` | Panel timings and controller setup. | No visible UI. |
| Click wheel and buttons | Reads the wheel, buttons and hold switch. | `button-target.h`, target input code | Hardware timing and key map. | No input. |
| Audio codec and I2S | Produces sound through the CS42L55. | `audio-6g.c`, `cscodec-6g.c` | Codec register and clocking knowledge. | No audio. |
| ADC, RTC and piezo | Battery/accessory sensing, clock and clicker. | `adc-6g.c`, `rtc-6g.c`, `piezo-6g.c` | Board calibration and register details. | Wrong battery data, no clock/click. |
| Serial debug | Early boot logs over serial. | `serial-6g.c` | The only low-level debug path. | Blind bring-up. |
| Bootloader hardware bring-up | Initialises the board before loading firmware. | `bootloader/ipod-s5l87xx.c` | The first code that runs. | No boot. |
| DFU and NOR knowledge | Installs and recovers the bootloader. | `utils/mks5lboot/` | Bricking risk if reinvented. | No safe recovery. |
| HFS+ partition scanner | Finds the HFS+ volume in APM/MBR/GPT/raw layouts. | `hfsplus_partition.c` | Handles real iPod partition tables. | Volume not found. |
| HFS+ volume/header validation | Checks block size, bounds, journal presence and alternate header. | `hfsplus_ro.c` | Prevents mounting corrupt or dirty volumes. | Data-loss risk. |
| HFS+ journal detection | Sees whether the volume is journaled, clean or dirty. | `hfsplus_ro.c` | A write engine must never mistake a dirty journal for a clean one. | Writes on an unrecovered volume. |
| Catalog B-tree parser | Translates catalog nodes into folders, files and names. | `hfsplus_ro.c` | HFS+ is not a simple directory format. | No names or lookup. |
| Extent parser and overflow reader | Maps logical file blocks to disk blocks. | `hfsplus_ro.c` | Large/fragmented files need this. | Plugin data files disappear. |
| Host HFS+ tests and fixtures | Proves the parser without a device. | `tools/hfsplus/tests/` | Fast, repeatable safety net. | Every change is a device gamble. |
| HFS+/FAT timestamp conversion | Converts dates between formats. | `hfsplus_time.c` | Small, exact and easy to get wrong. | Wrong dates and metadata. |
| Selected codecs and DSP | Decodes and processes audio. | `lib/rbcodec` | Years of codec work. | No playback. |
| One font and icon set | Gives the UI a consistent look. | `fonts/`, `icons/` | Cheap, proven assets. | UI looks unfinished. |

### E.2 Yellow list — copy then adapt

| Component | What it does | Current files | What to change | Why not copy as-is |
|---|---|---|---|---|
| Bootloader main flow | Finds storage, mounts, loads firmware and jumps. | `ipod-s5l87xx.c` | Native-only read, manifest/hash check, boot-state read, recovery menu, no write engine. | Current flow trusts an unsigned image and carries too much. |
| HFS+ reader module split | One large parser file. | `hfsplus_ro.c` | Split volume, B-tree, catalog, extents, Unicode, read. Expose node/record layouts the writer can reuse. | 1,274 lines in one file is hard to fuzz and review. |
| HFS+ namespace lookup | Finds names and walks directories. | `hfsplus_namespace.c` | Native-only lookup/iterate; delete the overlay merge, tombstones and shadowing. | Exact-byte lookup is not enough for writes; catalog order uses HFS+ case folding. |
| Donor overlay exporter | Reads old overlay data for migration. | `hfsplus_overlay.h`, `hfsplus_overlay_format.c`, `hfsplus_overlay_validate.c` | Host-side export only; no runtime loader or writer. | The overlay is not the live filesystem in Helikon-OS. |
| Diagnostics vocabulary | Reports stage, error, slot and reason. | `hfsplus_diag.c` | Keep the idea; rename stages for mount, transaction and journal replay. | Useful discipline, but the donor stages describe overlay loading. |
| FAT adapter | Maps FAT-style calls to HFS+. | `hfsplus_fat.c`, `hfsplus_fat.h` | Thin `fs_ops` adapter only; `WRITE` maps to a native transaction. | It leaks filesystem policy into generic code. |
| Settings | Stores user preferences. | `settings.c`, `settings_list.c` | New schema, two checksummed native files, transactional promotion. | Current storage is callback-heavy and deferred. |
| Debug menu | Shows disk, overlay and system diagnostics. | `debug_menu.c` | Split into small panels: disk, filesystem, journal, transaction, boot state. | 3,141 lines is too much for a diagnostic screen. |
| USB LUN and MBR relabel | Presents a read-only HFS+ view to macOS. | USB storage code | Keep read-only, explicit descriptors, RAM-only relabel. | The relabel must never be persisted to the partition table. |
| Album-art cache | Avoids repeated folder-art probes. | `albumart.c` | One directory cache, explicit invalidation after writes. | Negative caching can go stale. |
| Build system | Configures and builds Rockbox. | `tools/configure`, Makefiles | Add reproducible builds, size budgets, signed manifests, fuzz targets. | Current build is tied to the old product shape. |
| Host tools | Build database, artwork and overlay exports. | `tools/rockpod/` | New database schema and a donor-overlay exporter to native files. | Old tools write old formats. |
| Documentation | Explains install, safety and validation. | `README`, `shipping.md`, docs | Rename, simplify, keep invariants, add journal/fsck gates. | Docs drift faster than code. |
| ATA power and flush policy | Sleeps, wakes and flushes the disk. | `storage_ata-6g.c` | Explicit state machine; a flush error must fail the transaction. | Ad-hoc policy caused freezes and dead sleep. |
| Backlight/PMU policy | Turns the screen on/off and caches PMU state. | `backlight.c`, `backlight-6g.c` | Invalidate state on every transition. | Stale cache can leave the screen dark. |

### E.3 Orange list — use as reference, then reimplement

| Component | Why not copy | What to build instead |
|---|---|---|
| VFS and file cache | FAT-shaped, global state, HFS+ special cases. | Clean block/VFS/handle API with capabilities. |
| HFS+ B-tree mutation | No donor implementation exists. | Insert/update/delete with split/merge, free-node map and Apple key ordering. |
| Allocation bitmap writer | No donor implementation exists. | Set/clear blocks, update free counts and keep extents consistent. |
| Extents overflow writer | Donor only reads it. | Insert/update/delete overflow records and truncate correctly. |
| Journal and transaction engine | Donor only detects and refuses a dirty journal. | Append, commit, barrier, replay and abort with checksums. |
| Donor overlay allocator/store/load | Bitmap/entry/data ordering is hard to reason about. | Read it for commit-order ideas; do not port the engine. |
| Tagcache and tagtree | 8,700+ lines of cache, changelog, dircache and cursor logic. | Immutable host-built database and a small browser. |
| Playback state machine | 4,363 lines tied to legacy buffering and events. | Smaller buffer/decoder/output state machine. |
| Playlist storage | Legacy control-file machinery. | Versioned playlist file written through the transaction engine. |
| Settings engine | Huge table and callback system. | New schema and two-slot config store. |
| UI/GUI/WPS | 20k GUI lines plus 80k WPS assets. | Retained-mode screens and one theme format. |
| Plugin API | Raw hardware and storage access. | No plugins in v1, capability API later. |
| Database builder | Old schema and tagcache assumptions. | New schema, host-first, checksummed. |
| Update flow | Unsigned image and fixed jump. | Manifest, hash, signature, boot-state and rollback policy. |
| Power policy | Unmeasured states and early-boot callbacks. | Explicit state machine with measured transitions and a battery write gate. |
| Theme/asset pipeline | 133 themes and a large skin language. | One default theme, selectable fonts/icons. |

### E.4 Red list — remove from Helikon-OS

| Current thing | Why it must go | What replaces it |
|---|---|---|
| Overlay-only write model | A shadow namespace hides what the device actually wrote and cannot be read by macOS. | Native HFS+ transactions; the volume is the truth. |
| Journal bypass or silent disable | Writing a journaled volume without its journal corrupts metadata. | Replay the journal, or mount read-only. |
| Native metadata writes outside a transaction | A torn catalog or bitmap update can lose files. | One journaled transaction per operation. |
| Raw block writes from apps or plugins | Bypasses the filesystem, journal and bounds checks. | Only the HFS+ engine writes; apps use the VFS. |
| Overlay or write code in the bootloader | Adds size, risk and a write path before the OS. | Read-only rescue bootloader. |
| `--single` installation | Destroys the Apple NOR boot path. | Dual-boot installer only; recovery preserved. |
| Writeable USB mass storage | Host writes bypass the transaction engine and crash-consistency model. | Read-only LUN; Apple Disk Mode or the device owns writes. |
| Silent fallback to read-only | Hides a failed mount and looks like data loss. | Explicit `READ_ONLY`/`JOURNAL_DIRTY` status on screen and in logs. |
| Zeroing metadata to reset | Destroys catalog, bitmap or journal state to “fix” a mount. | Fail closed; repair on macOS with `fsck_hfs`. |
| Auto-repair during mount | An embedded repairer can make a bad volume worse. | Mount read-only and report the exact error. |
| Silent unsupported-file skipping | Makes real files look missing. | List the entry and return a specific error. |
| Live donor overlay formats in the core | Legacy formats multiply bugs and tests. | Host exporter only. |
| Tagcache and dircache | Huge, legacy, redundant with a new database. | Immutable database and direct lookup. |
| Full plugin tree | 946k lines, raw hardware, security and size risk. | First-party features; sandbox later. |
| Raw plugin hardware API | A plugin can brick the device or corrupt storage. | Capability API only, or no plugins. |
| Legacy WPS/themes | 80k lines of skin language and assets. | One new theme format. |
| Unsigned firmware/plugins | No integrity or trust boundary. | Signed manifest and explicit trust. |
| Periodic early-boot callbacks | Caused a 4–6 second freeze in the donor. | Explicit state machine and idle policy. |
| Cached PMU/backlight state without invalidation | Can leave the device dark or dead after sleep. | Invalidate on every transition. |
| Unmeasured overclock/undervolt/charging | Can damage hardware or battery. | Measured, documented policies only. |
| APFS/HFSX/FAT32-primary design | Outside the product’s filesystem contract. | HFS+ native, FAT read-only fallback if needed. |
| Full Rockbox application layer | Inherits 20 years of unrelated features. | Product-specific player/UI. |
| On-device full database rebuild in v1 | Slow, risky and unnecessary for first boot. | Host-built database; on-device later. |
| Unbounded dynamic caches | 64 MiB RAM cannot absorb them. | Fixed budgets and eviction policies. |

### E.5 Copy order if you are assembling the donor knowledge

1. Host tests and fixtures.
2. S5L8702 clock/GPIO/I2C/SPI.
3. PMU, power, LCD, buttons, audio, ADC/RTC.
4. ATA transport.
5. Partition scanner.
6. HFS+ volume/header validation and journal detection.
7. Catalog B-tree, extents and Unicode decode.
8. Bootloader hardware bring-up and DFU.
9. Selected codecs/DSP.
10. Read-only USB and diagnostics.
11. Donor overlay format/validator for the host exporter only.
12. Fonts/icons and documentation.

There is no donor native HFS+ writer. The catalog, bitmap, extents and journal
modules are written from the HFS+ specification and validated against macOS.
This is a knowledge-copy order, not a build order. Build order is in §27.

## Appendix F — Glossary

| Term | Plain-language meaning |
|---|---|
| **Bootloader** | The small program that runs first, finds the firmware and starts it. |
| **Boot state** | A small native file that tells the bootloader which image last booted and whether it passed its check. |
| **Firmware** | The operating system and drivers that run the player. |
| **HAL** | Hardware abstraction layer. A clean interface over registers and chips. |
| **Block device** | A device that reads and writes fixed-size sectors or blocks. |
| **VFS** | Virtual filesystem. The common file interface used by applications. |
| **HFS+** | Apple’s hierarchical file system used by the iPod. |
| **Catalog** | The HFS+ database of files, folders and names. |
| **B-tree** | The balanced tree HFS+ uses for the catalog and extents overflow file. |
| **Catalog node** | One fixed-size block of the catalog B-tree. |
| **CNID** | Catalog node ID. HFS+’s permanent number for a file or folder; the root is 2. |
| **Allocation bitmap** | The map of which disk blocks are in use. |
| **Extent** | A contiguous run of disk blocks belonging to a file. |
| **Extents overflow** | Extra extent records for files with more than eight inline runs. |
| **Journal** | A log of pending metadata changes that makes HFS+ writes crash-consistent. |
| **Journal replay** | Re-applying committed journal records after an interrupted write. |
| **Journaled volume** | An HFS+ volume with a journal. Helikon-OS must replay or refuse writes. |
| **Transaction** | A group of metadata changes that becomes visible together or not at all. |
| **Commit** | The moment a transaction is durable and visible. |
| **Native metadata** | Apple’s catalog, bitmap, extents tree, volume header and journal. |
| **Overlay (donor)** | The old `/.rockpod-rw` container. Helikon-OS uses it only for migration. |
| **Container (donor)** | The preallocated HFS+ file that held the donor overlay. |
| **Superblock (donor)** | The header that described the donor overlay’s layout. |
| **Compaction (donor)** | Copying live donor overlay records into a fresh area. |
| **Case folding** | Apple’s rule for treating names that differ only by case as the same key. |
| **Normalization** | Converting Unicode names to a canonical form. HFS+ stores decomposed names. |
| **`fsck_hfs`** | macOS’s HFS+ consistency checker. Helikon-OS treats it as the external oracle. |
| **Checksum** | A value used to detect corruption. |
| **DFU** | Device Firmware Upgrade. The USB recovery/install mode. |
| **NOR** | The flash chip that holds the early bootloader/Apple boot path. |
| **MOVE_AREA** | The memory window the bootloader must fit into. |
| **Cross-compiler** | A compiler that runs on the Mac but produces ARM code. |
| **Toolchain** | Compiler, linker, assembler and related build tools. |
| **Host test** | A test that runs on the Mac, not the iPod. |
| **Sanitizer** | A compiler mode that catches memory and undefined-behaviour bugs. |
| **Fuzzing** | Feeding random/malformed input to find crashes. |
| **Manifest** | A small file describing a firmware image, version, size and hashes. |
| **Signature** | A cryptographic proof that an image came from a trusted builder. |
| **LUN** | A USB mass-storage logical unit. Helikon-OS exposes it read-only. |
| **Dircache** | A legacy Rockbox directory cache. Helikon-OS does not use it. |
| **Tagcache** | A legacy Rockbox metadata database. Helikon-OS replaces it. |
| **BSS** | Zero-initialised global memory in a firmware image. |
| **DRAM** | The iPod’s 64 MiB main memory. |

## Appendix G — FAQ

**Can I build Helikon-OS today?**<br>
No. Helikon-OS is a design, not a source tree. You can build and test the donor
project today and follow the assembly plan.

**Do I need to be a programmer?**<br>
To write the new kernel, VFS, HFS+ write engine or UI, yes, or you need an
AI/developer. A non-coder can manage the build, backups, device tests,
acceptance gates and release checklist.

**Will it run on my iPod?**<br>
The first target is the iPod Classic 6G. Other S5L8702 iPods are future work.

**Will it erase my music?**<br>
It can, if the write engine has a bug. That is why the plan separates a
non-journaled prototype on a disposable volume from journaled writes on a real
library. Every experimental build still needs a verified backup. Do not test on
an only-copy library.

**Can I keep HFS+?**<br>
Yes. That is the point. Native HFS+ is the writable filesystem. Files the device
creates are ordinary HFS+ files, visible to macOS, not hidden in an overlay.

**Will macOS still read the volume?**<br>
That is a release gate. Every crash test must leave a volume that `fsck_hfs`
reports clean and macOS mounts. If macOS disagrees with the device, the build is
wrong.

**Can I still boot Apple firmware?**<br>
Yes, if the Apple NOR path is preserved. Never use `--single`.

**Will my Rockbox plugins work?**<br>
Not in v1. The plugin API is deliberately absent until a capability sandbox
exists. PictureFlow, album art and database browsing become first-party
features.

**Why not FAT32?**<br>
FAT32 conversion is inconvenient and loses the Mac-native HFS+ library.
Helikon-OS is for people who want to stay on HFS+.

**Does it need journaling?**<br>
For a library you care about, yes. The release design writes journaled HFS+.
A non-journaled volume is a prototype path on disposable data only, because a
power loss can require `fsck_hfs`.

**What happens if a write is interrupted?**<br>
On the next mount the journal replays committed transactions and discards
incomplete ones. If the journal is invalid or replay fails, the volume mounts
read-only and reports the error. It does not silently ignore the journal and it
does not auto-repair.

**Can I migrate my old overlay?**<br>
Yes. A host exporter reads the donor overlay and writes the files into ordinary
native HFS+ directories, with verify-before-switch and rollback. The firmware
does not “repair” the old container.

**How long will this take?**<br>
Months to years, not an afternoon. The donor build is a day. The first read-only
player is a larger project. The HFS+ writer, the journal and the device matrix
are the long poles. The donor overlay was small because it never had to
understand HFS+ B-tree mutation.

**What can I test without coding?**<br>
Toolchain builds, host tests, backups, Disk Mode/DFU, boot, playback, sleep,
USB read-only, `fsck_hfs` runs, crash-image replay rehearsals, migration
rehearsals and the device matrix.

**What should I never do?**<br>
Never format, erase, restore or delete the journal to fix a software error.
Never run a write test on the only copy of the music. Never use `--single`.
Never claim a gate passed without a recorded run.

**Is Helikon-OS an official Rockbox or Rockpod project?**<br>
No. It is an independent system. Rockbox and Rockpod are upstream lineage and
must be credited, but the product identity is Helikon-OS.

**How can I help?**<br>
Test on different 6G revisions and storage adapters. Record boot, sleep,
playback, write, crash-recovery and USB results. Report exact firmware/build
hashes, `fsck_hfs` output and logs.

## Appendix H — Build worksheet

Copy this into a notes file and fill it in before touching a device.

### H.1 Device inventory

| Field | Value |
|---|---|
| iPod model/revision | |
| Capacity | |
| Storage adapter | |
| Card/SSD model | |
| Battery condition | |
| macOS version | |
| Current firmware | |
| Current bootloader | |
| Journaled volume? (yes/no) | |
| Donor overlay present? (migration) | |
| Disk Mode confirmed | |
| DFU confirmed | |
| Apple restore path confirmed | |

### H.2 Backup evidence

| Item | Path | Size | SHA-256 | Read back? |
|---|---|---|---|---|
| Music library | | | | |
| `.rockbox` | | | | |
| `/.rockpod-rw` (donor migration) | | | | |
| Database files | | | | |
| Current firmware | | | | |
| `fsck_hfs -n` output | | | | |

### H.3 Toolchain evidence

| Tool | Version | Works? |
|---|---|---|
| `arm-elf-eabi-gcc` | | |
| `make` | | |
| Python 3 | | |
| `git` | | |
| `gh` | | |
| Host tests | | |
| Sanitizer | | |
| `fsck_hfs` | | |
| `diskutil verifyVolume` | | |

### H.4 Phase sign-off

| Phase | Evidence | Pass? | Date |
|---|---|---|---|
| 0 Donor build | | | |
| 1 Recovery/backup | | | |
| 2 Read-only prototype | | | |
| 3 Non-journaled write prototype (disposable) | | | |
| 4 Journaled write engine | | | |
| 5 Database/player/UI | | | |
| 6 USB/update/migration | | | |
| 7 Hardening | | | |

### H.5 Hard stops

- [ ] This is not the only copy of the music.
- [ ] The backup has been read back.
- [ ] Disk Mode and DFU are confirmed.
- [ ] The Apple restore path is confirmed.
- [ ] The bootloader is not being installed with `--single`.
- [ ] No step requires formatting the library volume or deleting the journal.
      (Formatting a disposable test volume in Stage 3 is the only exception.)
- [ ] No write test runs on the only copy of the music.
- [ ] Every crash test ends with `fsck_hfs` reporting zero repairs.
- [ ] Every claimed test has a recorded log.

## Closing

The donor tree is valuable because it contains years of hardware and format
knowledge. It is dangerous because it also contains years of product
accumulation. A new bootloader/firmware should be an act of **selection**, not
inheritance: keep the register sequences, the HFS+ parser and validators, the
journal discipline and the tests; rewrite the VFS, the database, the player, the
UI and the power policy; and build the one thing the donor never had—a native
HFS+ write engine that macOS and `fsck_hfs` still accept.

The overlay was a clever way to avoid B-tree mutation. It is not the product.
Helikon-OS is the harder, more useful promise: the iPod owns its HFS+ library,
writes ordinary files, survives a power cut, and hands the volume back to macOS
without a hidden namespace or a repair prompt. That is the standard the rest of
this paper is trying to meet.
