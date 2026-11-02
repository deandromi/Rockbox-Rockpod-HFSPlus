# Rockpod HFS+ Efficiency Paper

## Functional, code, and operational upgrades for the iPod Classic 6G

Status: **DRAFT — for owner review. Not a release document.**
Target: iPod Classic 6G / `ipod6g` (target ID 71), 64 MiB RAM, iFlash.
Baseline: V3.1 Beta 6 review build (Beta 5 plus v3 high-water batching).
Device evidence is from the published Beta 5 read-only USB run; the high-water
change is host-tested and firmware-built, not yet device-tested.
Date: 2026-11-02.

---

## Abstract

Rockpod HFS+ is a HFS+-native Rockbox fork for the iPod Classic 6G. Its
efficiency profile is dominated by three things, in order:

1. how long the ATA/iFlash path stays awake;
2. how many ATA flushes and metadata writes the overlay write path performs;
3. how much catalog, database and artwork I/O the firmware repeats at boot and
   during browsing.

A fourth, HFS+-specific multiplier is easy to miss: dircache is disabled, so
every `open()`/`file_exists()` path component is resolved by a linear directory
scan. Tagcache filename lookups, album-art probing and playlist checks inherit
that cost and can dominate ordinary playback/browsing.

CPU work matters, but the S5L8702 already drops to 54 MHz when it is not
boosting, and the measured firmware image is small enough that instruction
count is rarely the first-order cost. The practical wins are therefore mostly
about **avoiding disk work**, **batching durable writes**, and **not
re-validating immutable facts**.

This paper separates:

- **code facts** taken from the repository;
- **host measurements** from the existing test/build tooling;
- **device observations** recorded in the V3.1 Beta 5 USB run;
- **estimates and experiments** that still need hardware measurement.

It covers the custom HFS+ layer and the inherited Rockbox tree, including
storage, CPU/clock, backlight/LCD, disk cache, FAT, tagcache, playback,
PictureFlow, the bootloader, build flags, and the Mac-side database/artwork
workflow.

The first code change from this review is already in the tree: v3 high-water
entry reservations are batched in 64-slot chunks, removing the per-create
header flush while preserving persist-before-expose ordering. It is host-tested
and firmware-built but not yet device-tested.

---

## 1. Method and evidence labels

### 1.1 Scope

The review covers:

- `firmware/hfsplus/` — reader, partition scanner, overlay, FAT adapter, index
  cache, diagnostics;
- `bootloader/ipod-s5l87xx.c` — native-only HFS+ boot path;
- inherited `firmware/` storage, ATA, disk cache, FAT, power, backlight, LCD,
  CPU clocking and USB code;
- inherited `apps/` tagcache, file browser, playback, album art, PictureFlow,
  settings and plugin code;
- `tools/hfsplus/` and `tools/rockpod/` host tools;
- build/configure/linker flags and the generated `rockbox.map` /
  `bootloader.map`;
- operational workflow in `README.md` and `shipping.md`.

It does **not** attempt to optimize the audio DSP or codec math. Those are
inherited, already tuned for the target, and not the bottleneck on an iFlash
library device.

### 1.2 Evidence labels

| Label | Meaning |
|---|---|
| **[code]** | Directly visible in the repository. |
| **[host]** | Reproduced by the host test/build tooling. |
| **[device]** | Recorded on the test iPod; not a general hardware guarantee. |
| **[estimate]** | Engineering estimate; needs measurement. |
| **[experiment]** | Proposed change or setting that must be tested before adoption. |

### 1.3 Safety invariants

Any efficiency change must preserve:

- native HFS+ catalog, allocation bitmap, extents tree, volume header and
  journal are never written;
- `/.rockpod-rw` is user state and is never deleted, truncated or recreated
  without a verified backup;
- bitmap → entry/extent → data → logical size commit order;
- fresh initialization only for an all-zero container;
- fail-closed behavior for a damaged overlay;
- read-only USB does not become an accidental write path.

An efficiency change that weakens any of these is not an efficiency change; it
is a data-loss risk.

---

## 2. Baseline snapshot

### 2.1 Firmware and bootloader size

From the current build artifacts and map files:

| Artifact | `.text` | `.rodata` | `.data` | `.bss` | Image |
|---|---:|---:|---:|---:|---:|
| `rockbox.elf` | 638,568 B | 167,032 B | 9,340 B | 2,877,048 B | 823,248 B `rockbox.ipod` |
| `bootloader.elf` | 90,024 B | 5,620 B | 7,776 B | 2,017,856 B | 103,420 B |

The bootloader's `MOVE_AREA` is `0x1f800` = 129,024 bytes. A 10,240-byte
`.stack` lives inside that region, leaving a **code budget of 118,784 bytes**.
`_moveend` is `0x22039c00`, so the current image uses 103,424 bytes and leaves
**15,360 bytes (12.9%)** of code headroom. That is a release-blocking margin,
not a comfortable one. The DFU payload ceiling is about 130,292 bytes, but
`MOVE_AREA` binds first.

`size` reports 813,888 B of “text” because it includes `.rodata`; the map
splits that into `.text` 638,568 B plus `.rodata` 167,032 B (plus a small
remainder). Use the map for section-level decisions and `size` for the image
total. The bootloader's `size` BSS is 2,028,096 B; the map `.bss` is
2,017,856 B, with the difference in the 10,240-byte stack area.

Firmware IRAM use is 42,476 of 49,152 bytes, leaving 6,676 bytes (13.6%) free.
The firmware is not under a size crisis; the bootloader is.

### 2.2 Large RAM consumers

`arm-elf-eabi-nm` and the map file show:

| Symbol / object | Size | Notes |
|---|---:|---|
| `hfs_states` | 1,298,304 B (1.24 MiB) | one HFS+ volume state |
| overlay `entries[2048]` | 622,592 B | 304 B per entry |
| v3 `index_cache` | 262,144 B | 64 blocks, 256 KiB |
| overlay bitmap | 65,536 B | 2 GiB / 8 bits |
| catalog node cache | 32,768 B | one 32 KiB node |
| HFS+ scratch | 32,768 B | catalog/overlay scratch |
| HFS+ I/O bounce | 16,448 B | 16 KiB aligned buffer |
| disk cache (`cache_buffer`) | 262,144 B | 64 × 4 KiB |
| FAT bounce buffers | 163,840 B | 4 volumes × 10 × 4 KiB |
| `logfbuffer` | 262,145 B | debug log buffer |
| LCD static/double buffers | 307,200 B | 2 × 150 KiB |
| artwork buffer | 71,808 B | album art |

The point is not that all of this should be removed. The point is that
**`hfs_states` alone is larger than the bootloader's entire code**, and the
bootloader links the same state even though it never loads the overlay or the
v3 index cache. This is a structural size/RAM opportunity, not a micro-optimization.

### 2.3 Host and device evidence

- **[host]** HFS+ host suite: 20 reader tests, 7 partition tests (1 native
  adapter test skipped when not supplied), 30 adapter tests; ASan/UBSan passes.
- **[host]** Firmware and Thumb bootloader build; `mks5lboot` DFU builds.
- **[device]** V3.1 Beta 5 read-only USB: macOS mounted `Apple_HFS` at
  offset 67,624,960 read-only; copied firmware hash matched; host write was
  rejected; overlay `RPOVL13` parsed with 15 live entries; `diskutil
  verifyVolume` passed.
- **[device]** The device run did **not** measure power, flush latency, boot
  time or write amplification.

---

## 3. Where the energy actually goes

On a spinning iPod, disk spin-up dominates. On an iFlash Classic, the storage
is solid-state, but the ATA controller, card, level shifters and bus still have
an awake cost. The firmware also performs ATA `FLUSH CACHE` operations for
durability, and each metadata commit can wake or hold the path active.

A useful mental model:

```text
total energy ≈ awake_time × awake_power
             + flushes × flush_cost
             + bytes_written × write_cost
             + CPU_active_time × CPU_power
             + backlight/LCD_on_time × display_power
```

This ordering matters:

- A 4 KiB metadata write costs more than thousands of ARM instructions.
- A single unnecessary ATA flush can cost more than a large CPU-side loop.
- Keeping the LCD/backlight on for an extra 10 seconds can outweigh a whole
  boot-time index optimization.
- CPU boost is already demand-driven; forcing 216 MHz all the time is unlikely
  to help battery life.

The rest of this paper attacks the first three terms first.

---

## 4. Custom HFS+ layer

### 4.1 Partition scan and mount

