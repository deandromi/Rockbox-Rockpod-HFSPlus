# A Greenfield Bootloader and Firmware for the iPod Classic 6G

### A line-audited design study of the HFSPlus Rockboxed iPod tree

Status: research/design paper, 2026-11-03.  
Scope: every tracked subsystem in this repository, with custom modules audited at
file and function level and inherited Rockbox subsystems audited at module level.  
Purpose: decide what to keep, modify, rewrite or exclude if the project became a
completely new bootloader and firmware rather than a continuation of this fork.

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
- [9. The overlay](#9-the-overlay)
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
- [Appendix A — Custom file ledger](#appendix-a--custom-file-ledger)
- [Appendix B — Inherited hotspot ledger](#appendix-b--inherited-hotspot-ledger)
- [Appendix C — Verdict summary](#appendix-c--verdict-summary)
- [Appendix D — Name candidates](#appendix-d--name-candidates)

---

## 0. Abstract

The repository is large: roughly **7,462 tracked files and 2.8 million lines**
across `apps/`, `firmware/`, `lib/`, `bootloader/`, `tools/`, `themes/`, `wps/`
and assets. Most of that is inherited Rockbox. The project-specific value is
concentrated in a much smaller set of modules: the S5L8702 board support, the
HFS+ reader and adapter, the overlay format and validator, the bootloader
integration, the host tools and the test fixtures.

The central recommendation of this paper is:

> **Copy the oscilloscope work and the on-disk validators; rewrite the operating
> system around them.**

A new firmware should keep the hard-won hardware sequences and the HFS+ format
knowledge, but it should not inherit the FAT-shaped VFS, the 5,700-line tagcache,
the 4,300-line playback state machine, the 946,000-line plugin tree, the legacy
overlay engine or the “copy all of Rockbox because it boots” architecture.

The target architecture is **recovery-first, read-only by default, transactional
when writable**:

- a tiny native-HFS+ bootloader that verifies and loads firmware and never
  touches the overlay;
- a firmware built from a clean HAL, block layer, VFS, transactional overlay,
  host-built database, first-party player/UI and read-only USB;
- one new overlay format in the core, with a host-side converter for old data;
- explicit power states, explicit flush barriers and explicit errors;
- no native HFS+ writes, no journal replay, no destructive repair actions.

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

1. native HFS+ catalog, allocation bitmap, extents tree, volume header,
   alternate header and journal are never written;
2. the overlay is user state and is never deleted, truncated or recreated to
   “fix” a software error;
3. fresh initialization is only for an all-zero container;
4. the Apple NOR boot path is never destroyed;
5. no format, erase, restore or repartition as a software repair;
6. a missing/invalid overlay disables writes and fails closed;
7. no hardware or power claim without a recorded device measurement.

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

- **Safety first:** code that can write native metadata or brick NOR is
  presumptively excluded, regardless of convenience.
- **Complexity budget:** a module must justify its lines by user value or
  irreplaceable hardware knowledge.
- **One source of truth:** no parallel FAT/HFS+ paths, no duplicate overlay
  validators, no legacy and modern database formats live in the same core.

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

A new firmware should be **a small, recovery-first system that treats HFS+ as
read-only native storage and the overlay as a transactional sidecar**, with a
clean VFS and a first-party player/UI, rather than a general-purpose Rockbox
distribution with an HFS+ adapter bolted into the FAT layer.

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

The bootloader shares the HFS+ adapter and can link overlay code, although the
bootloader path deliberately does not load overlay state. The firmware image is
not authenticated; `load_firmware()` trusts the file it reads.

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
| Overlay code in bootloader | **X** | Do not link at all. |
| `load_firmware()` fixed-address jump | **M/R** | Add manifest, hash, size and board checks; load to validated address. |
| DFU installer and dual-boot knowledge | **K/M** | Keep, add image verification and recovery gates. |
| `--single` | **X** | Never expose. |
| Verbose error screens | **K/M** | Keep codes, add recovery instructions. |

### 5.3 Proposed bootloader

The new bootloader should be a **rescue loader**, not a small OS:

- native HFS+ read-only only;
- no overlay parsing, no writes, no plugins, no audio, no database;
- verify a firmware manifest: magic, board ID, version, load address, size,
  SHA-256 and signature;
- load `/.firmware/current.img` or a fallback image;
- offer Apple / firmware / recovery / USB modes;
- keep the Apple NOR path and DFU recovery intact;
- enforce a code-size/MOVE_AREA budget in the build;
- expose a recovery screen with error codes and a safe power-off.

A boot counter or A/B fallback is desirable, but it requires a writable,
bootloader-readable location that is not native HFS+ metadata. The cleanest
option is a small reserved area in the overlay container header, read-only from
the bootloader, with a checksum; the bootloader never writes it, the firmware
does. If that is too much for v1, use button-selected recovery and ship without
automatic rollback rather than inventing a risky NOR variable.

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
  decomposed forms. Decide between exact-byte lookup and canonical
  equivalence, document it, and test both.
- **Hardlinks, symlinks, compression, resource forks:** implement read-only
  support or reject with a specific code. Do not silently skip.
- **Extents overflow:** keep the read-only path, raise or remove the fixed
  extent cache limit only with a measured design, and expose the limit.
- **Journal:** never replay; mount read-only and report `-4`.

---

## 9. The overlay

### 9.1 Current shape

The overlay is a preallocated HFS+ file (`/.rockpod-rw`) with:

| Module | Lines | Role |
|---|---:|---|
| `hfsplus_overlay.h` | 145 | layout, flags, IDs, parent hash |
| `hfsplus_overlay_format.c` | 351 | header parse/sync, v1/v2/v3, entry encode |
| `hfsplus_overlay_load.c` | 256 | mount, stage diagnostics, init |
| `hfsplus_overlay_alloc.c` | 399 | bitmap, runs, growth, materialization |
| `hfsplus_overlay_store.c` | 367 | entries, IDs, hash indexes, parent chains |
| `hfsplus_overlay_validate.c` | 225 | whole-overlay validation |
| `hfsplus_index_cache.c` | 62 | persisted directory/CNID cache |
| `hfsplus_diag.c` | 161 | stage/reason/slot diagnostics |
| `hfsplus_state.h` | 108 | per-volume state |

The v3 format is a bitmap + 320-byte entry table + 64-block index cache + data
blocks. Commit order is bitmap → entry/extent → data → logical size, with
header syncs and checksums.

### 9.2 Verdict

**Keep the format and validator for migration; replace the engine.**

- **K:** v3 header/entry layout, checksum rules, validation reasons, stage
  diagnostics, high-water bound, index-cache idea.
- **M:** the validator should be shared by C and Python through a generated
  manifest or a common fixture corpus.
- **R:** allocation/store/load as a single transactional engine.
- **X:** v1/v2/v3 live in the new core; support them only in a host converter.
- **X:** zero-header reset, auto-repair, silent native fallback.

### 9.3 Proposed v4 container

A new format should make crash consistency the primary design constraint:

```text
superblock
  magic, version, block size, total blocks, generation,
  checkpoint segment, checkpoint sequence, feature flags, checksum
segments
  segment header: sequence, used bytes, checksum
  records: create/write/truncate/rename/delete/settings/data
  commit record: transaction id, record count, checksum
```

Rules:

1. append-only writes; never overwrite a live record;
2. data and metadata records are written first;
3. a single commit record makes the transaction visible;
4. flush/barrier before the commit record and after it;
5. replay is idempotent; a torn commit is ignored;
6. any checksum/generation failure mounts read-only;
7. compaction copies live records, writes a new checkpoint, then switches
   atomically;
8. one format in the core; a host converter imports v1/v2/v3.

This removes the bitmap/entry/data ordering puzzle from the critical path and
makes power-loss testing tractable.

---

## 10. Namespace, VFS and file cache

### 10.1 Current shape

| File | Lines | Role |
|---|---:|---|
| `hfsplus_namespace.c` | 686 | native + overlay merge, readdir, rename/remove |
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

**Replace the VFS; keep the HFS+ namespace policy and selected cache ideas.**

- **K:** overlay visibility rules, tombstone/rename semantics, parent/name
  identity, read-only name cache concept, stream read-ahead concept.
- **M:** `hfsplus_namespace.c` split into `lookup`, `iterate`, `mutate`.
- **M:** `hfsplus_fat.c` becomes a thin `fs_ops` adapter, not the center of the
  world.
- **R:** `fat.c`, `file.c`, `file_internal.c`, `fileobj_mgr.c`.
- **X:** FAT-shaped `readdir` directory scan as the primary lookup path,
  case-insensitive assumptions, global path state.

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
    ssize_t (*read)(struct file *f, void *buf, size_t n);
    ssize_t (*write)(struct file *f, const void *buf, size_t n);
    int (*truncate)(struct file *f, uint64_t size);
    int (*rename)(struct fs *fs, struct file *f, const char *name);
    int (*remove)(struct fs *fs, struct file *f);
    int (*stat)(struct fs *fs, const struct dirent *de, struct stat *st);
    int (*flush)(struct file *f);
};
```

Capabilities are explicit:

```text
READ, WRITE_OVERLAY, DIRECTORY, SEEKABLE, READ_ONLY_NATIVE
```

Errors are specific: `NOT_FOUND`, `NOT_DIR`, `IS_DIR`, `UNSUPPORTED`, `IO`,
`NO_SPACE`, `READ_ONLY`, `CORRUPT`. No errno guessing at the boundary.

---

## 11. Settings, diagnostics and debug UI

### 11.1 Settings

`apps/settings.c` (1,511 lines) and `apps/settings_list.c` (2,468 lines) form a
large table-driven settings system with runtime status files, CRC tracking,
config promotion and callbacks.

**Verdict: M/R.** Keep the user-facing settings and the table-driven idea; rewrite
the storage layer:

- versioned settings schema;
- two redundant config slots with checksums and atomic promotion;
- explicit defaults and migration;
- no settings write from arbitrary callbacks;
- no “deferred until idle” behavior that can hide a failed save.

### 11.2 Diagnostics

`hfsplus_diag.c` (161 lines), `apps/debug_menu.c` (3,141 lines) and `logf` are
valuable. **K/M:** keep the counters and stage/reason/slot diagnostics; split
the debug menu into a small diagnostic panel and a host-readable event log.
Persist crash/event logs in the overlay, never in native HFS+.

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
runtime stats log: play counts, ratings, resume points (overlay)
```

Built on macOS, validated on mount, memory-mapped/read-only on device.

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
- **X:** writeable USB mass storage on HFS+; host writes to the overlay;
  `--single`.

Updates should be a first-class protocol:

```text
firmware.img + firmware.manifest
  magic, board, version, load address, size, SHA-256, signature
```

The bootloader verifies before jumping. The firmware writes the new image and
manifest to a staging file in the overlay or a dedicated native file? Native
writes are forbidden, so staging must be in the overlay, and the bootloader
must be able to read the overlay header or a small native file written by
macOS. This is an open design decision; see §25.

---

## 17. Build system, toolchain and host tools

### 17.1 Current shape

- Rockbox `tools/configure` + recursive Makefiles;
- ARM `arm-elf-eabi-gcc` 9.5.0;
- `tools/scramble`, `bmp2rb`, `utils/mks5lboot`;
- `tools/hfsplus` inspector and tests;
- `tools/rockpod` host database/artwork/overlay tools;
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
   +--> Rescue bootloader (NOR)
          - native HFS+ read-only
          - manifest/hash/signature verification
          - Apple / firmware / recovery / USB menu
          - load validated image to DRAM
                 |
                 v
          Firmware
            board HAL
              block device
                filesystem: HFS+ read-only
                overlay: transactional v4
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
| HFS+ | native read-only namespace | overlay writes, UI |
| Overlay | transactional writes | native metadata, boot |
| VFS | lookup, handles, capabilities | hardware registers |
| Database | immutable metadata/index | playback state |
| Player | buffer, decode, output | filesystem internals |
| UI | screens, input, rendering | storage policy |
| USB | LUN, descriptors | native writes |
| Diagnostics | events, counters, recovery | product policy |

### 19.2 Memory budget

64 MiB must be treated as a hard budget:

- bootloader: code + stack inside `MOVE_AREA`;
- kernel/HAL: small, no large BSS;
- HFS+ caches: bounded node cache, bounded name cache;
- overlay: bounded transaction/compaction buffers;
- database: read-only mapped pages, not a full 10k-track RAM copy unless
  measured;
- audio: codec + buffer;
- UI: one screen + one theme;
- reserve for recovery/diagnostics.

Every cache needs a maximum and an eviction policy. No unbounded `core_alloc`.

### 19.3 Security model

- signed firmware manifest; verify before jump;
- read-only USB by default;
- overlay records validated with checksums and bounds;
- path names length-checked, no traversal;
- plugins only with capabilities, or not at all;
- DFU installer verifies the image and refuses `--single`;
- no secrets on the device; no network.

### 19.4 Update and recovery

- keep Apple firmware and DFU path;
- firmware image staged and verified;
- explicit “install update” action, never silent;
- recovery screen: read-only mount, export logs, rollback, DFU instructions;
- no auto-format, no auto-delete overlay.

---

## 20. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| NOR/DFU brick | Device unusable | Keep Apple path, never `--single`, recovery plan, hardware test. |
| HFS+ parser corruption | Data loss / crash | Standalone parser, fuzzing, bounds checks, read-only. |
| Overlay power-loss inconsistency | Lost writes / corruption | v4 commit records, barriers, replay tests, read-only mount on failure. |
| Native write fallback | HFS+ damage | Fail closed; no native write API in new core. |
| Storage resume failure | Dead device | Explicit power state machine; 100-cycle matrix. |
| Memory exhaustion | Audio/UI failure | Hard budgets, bounded caches, no full DB RAM copy by default. |
| Audio regression | Poor product | Codec/DSP tests, device audio matrix. |
| Migration data loss | User state lost | Host converter, backups, verify-before-switch. |
| Security issue | Malicious file/firmware | Signed manifests, validation, read-only USB, plugin sandbox. |
| Legal/licensing | Distribution issue | GPL compliance, attribution, trademark review. |
| Scope creep | Never ships | First-party feature list, no plugin tree, phased roadmap. |

---

## 21. What I would absolutely not include

These are not stylistic preferences. Each is a hard exclusion for the new
product unless a later, measured design explicitly reverses it.

1. **Native HFS+ metadata writes.** No catalog, bitmap, extents tree, volume
   header, alternate header or journal writes.
2. **Journal replay.** Dirty volumes mount read-only.
3. **Format/erase/restore/repair as a firmware action.**
4. **Overlay loading in the bootloader.** The bootloader is native-only.
5. **`--single` bootloader installation.** It destroys the Apple NOR path.
6. **Writeable USB mass storage on HFS+.** Read-only LUN only.
7. **Silent fallback to native writes when the overlay is invalid.** Fail
   closed.
8. **“Zero the header to reset” initialization.** Only all-zero containers may
   initialize.
9. **Hiding unsupported files from `readdir`.** Expose them and return a
   specific error.
10. **v1/v2/v3 overlay formats in the new core.** A host converter only.
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

The last one is the most important cultural rule. A new firmware that keeps all
of Rockbox has not simplified anything; it has inherited twenty years of
features that were designed for different hardware and different constraints.

---

## 22. Migration from the current tree

A new firmware must not strand existing users.

1. **Read-only audit:** mount the existing v3 overlay with the old validator and
   produce a manifest of entries, flags, sizes and checksums.
2. **Backup:** copy `/.rockpod-rw`, `.rockbox`, settings, playlists and
   database files. Verify the copies.
3. **Convert:** host tool reads v3 and writes v4 into a new container; verify
   every record and checksum.
4. **Dry run:** mount the v4 container read-only on the new firmware; compare
   namespace and file hashes.
5. **Switch:** keep the old container until the user confirms boot, playback,
   settings and a reboot.
6. **Rollback:** retain the old firmware and old container; never delete during
   the migration window.

The converter should be a host tool, not a bootloader feature, and should be
tested with the same fixture corpus as the C validator.

---

## 23. Phased roadmap

| Phase | Deliverable | Exit criteria |
|---|---|---|
| 0 | Rescue bootloader + native HFS+ read-only firmware skeleton | Boots, loads verified image, Apple path intact, recovery menu. |
| 1 | Read-only player: HAL, HFS+, audio, UI, database | Plays music, browses library, no writes. |
| 2 | Transactional overlay v4 | Settings/playlists/writes survive power-loss matrix. |
| 3 | Migration + host tools | v3 → v4 verified; backups; rollback. |
| 4 | USB read-only + diagnostics + update flow | macOS mounts read-only; logs export; update verified. |
| 5 | First-party features (PictureFlow, album art) | Feature parity with the product spec, not Rockbox. |
| 6 | Optional plugin sandbox | Capability model, quotas, crash isolation. |
| 7 | Power/thermal/battery tuning | Measured matrix; no unmeasured defaults. |

---

## 24. Validation strategy

Host:

- unit tests for every parser and state machine;
- fuzzing for HFS+ B-trees, extents, names, overlay records;
- property tests for allocation/commit ordering;
- model-based power-loss simulation;
- deterministic fixtures shared by C and Python validators;
- ASan/UBSan on every host target.

Device:

- cold/warm boot, battery-disconnect boot;
- HDD, iFlash SD, iFlash CF, SSD;
- 100-cycle sleep/wake and resume;
- power-cut during create/append/close/rename/delete/settings/cache install;
- native HFS+ `diskutil verifyVolume` and catalog/allocation/journal comparison;
- audio matrix, click wheel, charging, lineout;
- DFU install/uninstall and recovery;
- update/rollback and migration.

Release gate:

- no stable release without a recorded device run;
- no “host tests passed” claim presented as hardware validation;
- no unexecuted stress gate hidden in the notes.

---

## 25. Open design decisions

1. **Boot image verification:** signature scheme and key storage; how to avoid
   bricking if a key is lost.
2. **Fallback slot:** where to store a boot counter without native writes.
3. **Overlay v4 allocation:** pure log segments vs bitmap + log; compaction
   policy.
4. **Unicode policy:** exact bytes vs canonical equivalence; normalization
   table size.
5. **Database location:** host-built only vs optional on-device build.
6. **Plugin model:** none, capability API, or bytecode sandbox.
7. **FAT fallback:** read-only recovery only, or full read/write fallback.
8. **Theme format:** minimal declarative format vs retained-mode layouts.
9. **Update transport:** USB read-only + macOS staging vs DFU-only.
10. **Product name and identity** (see Appendix D).

---

## Appendix A — Custom file ledger

Line counts are from the audit tree; they will drift.

| File | Lines | Verdict | Keep / change / drop |
|---|---:|---|---|
| `bootloader/ipod-s5l87xx.c` | 1,016 | M | Keep bring-up/DFU; rewrite boot flow. |
| `firmware/hfsplus/hfsplus_ro.c` | 1,274 | K/M | Keep parser; split; add fuzz/normalization/extents policy. |
| `firmware/hfsplus/hfsplus_ro.h` | 163 | K/M | Keep contract; narrow. |
| `firmware/hfsplus/hfsplus_partition.c` | 296 | K | Keep scanner; expand malformed tests. |
| `firmware/hfsplus/hfsplus_io.c` | 220 | K/M | Keep sector I/O; one block API. |
| `firmware/hfsplus/hfsplus_overlay.h` | 145 | K | Keep v3 layout for migration. |
| `hfsplus_overlay_format.c` | 351 | K/M | Keep format; new v4 writer. |
| `hfsplus_overlay_load.c` | 256 | M | Keep diagnostics; new loader. |
| `hfsplus_overlay_alloc.c` | 399 | R | Replace with transactional allocator. |
| `hfsplus_overlay_store.c` | 367 | R | Replace with record/store engine. |
| `hfsplus_overlay_validate.c` | 225 | K/M | Keep reasons; share fixtures. |
| `hfsplus_namespace.c` | 686 | M | Split lookup/iterate/mutate; no silent skip. |
| `hfsplus_fat.c` | 377 | M | Thin fs_ops adapter. |
| `hfsplus_fat.h` | 109 | M | Error/capability mapping. |
| `hfsplus_mount.c` | 374 | M | Native-only bootloader; new mount flow. |
| `hfsplus_index_cache.c` | 62 | M | Keep idea; new v4 index. |
| `hfsplus_diag.c` | 161 | K/M | Keep counters; new event log. |
| `hfsplus_state.h` | 108 | M | New state model. |
| `hfsplus_time.c` | 40 | K | Keep conversion. |
| `tools/hfsplus/tests/*` | ~2,958 | K | Keep and extend. |
| `tools/rockpod/*` | ~1,036 | M/R | New schema/converter tools. |
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
| `hfsplus_overlay.h` | 145 | K | Keep v3 layout for migration and diagnostics. |
| `hfsplus_overlay_format.c` | 351 | K/M | Keep v3 parse/sync/checksum; add v4 writer in new module. |
| `hfsplus_overlay_format.h` | 31 | K/M | Keep v3 entry encode/decode contract. |
| `hfsplus_overlay_load.c` | 256 | M | Keep stage diagnostics; rewrite loader for v4. |
| `hfsplus_overlay_load.h` | 11 | M | New loader API. |
| `hfsplus_overlay_alloc.c` | 399 | R | Replace bitmap/run allocator with transactional segment allocator. |
| `hfsplus_overlay_alloc.h` | 19 | R | New allocator API. |
| `hfsplus_overlay_store.c` | 367 | R | Replace entry lifecycle/hash chains with record engine. |
| `hfsplus_overlay_store.h` | 26 | R | New record API. |
| `hfsplus_overlay_validate.c` | 225 | K/M | Keep reason/slot model; share fixtures with converter. |
| `hfsplus_namespace.c` | 686 | M | Split lookup/iterate/mutate; never skip unsupported entries. |
| `hfsplus_namespace.h` | 12 | M | New namespace API. |
| `hfsplus_fat.c` | 377 | M | Thin fs_ops adapter; keep diagnostics/error mapping. |
| `hfsplus_fat.h` | 109 | M | Capability/error helpers; remove policy leakage. |
| `hfsplus_mount.c` | 374 | M | Native-only bootloader path; new firmware mount. |
| `hfsplus_index_cache.c` | 62 | M | Keep idea; new v4 index/checkpoint. |
| `hfsplus_index_cache.h` | 10 | M | New API. |
| `hfsplus_diag.c` | 161 | K/M | Keep counters/stage reasons; new event log. |
| `hfsplus_diag.h` | 11 | K/M | Extend, do not renumber. |
| `hfsplus_state.c` | 10 | M | New per-volume state model. |
| `hfsplus_state.h` | 108 | M | Split bootloader/firmware state; no 708 KiB overlay in boot. |
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
| `tools/rockpod/` | 6 | 1,036 | M/R | New schema, converter, exporter. |
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
| Overlay v4 | ~1,200 | format/validator concepts | One transactional engine. |
| VFS/block | ~1,500 | `fat.c`, `file.c` | Clean interfaces. |
| Database | ~1,500 | host builder concept | Immutable, no dircache. |
| Player/audio | ~3,000 | selected Rockbox codecs | New state machine. |
| UI | ~4,000 | new | First-party screens. |
| USB/diagnostics | ~1,500 | USB/diag concepts | Read-only, structured logs. |
| Host tools | ~5,000 | `tools/hfsplus`, `tools/rockpod` | New schema/converter. |
| **Product core** | **~25,000–30,000** | — | Excludes selected codecs and assets. |

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
| Overlay references | X | Remove entirely. |

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

**Keep:** S5L8702 HAL, ATA transport, partition scanner, HFS+ reader, overlay
format/validator for migration, DFU/NOR knowledge, host tests, selected codecs,
one font/icon/theme set, documentation invariants.

**Modify:** boot flow, HFS+ namespace, overlay mount/diagnostics, settings
storage, USB descriptors, debug UI, build system, migration tools.

**Replace:** VFS/file layer, overlay allocator/store, tagcache/tagtree, playlist
storage, playback state machine, UI framework, database schema.

**Exclude:** native writes, journal replay, overlay in bootloader, `--single`,
writeable USB, zero-header reset, silent unsupported-file hiding, live legacy
overlay formats, tagcache/dircache, full plugin tree, raw plugin hardware API,
legacy WPS/themes, unsigned updates, periodic early-boot power callbacks,
unmeasured power/charging changes, full Rockbox application copy.

## Appendix D — Name candidates

These names do not contain “Rockpod”, “Rockbox” or “HFS+”. Verify trademark,
domain and GitHub availability before adopting one.

| Name | Why it fits | Possible family |
|---|---|---|
| **Keel** | The structural backbone of a ship; stability and boot foundation. | Keel bootloader / project. |
| **Basalt** | A solid, immutable base; the native volume as bedrock. | Basalt firmware core. |
| **Cairn** | A trail marker; recovery, waypoints and safe return. | Cairn recovery mode. |
| **Auralis** | A coined audio name; player and sound identity. | Auralis player. |
| **Vellum** | A clean surface for the interface; UI/theme identity. | Vellum UI. |
| **Rampart** | A defensive wall; overlay and fail-closed protection. | Rampart overlay. |
| **Kestrel** | A precise, fast bird; responsiveness and reliability. | Kestrel performance profile. |
| **Mooring** | A safe attachment point; storage and recovery. | Mooring storage layer. |

**Recommended family:** **Keel** as the project/boot foundation, **Basalt** as
the firmware core, **Cairn** for recovery, **Auralis** for the player and
**Vellum** for the UI. The names are independent of the old filesystem and
Rockbox identity. Verify trademark, domain and GitHub availability before
committing; avoid names already used by large technology or audio companies.

---

## Closing

The donor tree is valuable because it contains years of hardware and format
knowledge. It is dangerous because it also contains years of product
accumulation. A new bootloader/firmware should be an act of **selection**, not
inheritance: keep the register sequences, the HFS+ parser, the overlay format
knowledge and the tests; rewrite the VFS, the database, the player, the UI and
the power policy; and refuse the code that makes native data unsafe or the
product impossible to reason about.