**[code]** `hfsplus_partitions()` in `firmware/hfsplus/hfsplus_mount.c` scans
the MBR, tries an `Apple_HFS` entry in 4096-byte units, then the target
fallback at `HFS_FALLBACK_OFFSET`, then the generic
`hfs_partition_scan()`. The generic scanner probes both 4096-byte and native
units for MBR/GPT, and validates an HFS+ header at the candidate offset.

**[code]** The scan caches the chosen HFS+ offset/unit in
`usb_hfs_offset`/`usb_hfs_unit` so the USB MBR presentation survives the
filesystem unmount. This is now also an efficiency primitive: it avoids
re-deriving geometry at USB handoff.

**Assessment:** the scan is a handful of sector reads; it is not a primary
power cost. Do not add a persistent partition cache just to save it. The
important part is that the result is validated once and reused.

### 4.2 Catalog reader and index

**[code]** `hfs_ro_mount()` validates the primary header, falls back to a
validated alternate header, and then builds the directory/CNID index. The v3
overlay persists that index in 64 reserved blocks (256 KiB). On a cache hit,
`hfs_index_cache_load()` imports it and avoids the full catalog leaf walk. On a
miss, `hfs_ro_enable_fast_index()` walks every catalog leaf, validates every
record, and builds:

- a 16,384-parent directory index;
- a 4,096-entry CNID index.

**[code]** The reader keeps **one** 32 KiB catalog node in
`catalog_cache`. Sequential leaf walks are fine; random lookups can evict the
node and re-read it.

**[code]** `hfs_ro_fast_index_generation()` keys the cache on the native
`write_count`, `modify_date`, catalog size/blocks, total/leaf node counts and
volume attributes. The cache is advisory and checksummed; a mismatch rebuilds.
That is the right model, but it should be tested against Mac-side catalog
changes (add/remove/rename while the iPod is in Disk Mode) before relying on a
cache hit for a long trip.

**Why it matters:** on a large library, a full catalog walk is the largest
boot I/O cost after the overlay. The v3 cache is the right first fix. The
remaining opportunities are:

1. **[experiment]** Measure cache HIT/MISS and mount read bytes on the device
   before changing the cache.
2. **[experiment]** If the cache is too large for small libraries, make the
   reserved cache size dynamic (e.g. 32 or 64 blocks) or compress the exported
   index. The 256 KiB RAM buffer is a fixed cost even when the index is small.
3. **[experiment]** Add a small 2–4 node catalog LRU if profiles show random
   lookup thrash. Do not add it speculatively: one node is often enough for a
   sequential walk, and extra cache costs RAM.
4. **[experiment]** Build the index lazily on the first lookup/readdir instead
   of unconditionally at mount. This improves time-to-first-UI, but not total
   energy for a browsing session. It also changes initialization semantics, so
   it needs careful tests.

Do **not** disable the index: `hfs_ro_find_id()` and directory iteration fall
back to full catalog scans, which is worse for normal use.

**I/O bounce:** **[code]** `device_read()` always reads into the 16 KiB aligned
`hfs_io.buffer` and copies to the caller, even when the caller's buffer is
already aligned and sector-sized. A 32 KiB catalog node therefore pays eight
bounce copies. **[experiment]** bypass the bounce when the caller buffer is
32-byte aligned and the transfer is sector-aligned. The bounce exists for ATA
DMA alignment, so this needs a hardware read test; do not remove the bounce
globally.

### 4.3 Overlay load and validation

**[code]** `overlay_load()` reads:

- the header and backup;
- the allocation bitmap (up to 64 KiB);
- the entry table in 96-entry chunks, bounded by the persisted
  `entry_high_water`;
- the v3 index cache when present.

It then runs `overlay_validate()` over the entries, rebuilds the RAM ID/parent
hash indexes, and publishes READY.

**[code]** `overlay_validate()` currently:

- iterates all 2,048 entry slots, even when `entry_high_water` is smaller;
- resolves legacy duplicate live names and duplicate BASE/tombstone entries
  with an O(U²) nested loop;
- validates every tombstone and BASE relation.

**[estimate]** With 2,048 used slots the duplicate loop is roughly two million
comparisons. That is CPU-only and small compared with a catalog scan, but it is
still avoidable work on every mount.

**[code]** The 707,768-byte `struct hfs_overlay` is zeroed by `crt0` BSS
initialization, then again by `hfsplus_init()`, again by `overlay_load()`, and
again by the bootloader's `hfsplus_mount_overlay()` fallback. Some of those
zeros are redundant. A `memset` is cheap per byte, but 0.7–1.4 MB of writes per
boot is measurable CPU and can be removed with a clear initialization contract
(e.g. zero only on first use or after a failed mount). Do not remove the
zeroing that is required for rollback safety without tests.

**Opportunities:**

1. **[code, low risk]** Bound the validator and `overlay_rebuild_indexes()`
   loops by `entry_high_water` for v3, while keeping the full 2,048 loop for
   v1/v2. This is a pure CPU/scan reduction and preserves all checks for slots
   that can actually exist.
2. **[experiment]** Replace the duplicate-name O(U²) loop with a two-pass
   key/winner table keyed by `(parent, name)` and
   `(parent, base_id, flags)`. The legacy rules are subtle; add fixtures for
   duplicate live names, duplicate BASE entries, duplicate tombstones and
   mixed legacy shapes before changing it.
3. **[experiment]** Add a persistent used-slot bitmap or summary to the v3
   header. It would bound the entry read more tightly than a 64-slot
   high-water chunk and avoid reading reserved-but-empty slots. It costs
   header space and one more thing to validate; only do it with a format
   version bump and migration plan.

**Trade-off introduced by high-water batching:** a v3 overlay with very few
entries now reserves up to 64 slots, so the next mount may read up to 64 extra
320-byte slots (about 20 KiB) instead of only the used ones. That is a small
one-time read cost in exchange for removing a header flush on nearly every
create. It is the right trade for write-heavy bursts; measure it if mount time
on tiny overlays matters.

### 4.4 Overlay write path and flushes

**[code]** The commit order is:

```text
bitmap -> entry/extent -> data -> logical size
```

`overlay_sync_header()`, `overlay_sync_bitmap_range()` and
`overlay_sync_entry()` each call `overlay_flush()`, which calls
`storage_flush()` when `HAVE_STORAGE_FLUSH` is enabled. A create can therefore
produce an entry flush and, when the high-water bound grows, a header flush.

**[host]** The high-water batching change is implemented in
`hfsplus_overlay_store.c`. On a new slot it reserves
`HFS_OVL_HIGH_WATER_CHUNK` (64) slots, syncs the header once, then writes the
entry. For 2,048 creates the host regression allows at most

```text
2,048 entry flushes + 64 header flushes = 2,112 flushes
```

instead of the old two flushes per create (4,096). The expected steady-state
count is 2,080. Each avoided header sync also avoids writing the 4 KiB backup
and 4 KiB primary header, i.e. about 16 MiB of metadata writes across that
burst. The 30 adapter tests pass with this bound; the device-level flush cost
is not yet measured.

**[code]** `overlay_realloc()` and `overlay_materialize_base()` copy data in
4 KiB chunks through `overlay_container_read()`/`overlay_container_write()`.
The underlying `hfs_io` bounce buffer is 16 KiB, so a 16 KiB copy loop would
reduce call overhead and ATA transactions without changing ordering.

**[code]** `overlay_alloc_run()` scans the bitmap bit-by-bit from
`alloc_hint` with two passes; `overlay_range_owned()` scans up to 2,048
entries per allocation/free. These are CPU costs, not disk costs, but they
matter on large allocations and fragmentation.

**Safe opportunities, in order:**

1. **[done, host]** Batch high-water header updates in 64-slot chunks.
2. **[experiment]** Copy realloc/materialization data in 16 KiB chunks using a
   second aligned buffer, keeping the same persist-before-expose order. This
   is a pure throughput improvement for growing/moving files.
3. **[experiment]** Add per-operation flush counters to diagnostics:
   create, write, close, rename, delete, settings. The existing
   `hfsplus_io_flushes()` counter is a total; a breakdown is needed before
   changing flush semantics.
4. **[experiment, high risk]** A commit-group/WAL design that writes bitmap +
   entry + data intent, then flushes once. This can remove the remaining
   per-entry flush, but it is a format and recovery change. It requires the
   power-loss matrix and a tested recovery path. Do not do it as a
   micro-optimization.
5. **[experiment]** Skip an entry sync when the encoded entry is unchanged
   (dirty-bit or cached encoded entry). This is safer than removing flushes
   globally, but needs a precise dirty model.

Do not simply remove intermediate flushes. The bitmap-before-entry ordering is
what prevents a live entry from pointing at unallocated blocks after power
loss.

**[code]** The ATA layer currently hides flush failures: `ata_flush()` always
returns 0 and `ata_flush_cache()` ignores readiness errors. `overlay_flush()`
therefore cannot tell whether a metadata commit actually reached stable
storage. **[experiment]** propagate ATA flush errors through `storage_flush()`
and surface them in overlay diagnostics; this is a correctness prerequisite for
any future commit-group work.

### 4.5 Namespace merge and readdir

**[code]** `hfsplus_readdir()` lists native entries first, then walks the
overlay table linearly from slot 0 to 2,047 for each directory. The overlay
parent hash (`parent_head`/`parent_next`) already exists and is used by
`overlay_hides_base()`, but the readdir overlay phase does not use it.

**Opportunity:** **[experiment]** walk only the parent bucket chain for
`logical_parent` and `backing_parent` instead of scanning all 2,048 slots.
For an overlay with tens of writable files this is a small win; for an overlay
with hundreds it is a clear reduction in per-directory CPU. Keep the linear
fallback behind a validation flag for legacy images until the parent hash is
proven on all shapes.

**[code]** `overlay_hides_base()` already uses the parent hash and checks both
logical and backing parents. A hidden-name cache is only worth adding if
profiles show native readdir spending significant time there. Do not add it
speculatively; the current hash walk is bounded by the number of overlay
children in those parents.

### 4.6 USB read-only path

**[device]** The V3.1 Beta 5 read-only USB path now:

- presents the data partition as `Apple_HFS` in the USB MBR buffer;
- returns the 8-byte `MODE_SENSE(10)` write-protect header;
- drains write data-out before `DATA PROTECT`;
- flushes on `SYNCHRONIZE CACHE` and at handoff.

For efficiency, the read-only path is already close to ideal: it does no host
writes and no overlay work. The remaining USB work is diagnostic, not power:

- **[experiment]** eliminate the one transient `MODE_SENSE(10)` retry that
  macOS logs during enumeration;
- **[experiment]** keep a `READ_10`/`READ_16` byte/request counter in
  diagnostics for host-side read profiling;
- **do not** add transparent write-capable USB. Raw block writes bypass
  `/.rockpod-rw`; a write mode would need an explicit native-view/overlay-reset
  workflow, not Finder writes.

### 4.7 Diagnostics and measurement

**[code]** `Debug → Disk Info` already reports:

- overlay READY/stage/error/slot/reason;
- v3 index cache HIT/MISS;
- HFS+ I/O reads/writes/flushes;
- SSD detection, spinup time, power-management capability;
- failing overlay entry fields.

This is a good foundation. The paper recommends adding:

- per-operation flush counts (create/close/rename/delete/settings);
- bytes read/written at the HFS+ layer;
- catalog index build time and cache load time;
- USB read request/byte counts;
- bootchart markers around `disk_mount_all`, `hfsplus_mount`, index cache load
  and overlay load.

These are small counters and make the next optimization evidence-driven
instead of guess-driven.

---

## 5. Inherited Rockbox: storage, power, CPU, display

### 5.1 ATA and storage

**[code]** `ipod6g` uses:

- 512-byte logical sectors on the iFlash;
- 4096-byte virtual sectors (`DEFAULT_VIRT_SECTOR_SIZE 4096`);
- 4096-byte maximum physical/virtual sector support;
- a 16 KiB HFS+ bounce buffer;
- ATA DMA and a one-sector cache for partial physical-sector access;
- `storage_flush()` on metadata commits.

**[code]** `disk_spindown` defaults to **0 = never sleep** in
`apps/settings_list.c`. The comment is explicit: iPod flash adapters can fail
to resume after ATA sleep, so the safe default is to stay active. The 6G
driver has a separate SSD path:

- `ata_ssd_mode` is set from `ata_disk_isssd()`;
- `ata_sleepnow()` in SSD mode gates the ATA controller clock instead of
  issuing a full ATA standby;
- after 10 seconds with the backlight off and the controller gated, it cuts
  AUTOLDO for a deeper sleep;
- `ata_power_up()` has a fast clock-gate restore path.

**[code] The zero-timeout coupling is more than a power issue.** With
`ata_sleep_timeout == 0`, the 6G event handler always asserts storage active.
The generic storage thread then sees no driver asking to sleep, so it never
calls `call_storage_idle_notifys(false)` and never sends
`Q_STORAGE_SLEEPNOW`. That means:

- the HDD never spins down;
- the SSD clock-gate/deep-sleep path is unreachable from the tick path;
- deferred settings/status writes (which use storage-idle callbacks) are not
  flushed while idle; they only run at forced points such as clean shutdown,
  config load, USB handoff or reboot.

The first two are power costs; the third is a persistence-timing regression.
**[experiment]** decouple “automatic spindown disabled” from “idle
notification”: keep asserting active for the zero-timeout case, but still run
a periodic idle-notify pass (respecting `call_storage_idle_notifys()`'s
`lock_until`). Honor an explicit `Q_STORAGE_SLEEP` for SSD even when the auto
timeout is zero. Do not simply change the default or make every tick sleep the
disk.

**[code]** `ata_sleepnow()` flushes the ATA cache unconditionally on SSD even
when no writes occurred, and `ata_flush_cache()` ignores readiness errors;
`ata_flush()` always returns 0, so `storage_flush()` cannot propagate an ATA
flush failure to the HFS+ overlay. **[experiment]** track “writes since flush”
to skip redundant flushes and return real errors. This is a correctness
improvement as well as an efficiency one.

**[code]** `ata_power_down()` can be reached from the reset error path without
setting `ssd_deep_asleep`; the SSD fast path in `ata_power_up()` assumes a
clock-gated state when that flag is false. A later power-up could skip the full
PATA/PCON restore. **[experiment]** use an explicit power-state enum and set it
in every power-down path before trusting the fast path.

**[estimate]** This is the single largest non-code power opportunity on an
iFlash device. If `Debug → Disk Info` reports `SSD detected: yes`, setting
`Storage mode: SSD` and a nonzero `Disk spindown` (e.g. 7–30 s) may let the
device enter the clock-gated/deep state during idle playback and browsing.
The risk is resume failure on a particular adapter/card, which is exactly why
the default is 0. It must be tested over many sleep/wake cycles before it
becomes a documented recommendation or default.

**[code] Wake amplification:** `backlight_on()` is called from the clickwheel
ISR, key repeat and every action, and in SSD mode it posts
`Q_STORAGE_PRE_WAKE` each time. `storage_spin()` is called on every WPS/browser
key; on SSD it synchronously powers the ATA up, and a full deep-sleep wake can
take hundreds of milliseconds. With spindown enabled, wheel movement could
therefore cause a wake request per event and a UI stall. **[experiment]**
deduplicate PRE_WAKE, post it only on an off→on transition when storage is
asleep, and make `ata_spin()` queue an asynchronous pre-wake instead of a
blocking power-up in UI context. Measure key-to-screen and key-to-disk latency
before removing pre-spin entirely.

**[experiment] ATA power test:**

1. Confirm `SSD detected: yes` and `Storage mode: SSD`.
2. Record idle current with spindown 0 for 30 minutes.
3. Set spindown 7 s, repeat; then 15 s and 30 s.
4. Between each run, exercise 100 wake/sleep cycles and verify playback,
   metadata writes and clean reboot.
5. Stop at the first resume failure; do not change the default from 0 without
   a multi-device matrix.

**[code]** The generic storage thread wakes every `HZ/2` (500 ms) and sends
`Q_STORAGE_TICK`; the 6G ATA event uses it for spindown timing and SSD deep
sleep. That is a 2 Hz background wakeup even when idle. It is probably small
compared with display/storage power, but if idle profiling shows CPU wakeups
dominating, make the tick adaptive or 1 s while preserving sleep-timeout
accuracy. Do not change it before measuring.

### 5.2 CPU and clocking

**[code]** The S5L8702 clock table has 216/108/54 MHz modes. Boot starts at the
216 MHz mode for init, then `cpu_boost(false)` drops to the 54 MHz default
after the tagcache init path; `cpu_boost()` is reference-counted and returns to
216 MHz for filetree scrolling, tagcache work, PictureFlow and several
plugins. The CPU is therefore already demand-driven.

**Assessment:** do not force 216 MHz for normal playback/browsing, and do not
add a background “performance” mode. The useful CPU work is reducing wakeups
and avoiding repeated scans, not raising clock speed.

**USB exception:** `usb_slave_mode(true)` calls `trigger_cpu_boost()` and sets
the USB thread to `PRIORITY_REALTIME`; the boost is cancelled only when
`usb_slave_mode(false)` runs. A mass-storage session therefore stays at
216 MHz even while idle. `set_ahb_boost()` already exists for a 108 MHz AHB
mode without forcing the CPU to 216 MHz, but only the `false` path is currently
used. **[experiment]** try `set_ahb_boost(true)` for USB, or boost only during
active transfers, and measure large-file throughput, macOS enumeration and USB
audio. Do not lower it without those measurements.

### 5.3 Backlight, LCD and display

**[code]** Defaults:

- backlight timeout: 15 s (`DEFAULT_BACKLIGHT_TIMEOUT`);
- brightness: `0x20` (`DEFAULT_BRIGHTNESS_SETTING`);
- LCD sleep after backlight off: 5 s;
- LCD static and double buffers: 150 KiB each.

`ipod6g` does **not** define `CONFIG_BACKLIGHT_FADING`; the fade code in
`backlight.c` is compiled out. Transitions are immediate, so there is no
fade-time setting to tune on this target.

**Non-code opportunities (user settings, no firmware change):**

- lower brightness if the display is comfortable;
- shorten backlight timeout to 5–10 s;
- keep LCD sleep enabled;
- use a dark, low-motion theme for browsing.

These are user-visible trade-offs, not defaults to change silently. A 15 s
backlight is a reasonable default; the biggest wins are for users who leave
the player idle on a menu.

**[code]** `backlight_on()` is called from the clickwheel ISR, key repeat and
every action. In SSD mode it also posts `Q_STORAGE_PRE_WAKE`; `backlight_hw_on()`
writes PMU register `0x29` through a full I2C transaction even when the
backlight is already on. Wheel movement can therefore cause a PMU write (and a
storage wake request) per event. **[experiment]** cache the last hardware
backlight state/brightness in `backlight-6g.c`, write only on a real off→on or
brightness change, and post `Q_STORAGE_PRE_WAKE` only on an off→on transition
when storage is actually asleep. Keep a forced path for init, LCD sleep and
shutdown.

**[code]** `settings_apply()` calls `lcd_update()` unconditionally, even when
the display flip did not change; at boot this repeats a full-screen transfer
already done by `show_logo_boot()`. Minor boot-time saving, not a power item.

### 5.4 Poweroff, sleep timer and charging

**[code]** `poweroff` defaults to 10 minutes; the sleep timer is available but
off by default. Charging behavior is target-specific.

**Operational advice:** use the sleep timer for bedtime listening; do not
disable idle poweroff; avoid leaving the backlight on while charging unless
needed. Battery health and charge voltage are outside this paper’s scope.

**[code]** `lineout_active` and `accessory_supply` both default **on**. Lineout
powers the CS42L55 LINA/LINB amplifiers; accessory supply powers LDO6 for a
remote/Mikey. Every volume change also performs two read-modify-write I2C
operations for a lineout volume that never changes. **[experiment]** measure
current with each rail off and cache the lineout volume; do not change defaults
without dock/remote testing.

**[code]** The power thread samples the PMU ADC at 2 Hz. `pmu_read_adc()` has
no timeout in its `ADCS3` polling loop, and `pmu_read()` ignores I2C errors and
can return an uninitialized value. **[experiment]** add bounded retries/error
propagation and consider 1 Hz sampling when not charging and the voltage is
stable. Test the low-battery and charger transitions before slowing it.

### 5.5 Startup ordering and boot charts

**[code]** The firmware boot path is approximately:

```text
system/LCD/backlight/button/powermgmt init
  -> storage_init
  -> early USB check
  -> disk_mount_all
       -> hfsplus_partitions
       -> hfsplus_mount
            -> hfs_ro_mount
            -> overlay_load
            -> index cache load or full catalog build
  -> settings_load / settings_apply
  -> dircache (disabled for HFS+ builds)
  -> tagcache_init
  -> audio_init / playlist / filetype
  -> usb_init / usb_start_monitoring
```

`apps/main.c` already has `CHART()` markers around most of these phases. A
diagnostic `DO_BOOTCHART` build is the cheapest way to attribute boot time
before changing mount or cache code. The first UI frame and overlay READY are
separate milestones; report both.

**[experiment]** Add CHART markers around the v3 index-cache load and the
overlay entry-table read so a slow cache miss is visible without a profiler.

---

## 6. Inherited Rockbox: filesystem, caches and applications

### 6.1 Disk cache and FAT

**[code]** Rockbox has a 64-entry × 4 KiB disk cache (256 KiB) and a FAT
bounce area of 4 volumes × 10 × 4 KiB (160 KiB). The HFS+ reader bypasses the
generic disk cache and uses its own 16 KiB `hfs_io` bounce buffer plus a
32 KiB catalog node cache.

**Assessment:** bypassing the generic disk cache for HFS+ is good: it avoids
double caching and keeps the HFS+ layer’s validation semantics explicit. Do
not route HFS+ through the FAT disk cache without a measured reason.

**No dircache means linear path resolution.** **[code]** HFS+ builds exclude
`HAVE_DIRCACHE`, so `open_path_component()` resolves each path component by
scanning the parent directory with `readdir` and `strcasecmp`. The HFS+
adapter’s parent/CNID index narrows the starting node but does not provide a
binary name lookup to generic code. Every album-art probe, tagcache filename
lookup, playlist existence check and file open pays this cost. **[experiment]**
add an adapter `lookup(parent, name)` hook that binary-searches the catalog
B-tree key, with the readdir loop as fallback; or add a small read-only
positive/negative path cache invalidated on create/remove/rename/mount. Do not
enable dircache for HFS+ blindly: the guard is deliberate and overlay
namespace/write invalidation is not implemented.

**Latent resource risk:** `MAX_OPEN_FILES + MAX_OPEN_DIRS` is 63, and
`DC_NUM_ENTRIES` is 64 with one reserved client buffer. Three internal
`AUX_FILEOBJS` opens can theoretically exceed the cache buffers; `file_cache_alloc()`
panics if no buffer is available. Normal use is unlikely to hit this, but a
stress test and a graceful failure path are worth adding.

**RAM-only opportunities:** `NUM_VOLUMES_PER_DRIVE` is 4, so the FAT bounce
area is 160 KiB even for one HFS+ data partition; reducing the per-drive
volume count could save about 120 KiB, but multi-partition/FAT fallback must
keep working. `logfbuffer` is 256 KiB; a release build could shrink or disable
`ROCKBOX_HAS_LOGF` at the cost of field diagnostics. Neither is a power win by
itself; both free RAM for tagcache/artwork experiments.

**Opportunity:** **[experiment]** on an HFS+-only build, the 256 KiB disk cache
and 160 KiB FAT bounce area are mostly fallback capacity. Reducing
`DC_NUM_ENTRIES` or `FAT_BOUNCE_SECTORS` would free RAM for tagcache/artwork,
but it would slow FAT fallback and may affect boot-time partition scanning.
Measure before changing; RAM is not currently the binding constraint.

### 6.2 Dircache and tagcache

**[code]** `HAVE_DIRCACHE` is disabled for HFS+ builds by
`firmware/export/config.h` (`!defined(HAVE_HFSPLUS_RO)`). The HFS+ reader’s
index is the replacement. `HAVE_TC_RAMCACHE` is enabled, but
`tagcache_ram` defaults to **off**, and `tagcache_autoupdate` defaults to
**false**.

**Why:** the supported workflow builds the database on the Mac. On-device
tagcache scans are slow, write to the overlay/native files, and are explicitly
unsupported as the primary workflow.

**The HFS+-specific tagcache trap:** `tagcache_find_index()` uses
`find_entry_ram()` only when both `HAVE_TC_RAMCACHE` and `HAVE_DIRCACHE` are
defined. HFS+ builds disable dircache, so it falls back to
`find_entry_disk(filename, localfd=true)`, which resets `last_pos` and scans
the filename tag file from the beginning. With `runtimedb` or `autoresume`
enabled, that scan can happen for **every buffered track**. Both settings
default to off; keep them off until a filename→idx hash or persistent
`last_pos` path exists.

**Other tagcache costs:**

- `tagcache_fill_tags()` is compiled out without dircache, so the playlist
  viewer falls back to `get_metadata_ex()` and parses each visible row from
  disk.
- The database browser’s chunk loader starts each chunk at offset 0 and
  retrieves rows before skipping them; large tables can become effectively
  quadratic in scroll latency.
- RAM-cache loading reads the master index one entry at a time and tag
  headers/data one at a time; batching uses the existing 64-entry index buffer.
- On-device `tagcache_autoupdate` / Update Now performs a full tree walk,
  two `database.ignore` probes per directory, a filename lookup per file and a
  `file_exists()` walk per existing entry. It is the slowest path in the
  inherited stack and should stay off.

**Opportunities:**

- **[experiment]** Try `tagcache_ram = on` (or `quick`) for large libraries.
  It can make database browsing faster, but consumes RAM that may otherwise be
  used by artwork/plugins. `allocate_tagcache()` already refuses an allocation
  that would leave less than 1 MiB for the audio buffer, plus a 32 KiB reserve;
  measure boot time, browse latency and free RAM before changing the default.
  Because `HAVE_DIRCACHE` is disabled for HFS+ builds, the dircache-specific
  filename fast path in `tagcache` is not compiled; measure whether the RAM
  cache still helps enough to justify its memory.
- **[experiment]** Add a “database cache hit/miss” or load-time counter to
  diagnostics so users can see whether the Mac-built database is being used.
- **[experiment]** Add a filename→idx hash (or persistent filename fd plus
  `last_pos`) for `tagcache_find_index()`/`tagcache_fill_tags()`. This is the
  single highest-value application fix if runtime DB/autoresume are important.
- **[experiment]** Preserve the tagcache search cursor between browser chunks,
  or add a skip-without-retrieve API.
- **Keep autoupdate off** unless the user explicitly wants an on-device scan.
  A background scan is a battery and write-amplification trap.

### 6.3 Settings persistence

**[code]** `settings_save()` is already deferred and change-aware:

- it unregisters the status flush and registers a storage-idle callback;
- `flush_config_block_callback()` writes only when `settings_crc_changed()`;
- the config is written through a temp file and promoted;
- the runtime/status update is also deferred to storage idle.

**Assessment:** do not add synchronous settings writes or a “save on every
keypress” path. The current design is the right one. However, with the default
`disk_spindown = 0`, the driver always asserts storage active, so the automatic
storage-idle callback path does not run; settings/status writes are forced only
at shutdown/config load/USB/reboot. That is a persistence-timing caveat, not a
reason to make every write synchronous. Fix the idle-notify coupling in §5.1
and measure the resulting write frequency.

### 6.4 File browser, metadata and album art

**[code]** The file browser boosts the CPU while scrolling and parses metadata
on demand. It calls `dir_get_info()` and converts mtime for every entry before
filtering, and extension lookup is a linear scan of up to 192 file types per
entry and per redraw. A 5,000-entry directory can do on the order of a million
string comparisons in `ft_load()`; this is CPU-only but easy to remove.
**[experiment]** compute mtime only for surviving rows, and store the filetype
index in the entry for color/icon/support checks. Keep `talk_file_clip` off;
when enabled it does a second directory scan and O(n²) thumbnail matching.

**[code]** Album art is buffered through `artwork_buffer` (about 70 KiB) and
PictureFlow has its own cache in `.rockbox/rocks/demos/pictureflow`.

**[code]** Album art defaults to `Prefer embedded` (`album_art = 1`). Embedded
art avoids the external file search when the tag contains art; that is the
better default for performance. If the library relies on external files, the
search is expensive: `find_albumart()` can issue roughly **25–37
`file_exists()` probes per track** across sized/unsized passes and
track/album/cover/folder/parent-directory variants. Without dircache each probe
walks path components through a linear `readdir` scan.

**Operational opportunities:**

- keep `Prefer embedded` unless the library has no embedded art;
- if using image files, use a 128×128 `cover.jpg` and the host PictureFlow
  cache;
- avoid multi-megapixel covers for the iPod to decode;
- avoid PictureFlow “Rebuild Cache” on the device;
- use `Show Files: Supported`/`Music` filters to reduce directory noise;
- keep directories reasonably sized; HFS+ B-trees handle many entries, but
  each directory listing still walks records.

**Code opportunities:**

- **[experiment, high value]** Cache the album-art search result **per
  directory, including negative results**, and reuse it for every track in the
  folder and size slot. Invalidate on create/rename/remove, overlay changes,
  USB mount/unmount or reboot; a stale negative cache can hide newly added art.
- **[experiment]** Add a direct `cover.jpg`/`folder.jpg` fast path before the
  long name matrix. Make it configurable if track-specific art must win.
- **[experiment]** Move art path resolution off the audio thread; currently it
  runs in `audio_finish_load_track()`.
- **[experiment]** If profiles show repeated album-art decodes during normal
  playback, add a small decoded-art cache keyed by track or album. The existing
  `artwork_buffer` is a single buffer; a multi-entry cache costs RAM and needs
  an eviction policy.

**[code]** The host PictureFlow builder
(`tools/rockpod/build_rockpod_pictureflow_cache.py`) pre-scales covers to
128×128 RGB and packs RGB565 `.pfraw` files on the Mac. The index builder
(`build_rockpod_coverflow_index.py`) reads the committed database and never
scans audio tags on the iPod.

### 6.5 Playback, playlists and codecs

**[code]** `CODEC_SIZE` is 1 MiB and `PLUGIN_BUFFER_SIZE` is 3 MiB. The
codec/audio buffers are inherited and tuned. Playback at 54 MHz is already the
expected path; codec boost is demand-driven.

**Application-level multipliers:**

- **Track load** checks paths more than once: `playlist_peek()` calls
  `file_exists()`, `audio_load_track()` checks and opens again, `bufopen`
  opens for metadata, and the buffering thread reopens to parse. That is
  roughly 2–3 path resolutions plus 2–3 opens per track before audio data.
  **[experiment]** trust internally generated playlists and let the open fail
  for bad external paths; test malformed M3U behavior.
- **Playlist viewer** has a custom change that always retrieves tags from
  metadata/DB, ignoring the “track name/full path” display setting. Without
  `tagcache_fill_tags()` this parses a file per visible row. **[experiment]**
  respect the display setting, reuse the current `mp3entry`, or use the
  tagcache title path.
- **Recursive playlist insert** reloads the parent directory after every
  subdirectory. For a wide library this is one parent scan per child.
  **[experiment]** snapshot the remaining subdirectory names before recursing
  so the parent is read once.
- **Stream cache** fills one logical sector per miss even though the cache
  entry is 4 KiB. **[experiment]** add a read-only multi-sector read-ahead
  window; do not apply it to writable streams without a dirty-range design.
- **Polling loops:** storage wakes at 2 Hz, tagcache at 1 Hz, buffering and
  audio at 2 Hz while active. These are small compared with disk I/O but are
  measurable idle wakeups; make the tagcache timer event-driven after
  `check_done` first, then measure before touching audio/buffering.
- **PCM DMA** uses one LLI per chunk (`CHUNK_MAX_BYTES = LLI_MAX_BYTES * 1`).
  A two-LLI option could roughly halve PCM DMA callbacks, but test underruns
  and transfer granularity first.

**Assessment:** do not trade audio buffer margin for a small RAM win. Buffer
margin protects playback from storage latency; a skip is a worse outcome than
a few hundred kilobytes of RAM. Profile codec CPU only if playback battery is
anomalous after storage/display tuning.

### 6.6 Plugins, themes and UI

**[code]** PictureFlow has a target-specific “~2× faster cover animation”
change and boosts the CPU while active. Themes/fonts and album art affect both
CPU and LCD power.

**Opportunities:**

- prefer simple, dark themes with fewer full-screen redraws;
- avoid large fonts/bitmaps if not needed;
- close plugins when not in use; PictureFlow’s cover thread and cache are
  expensive to keep resident;
- do not benchmark battery with a plugin running unless that is the workload;
- **PictureFlow with the host-built index** is cheap: one index read plus a
  32 KiB `.pfraw` read per slide. The fallback without an index performs
  per-album tagcache searches, metadata parsing, art search and decode — avoid
  it. **[experiment]** extend the host index with compact per-album track
  lists, or enable `tagcache_ram`, so per-album track resolution is a memory
  read instead of a database scan.
- **Tagtree uniqueness** uses a 64 KiB linear list; large artist/album sets can
  become O(U²). Replace with an open-addressed hash set if profiles show DB
  browser latency. Similarly, resolving one filename from an album row can
  load the entire child table; resolve directly when possible.

---

## 7. Bootloader

**[code]** `bootloader/ipod-s5l87xx.c` mounts native HFS+ only. It does not
load the overlay, build the full directory/CNID index, or expose the read-only
USB MBR presentation. That is correct: the bootloader only needs to find and
load `/.rockbox/rockbox.ipod`.

**[host]** Bootloader `.text` is 90,024 B and the image is 103,420 B;
`MOVE_AREA` leaves 15,360 B of code headroom after the 10,240 B stack. BSS is
2,017,856 B, of which `hfs_states` is 1,298,304 B. HFS+ code is 22,600 B
(25% of `.text`), including write-path modules that the bootloader never
executes: `hfsplus_overlay_alloc.o` 2,078 B, `hfsplus_overlay_store.o`
2,040 B, and `hfsplus_overlay_format.o` 1,248 B. The overlay loader itself is
already compiled out.

**Opportunities:**

1. **[experiment, low risk]** Give the bootloader a minimal `hfs_state`: omit
   or stub the 707,768-byte overlay member under `BOOTLOADER`, and remove the
   `memset(&s->overlay, 0, sizeof(...))` in `hfsplus_mount_overlay()`. This
   removes a 708 KiB boot-time memset and the reason the overlay BSS exists.
   The full 1.24 MiB `hfs_states` would shrink substantially.
2. **[experiment, medium risk]** Guard the overlay write entry points reachable
   from `hfsplus_fat.c` with `#ifndef BOOTLOADER`. The addressable text is
   about 5,366 B; headroom could rise from 15,360 to about 20.7 KB. Keep the
   read paths and native HFS+ validation intact.
3. **[experiment, high risk]** `HAVE_BOOTLOADER_USB_MODE` costs roughly 11 KB
   (USB storage, DesignWare and core). Removing it is the largest single
   bootloader saving but conflicts with the documented recovery/charging path.
   Do not remove it without a recovery test plan.
4. **[experiment, build safety]** `--thumb` is implemented by
   `tools/thumb-cc.py`, which silently retries without `-mthumb` if a Thumb
   compile fails. That can produce an ARM bootloader with no error. Make the
   fallback loud or fail unless explicitly allowed; add an `ASSERT` for the
   `MOVE_AREA` code budget in `firmware/target/arm/s5l8702/boot.lds`.
5. **Do not** try to enable function sections in the bootloader: it already
   uses `-ffunction-sections -fdata-sections` and `--gc-sections`. The
   remaining size is real code and data, not a linker oversight.

---

## 8. Build, linker and host tooling

### 8.1 Compiler/linker flags

**[code]** The generated build uses:

```text
-Os -ffreestanding -fomit-frame-pointer -funit-at-a-time
-fno-delete-null-pointer-checks -fno-strict-overflow -fno-common
-mcpu=arm926ej-s
```

and links core with `-Wl,--gc-sections`. The bootloader additionally uses
`-ffunction-sections -fdata-sections`; the firmware core does not.

**[host]** This was measured, not assumed:

- firmware `--gc-sections` reclaimed **0 bytes** because the core objects are
  single `.text` sections; the "Discarded input sections" list contains 520
  entries, all zero-size.
- an upper bound of 69 unreferenced symbols / 4,680 B exists, mostly libgcc
  soft-float helpers and unused vector stubs.
- the bootloader, which does use function sections, discards 2,140 B.

**Conclusion:** do **not** enable function/data sections in the firmware core
for size; the measured upside is below 1%, and it adds build cost and section
placement risk. The bootloader already has it.

**Remaining build opportunities:**

- **[experiment]** Try `-O2` for the HFS+/disk paths only, or for the whole
  firmware, in a separate build. The image is read once (823 KB at ATA speed),
  DRAM has ample slack, and ARM926 at 54 MHz may benefit more from speed than
  from size. Measure boot time, binary size and I-cache behavior; do not
  replace `-Os` without data.
- **[experiment]** LTO (`-flto`) is available in `tools/configure` but off by
  default. Treat it as an experiment, not a default; GCC 9.5 with a
  freestanding ARM link script may expose section/order issues.
- **[code]** Keep a size budget in the release runbook: firmware `.text`/`.bss`
  and bootloader `_moveend` with a warning threshold. The bootloader has only
  15,360 B of code headroom.
- **[code]** Add a `size-report` helper that prints the top 20 symbols, IRAM
  use and bootloader headroom after each build.
- **[operational]** Install and document `ccache` (`tools/configure` already
  auto-detects it) to cut incremental build time. It is currently absent on
  the build host.

### 8.2 Release packaging

**[host]** The current `rockbox.zip` is 19,096,468 B, 43,718,354 B
uncompressed, with 554 files. Two optional payloads dominate:

- the four prebuilt fonts for the optional `Themify_2` theme total 21.7 MB
  uncompressed and 9.69 MB compressed — **51% of the compressed zip**;
- the three 3D-game overlays (`duke3d`, `quake`, `wolf3d`) add about 3.0 MB
  uncompressed.

A `--no-bundled-themes` / `ROCKPOD_THEMES=` switch in `tools/buildzip.pl`
could ship the core firmware at roughly half the download size and move the
theme/game payloads to optional downloads. Keep the current default until the
owner decides; this is a packaging decision, not a runtime change.

**Release hygiene opportunities:**

- add a guard that the `rockbox.ipod` embedded in `rockbox.zip` has the same
  SHA-256 as the release `rockbox.ipod` (the current build tree can contain a
  stale zip after a firmware rebuild);
- add `-x '*.DS_Store' -x '*/._*'` to the final zip command;
- document that `tools/release/bins.pl` is not usable on macOS (`nproc`);
- keep the bootloader size/`_moveend` values in `shipping.md` generated from
  the map, not hand-copied.

Firmware payload is dominated by `apps/other` 313,792 B, `libfirmware`
188,004 B, `rbcodec` 63,372 B and IAP 43,816 B. The boot splash bitmap is
62,744 B and the USB logo 16,920 B. These matter for download/flash size and
I-cache, not for idle power; do not remove IAP or the debug menu without an
owner decision. The debug menu is the diagnostic surface for overlay
stage/reason/slot.

### 8.3 Host database and artwork tools

**[code]** `tools/rockpod/build-database-macos.sh` configures a host build,
patches the generated Makefile for clang, and builds `database.ipod6g`.
`build_rockpod_pictureflow_cache.py` uses FFmpeg to decode/resize covers and
writes `.pfraw` files; `build_rockpod_coverflow_index.py` reads the committed
database and builds the index without scanning tags.

**Opportunities:**

- **[operational]** Run the database build once after a library change, not on
  every connection. Keep the build log and backup.
- **[experiment]** Add an incremental manifest/hash so the PictureFlow builder
  skips unchanged covers; it already stages outputs, but a content hash avoids
  re-encoding the whole library.
- **[experiment]** Parallelize cover conversion with a bounded worker pool
  (FFmpeg is the bottleneck); keep deterministic output ordering. Replace the
  per-pixel Python `struct.pack` loop with a single `bytearray`/`array('H')`
  write; this is CPU-only but noticeable on a large library.
- **[experiment]** Add a `--check` mode that validates the committed database
  and cache without writing, useful before a long trip.
- **[experiment, medium risk]** Make `configure --type=d` emit a native macOS
  host tool without SDL2 and without the six `sed` substitutions in
  `build-database-macos.sh`. This removes a recurring breakage and a misleading
  dependency.
- **[experiment]** Build the database tool with `-O2`; it scans whole music
  libraries on the Mac and does not need ARM size optimization.

### 8.4 Tests and build time

**[host]** The HFS+ suite is fast (tens of tests in seconds) and ASan/UBSan is
practical. The firmware build is the slow part.

**Opportunities:**

- keep the host suite as the first gate; it catches overlay/flush regressions
  without a device;
- add a high-water regression test (done) and a per-operation flush test;
- cache object files between builds; avoid `make clean` unless changing
  configuration;
- run the bootloader build only for shared changes, but always run it before a
  release;
- **[experiment]** de-duplicate the adapter source list used by
  `tools/hfsplus/Makefile`, `tools/hfsplus/tests/check_integration.py` and
  `firmware/SOURCES`; build shared objects once instead of recompiling the
  same 15 files in three modes;
- **[code]** add one `tools/rockpod/check.sh` gate for host tests, sanitize,
  integration, Python syntax and size/headroom, so a release cannot forget a
  step;
- **[code]** move the `$(shell rm -f $@)` in `firmware/firmware.make` into the
  recipe body so `make -n` cannot delete `libfirmware.a`.

---

## 9. Non-code and operational upgrades

These require no firmware change and are often the best return per minute.

### 9.1 Power settings matrix

| Setting | Current default | Candidate | Trade-off |
|---|---|---|---|
| Disk spindown | 0 (never) | 7–30 s with SSD mode | Resume reliability; must test |
| Storage mode | Auto | SSD if detected | Wrong mode may hurt; verify |
| Backlight timeout | 15 s | 5–10 s | User comfort |
| Brightness | 0x20 | Lower if comfortable | Readability |
| LCD sleep after backlight | 5 s | Keep/shorten | None if screen is off |
| Idle poweroff | 10 min | Keep | Convenience |
| Lineout active | On | Measure off if not docking | May break dock audio |
| Accessory supply | On | Measure off if no accessory | May break remote/Mikey |
| USB mass-storage CPU | 216 MHz boost | 108 MHz AHB experiment | Throughput/audio risk |
| Tagcache RAM | Off | On/quick for large libraries | RAM vs browse speed |
| Tagcache autoupdate | Off | Keep off | On-device scans are expensive |
| Runtime DB / autoresume | Off / off | Keep off until filename hash | Full tag-file scan per track |
| Album art | Prefer embedded | Keep embedded unless no embedded art | External file search is expensive |
| Cuesheet | Off | Keep off unless needed | 1–3 path probes per track |
| `talk_file_clip` | Off | Keep off | Second directory scan + O(n²) matching |
| Next-folder advance | Off | Keep off unless needed | Can trigger recursive tree scans |
| Recursive dir insert | On | Fix parent reload or turn off for huge trees | Parent scanned once per subdirectory |
| Playback log | Off | Keep off | One append per track |
| Max files in dir | 5000 | Lower only if directories are known small | Truncation/OOM trade-off |
| Antiskip margin | 5 s | Keep on HDD; SSD code uses 1 s | Startup latency vs skip resilience |
| PictureFlow update art | Off | Keep off; build on Mac | On-device rebuild is expensive |

**The spindown/SSD experiment is the highest-value non-code test.** It must be
done with a resume stress test before being recommended.

### 9.2 Library and filesystem layout

- Keep the native library as ordinary HFS+ files; do not put bulk music in the
  overlay.
- Keep the overlay for settings, playlists, databases and generated artwork.
- Use 4 KiB HFS+ allocation blocks (the current format) and keep the volume
  cleanly unmounted.
- Avoid directory names with decomposed/composed Unicode ambiguity; lookup is
  exact-byte.
- Keep names under the documented limits; overlong names are skipped.
- Avoid giant single directories when a simple Artist/Album split works; the
  HFS+ B-tree is fine, but every listing still walks records.
- Keep `cover.jpg` at the target resolution; do not store multi-megapixel art
  for PictureFlow.
- Do not use on-device Database Initialize/Update or PictureFlow Rebuild Cache
  as the normal workflow.

### 9.3 Hardware and adapter

- Use a reputable iFlash adapter and card; card quality affects latency,
  power and resume behavior.
- Record the adapter/card model when testing spindown or deep sleep.
- Prefer a card with good random-read latency; the HFS+ catalog walk and
  tagcache benefit more from latency than sequential throughput.
- Keep the battery healthy; a degraded battery dominates all software wins.
- Use a known-good cable/port; a flaky connection can look like a storage
  bug.
- Do not format/restore/repartition to “fix” a performance problem.

### 9.4 Workflow

- Use Apple Disk Mode for writes, backups, database builds and repairs.
- Use Rockbox read-only USB for diagnostics and copying files off.
- Build the database and PictureFlow cache on the Mac, then eject cleanly.
- Keep a backup of `/.rockpod-rw` before any format/migration experiment.
- Measure one change at a time; record the workload and battery state.

---

## 10. Prioritized roadmap

### P0 — safety and evidence

1. **Power-loss matrix** for create, append, close/fsync, rename, delete,
   settings save and cache install. The high-water batching changes header
   timing; re-run it.
2. **Device baseline** for boot time, mount reads/bytes, cache HIT/MISS,
   flush counts per operation, and idle/playback current.
3. **Native HFS+ integrity check** after a stress run: catalog/allocation/
   journal unchanged; `diskutil verifyVolume` clean.

### P1 — highest leverage

4. **[done host]** v3 high-water 64-slot batching.
5. **Spindown/SSD experiment plus idle-notify decoupling**: keep the safe
   default, but make explicit sleep and idle callbacks reachable with
   `disk_spindown = 0`.
6. **Per-operation flush counters** in diagnostics.
7. **Realloc/materialize 16 KiB copy chunks**.
8. **Album-art folder result cache** (positive and negative), with
   invalidation.
9. **Runtime DB/autoresume filename hash** or persistent filename fd/last_pos.
10. **Adapter name lookup or read-only path cache** to remove linear path
    resolution without enabling dircache.
11. **Wake-amplification gating**: deduplicate PRE_WAKE, post it only on
    off→on when storage is asleep, make `ata_spin()` asynchronous.
12. **Backlight PMU write cache** and off→on-only hardware update.

### P2 — structural wins

13. **Tagcache fill/title path without dircache** and browser cursor
    preservation.
14. **Remove redundant track-load path/open checks**; respect playlist-viewer
    display mode.
15. **Read-only stream read-ahead** (4 KiB window) and batched tagcache RAM
    load.
16. **Readdir parent-bucket walk** instead of 2,048-slot linear scan.
17. **Two-pass duplicate-name resolution** with legacy fixtures.
18. **Bound validator/rebuild loops by high-water** for v3.
19. **Bootloader minimal state**: omit the 708 KiB overlay memset/member under
    `BOOTLOADER`, guard unused write entry points, add the `--thumb` fallback
    guard and `MOVE_AREA` budget assert.
20. **Release packaging**: optional `Themify_2` fonts and 3D-game overlays to
    roughly halve the zip; add embedded-firmware hash and `.DS_Store` guards.
21. **Tagcache RAM / browse-latency experiment**.
22. **Firmware `-O2`/LTO experiment** in a separate build; measure boot time,
    size and I-cache behavior before changing `-Os`.
23. **Filetype/mtime/file-browser CPU cleanup** and recursive playlist parent
    reload.
24. **Tagtree uniq hash** and direct filename resolution for album rows.
25. **Lineout/accessory current measurement** and lineout-volume write cache.
26. **USB 108 MHz AHB / transfer-only boost experiment**.
27. **PMU ADC timeout/error handling** and 1 Hz stable sampling experiment.
28. **ATA flush error propagation** and explicit SSD power-state enum.

### P3 — format and high-risk work

29. **WAL/commit-group** for bitmap+entry flush coalescing; only with the
    power-loss matrix.
30. **v2→v3 migration** with backup/rollback.
31. **Index-cache size/compression** experiment.
32. **Catalog node LRU** if profiles justify it.

### Non-code quick wins

33. Backlight/brightness and LCD sleep settings.
34. Mac-side database/PictureFlow incremental builds.
35. Card/adapter documentation and resume testing.
36. A “measure before/after” checklist and `check.sh` in the release runbook.
37. Install/document `ccache` and a size-report helper.

---

## 11. Measurement protocol

### 11.1 Boot time

- Build with `DO_BOOTCHART` for a diagnostic run; use the `CHART` markers
  around `disk_mount_all`, `hfsplus_mount`, index-cache load and overlay load.
- Record 5 cold boots and 5 warm reboots, with a small and a large library.
- Report time to first UI frame and time to overlay READY separately.

### 11.2 Mount I/O

- `Debug → Disk Info` reports HFS+ reads/writes/flushes and cache HIT/MISS.
- Record counters immediately after mount, before browsing.
- If counters reset per mount, capture them with a photo/serial log.
- Report bytes, not just request count.

**Application probes to add before optimizing the app layer:**

- `open_path_component()` readdir entries scanned and `file_exists()` calls;
- album-art probes per track and positive/negative folder-cache hits;
- `tagcache_find_index()` bytes scanned and calls;
- tagtree chunk offset and skipped rows;
- file-cache sector fills and `fat_readwrite` calls.

These are CPU/disk-multiplier metrics; host counters can validate the logic,
but only the device can prove the power effect.

### 11.3 Flush counts

Use the host adapter and the device:

- create 1, 10, 64, 256 files;
- append 1 MiB, close, rename, delete;
- change one setting, save, power off;
- record `hfsplus_io_flushes()` before/after each operation.

The high-water regression already checks the 2,048-create bound on the host.
Add per-operation counters before optimizing further.

### 11.4 Write amplification

- Create a 1 MiB overlay file and count HFS+ writes/flushes.
- Compare append-only, random 4 KiB writes, grow-by-realloc and truncate.
- Report metadata bytes vs data bytes.

### 11.5 Power

- Use a USB power meter or a battery-drain script over a fixed workload:
  idle menu, browse, playback with backlight off, playback with backlight on,
  PictureFlow.
- 30 minutes per run, same starting battery voltage, same card, same
  brightness.
- Report current and projected hours; do not extrapolate from a 2-minute run.

**Counters to add before changing defaults:**

- storage wake graph: `Q_STORAGE_PRE_WAKE`, `Q_STORAGE_SLEEP`,
  `Q_STORAGE_SLEEPNOW`, `ata_power_up`, `ata_power_down`,
  `ssd_deep_asleep`, and the caller (wheel, repeat, action, buffering,
  backlight);
- `call_storage_idle_notifys(false/true)` and each registered callback;
- config/status writes and their timestamps;
- PMU writes by register and `pmu_read_adc()` calls/poll iterations;
- CPU residency at 216/108/54 MHz for idle menu, playback screen off, wheel
  browsing, USB mass storage and charging.

### 11.6 Resume reliability

- With spindown/SSD mode, run 100 sleep/wake cycles.
- After each cycle: play 10 seconds, browse one directory, save one setting.
- Stop at the first failure and record adapter/card/firmware.

### 11.7 Acceptance gates

- No native HFS+ metadata changes.
- No overlay entry loss or duplicate live name.
- No block reuse while a live entry points at it.
- No boot regression beyond an agreed threshold.
- No resume failure in the stress test.
- Host suite and ASan/UBSan pass.

---

## 12. Appendix: key references

### Custom HFS+

- `firmware/hfsplus/hfsplus_mount.c` — partition scan, mount lifecycle,
  USB MBR geometry cache.
- `firmware/hfsplus/hfsplus_partition.c` — MBR/GPT/APM/raw scanner.
- `firmware/hfsplus/hfsplus_ro.c` — catalog reader, index build/export/import.
- `firmware/hfsplus/hfsplus_overlay_load.c` — header/bitmap/entry load.
- `firmware/hfsplus/hfsplus_overlay_validate.c` — whole-overlay validation.
- `firmware/hfsplus/hfsplus_overlay_store.c` — entry lifecycle, high-water.
- `firmware/hfsplus/hfsplus_overlay_alloc.c` — bitmap, runs, growth,
  materialization.
- `firmware/hfsplus/hfsplus_namespace.c` — native+overlay merge/readdir.
- `firmware/hfsplus/hfsplus_fat.c` — FAT/VFS boundary, close/flush.
- `firmware/hfsplus/hfsplus_index_cache.c` — v3 index cache.
- `firmware/hfsplus/hfsplus_state.h` — BSS layout and counters.
- `firmware/usbstack/usb_storage.c` — read-only LUN, MBR view, MODE SENSE.

### Inherited Rockbox

- `apps/main.c` — init order and CHART markers.
- `apps/settings.c` / `apps/settings_list.c` — defaults and deferred saves.
- `firmware/target/arm/s5l8702/ipod6g/storage_ata-6g.c` — ATA, SSD sleep.
- `firmware/target/arm/s5l8702/system-s5l8702.c` — CPU frequency.
- `firmware/backlight.c` — backlight/LCD timers.
- `firmware/common/disk_cache.c`, `firmware/common/fat.c` — disk/FAT caches.
- `apps/tagcache.c` — tagcache and RAM cache.
- `apps/plugins/pictureflow/pictureflow.c` — PictureFlow cache/boost.
- `bootloader/ipod-s5l87xx.c` — native-only boot path.

### Build/tools

- `tools/root.make` — global compiler/linker flags.
- `build-ipod6g/Makefile`, `build-ipod6g-bootloader/Makefile` — generated
  target flags.
- `build-ipod6g/rockbox.map`, `build-ipod6g-bootloader/bootloader.map` —
  section/symbol sizes.
- `tools/rockpod/build-database-macos.sh` — host database build.
- `tools/rockpod/build_rockpod_pictureflow_cache.py` — host cover cache.
- `tools/rockpod/build_rockpod_coverflow_index.py` — host index.
- `tools/hfsplus/tests/` — reader/partition/adapter tests.

---

## 13. What is already efficient

Do not re-litigate these without a measured regression:

- **Container writes** no longer revalidate the native allocation bitmap on
  every write; stage 2 validates the immutable container once.
- **v3 index cache** avoids the full catalog walk on a generation match.
- **v3 high-water** bounds entry-table reads; this paper adds 64-slot batching
  to remove the per-create header flush.
- **Native readdir** uses the HFS+ directory index; overlay visibility uses the
  parent hash for hidden-base checks.
- **HFS+ bypasses the generic disk cache**, avoiding double caching and keeping
  its validation semantics explicit.
- **Node offset tables** are validated once per catalog node read, not per
  record.
- **Settings saves** are deferred, CRC-checked and idle-driven in design; the
  default zero-spindown suppresses the idle callback path, so treat automatic
  flushing as broken until §5.1 is fixed.
- **CPU boost** is reference-counted and demand-driven; the default drops to
  54 MHz after init.
- **Host database/PictureFlow builders** do the expensive tag/artwork work on
  the Mac, not on the iPod.
- **Disk cache/stream cache** use a 64-entry LRU, per-object 4 KiB stream
  buffers shared across duplicate opens, and full-sector reads bypass the
  cache.
- **FAT coalescing** merges contiguous clusters into up to 256-sector storage
  transfers with a bounce buffer.
- **Playback buffering** uses 32 KiB reads and a 75% high watermark; playlist
  resume reads the control file in aligned bulk chunks.
- **Tagcache commit** uses 32-result seek lists and a 64-entry preload, and
  reserves 1 MiB for the audio buffer.
- **File copy** uses the plugin buffer (up to 3 MiB) and sector-aligned chunks.
- **Read-only USB** performs no host writes and no overlay work.
- **USB insertion is event-driven**; the old 100 Hz `usb_tick()` is compiled
  out, and mass storage double-buffers 24 KiB transfers.
- **IAP** is adaptive at 1 Hz idle / 10 Hz active; the PMU thread blocks on its
  queue, and the timeout API installs a tick task only when a timeout exists.
- **Audio idle** powers down the CS42L55 and gates I2S clocks on stop.
- **The bootloader** skips the overlay and the full directory/CNID index.

## 14. What not to do

- Do not remove intermediate bitmap/entry flushes without a WAL or commit-group
  design and the power-loss matrix.
- Do not enable transparent write-capable USB; raw block writes bypass the
  overlay.
- Do not delete, truncate or “repair” `/.rockpod-rw` to fix a mount or
  performance problem.
- Do not use on-device Database Initialize/Update or PictureFlow Rebuild Cache
  as the normal workflow.
- Do not enable `runtimedb` or `autoresume` until the filename-tag lookup is
  fixed; without dircache each buffered track can scan the filename tag file
  from the start.
- Do not force 216 MHz for normal playback/browsing or add a background
  performance mode; the boost model is already demand-driven. The USB
  mass-storage boost is the one exception to measure.
- Do not enable firmware function sections for size: the measured upside is
  below 1% and the section placement risk is not worth it.
- Do not remove IAP, the debug menu or the bootloader USB recovery mode without
  an explicit owner decision and a recovery plan.
- Do not change `disk_spindown` from 0 in a release default until the SSD/ATA
  resume test passes on the target adapter/card.
- Do not fix the zero-spindown idle-notify problem by making every tick sleep
  the disk; decouple notification from sleep and gate SSD explicit sleep on
  storage type.
- Do not remove PRE_WAKE/`storage_spin()` without measuring first-read and
  key-to-screen latency.
- Do not change lineout/accessory defaults without dock/remote testing.

## 15. Bottom line

The safe, high-value sequence is:

1. measure the device baseline and rerun the power-loss matrix;
2. fix the zero-spindown idle-notify coupling and wake amplification, then
   test SSD/spindown over 100 resume cycles;
3. keep the high-water batching and add per-operation flush counters;
4. fix the application multipliers: album-art folder cache, runtime-DB
   filename lookup, adapter name lookup/path cache;
5. batch data copies and move readdir/validation to bounded structures;
6. split the bootloader state; keep its existing section GC, but do not add
   firmware section GC for size (measured zero gain);
7. leave WAL/commit-group and write-capable USB for a later, explicitly
   designed release.

The biggest non-code win is likely the SSD clock-gate/deep-sleep setting, not a
CPU tweak. The biggest code win already landed is the high-water batching. The
next code win should be chosen from measured flush counts, not from intuition.

The final V3.1 release should carry the codename **“Nuclear and Readable”**;
the beta builds remain `V3.1 Beta N` and must not claim the final codename.
