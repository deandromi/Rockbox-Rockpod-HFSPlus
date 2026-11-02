# shipping.md — build, release, install and verification runbook

Status: source-derived, local build verified 2026-11-01.
Release identity: **Rockpod HFS+ — V3.0 Beta 2 // Version V13.0**.
Internal version file: `docs/VERSION` = `3.0.0-beta2-v13.0`.
Target: **iPod Classic / 6G (`ipod6g`, target ID 71) only**.
Platform: macOS for build and installation. Windows/Linux installation is out of scope.
Companion document: `agents.md` (architecture, invariants, code-quality review).

> This runbook is not a substitute for a backup. It deliberately contains hard stops. If a gate fails, stop; do not “try the next command and see”.

---

## 0. Evidence status

### 0.1 Verified in this checkout on 2026-11-01

| Check | Result |
|---|---|
| Host reader/partition tests | `make -C tools/hfsplus test` passed: API test, 20 reader tests, 7 partition tests (`test_native_adapter` skipped) and 30 adapter tests. |
| Reader under ASan/UBSan | Passed with `-no-pie` removed (the stock `sanitize` target is Linux-only). |
| Real macOS HFS+ image | `hfsplus-inspect --scan` read a 64 MiB non-journaled image and a 64 MiB `HFS+J` image. |
| Firmware build | `build-ipod6g/rockbox.ipod` 806,560 bytes; `rockbox.zip` 18–19 MiB. |
| Bootloader build | `build-ipod6g-bootloader/bootloader-ipod6g.ipod` 103,144 bytes; `_moveend` within `MOVE_AREA`. |
| DFU installer build | `utils/mks5lboot/mks5lboot` 69,832 B Mach-O arm64; `--mkdfu-inst` accepted the bootloader and produced a 107,056-byte DFU image. |
| Database tool build | `build-ipod6g-database/database.ipod6g` 184,464 B Mach-O arm64, built after restoring `uisimulator/common/filesystem-sim.{c,h}` from official Rockbox. |
| Database tool smoke test | Ran on a temporary iPod root with `VOICE_PAUSE.wav`; exit 0, `database_idx.tcd` magic `0x54434810`, one track. |
| Cover Flow index host validation | Two-album temporary database: `seek`/`artist_seek` matched tag-record offsets (`12`/`36`); name pools resolved correctly. |
| Python helpers syntax | `python3 -m py_compile` passed for both `tools/rockpod` scripts and the host tests. |
| Temporary overlay write harness | Basic create/write/read/rename/remove passed, then exposed critical bugs; see `docs/hfsplus-audit.md`. |

### 0.2 Open risks and confirmed blockers

| Item | Status |
|---|---|
| **Overlay consistency (HFS-001/002)** | **FIXED in V1.1 (host/build)**: validation is fail-closed; entries are committed before old blocks are freed. |
| **Base long-name overflow (HFS-003)** | **FIXED in V1.1 (host/build)**: overlong names are rejected/bounded; the ASan reproduction now returns unsupported. |
| **Directory rename namespace (HFS-004)** | **FIXED in V1.1 (host)**: dual logical/backing parent matching; delete/create child tests survive rename and remount. |
| **Partition scanner (HFS-006/007/008/131)** | **FIXED in V1.1 (host tests)**: tables parsed before raw probing, dual-unit GPT probing, candidate signature checks and malformed-entry continuation. |
| **Non-overlay EOF extension (HFS-009)** | **FIXED in V1.1 (host)**: rejected before touching data. |
| **Bootloader overlay (HFS-117/118/119)** | **FIXED in V1.1 (build)**: bootloader mounts native HFS+ only; hardware bootloader test still recommended. |
| **Silent overlay fallback (HFS-123)** | **FIXED in V1.1 (host)**: invalid/missing overlay forces read-only; no native write path. |
| **Overlay ID collision (HFS-124)** | **FIXED in V1.1 (host)**: ID range/uniqueness/parent validation; collision image now mounts read-only without the overlay entry. |
| **Catalog key order (HFS-126)** | **FIXED in V1.1 (host)**: decreasing parent keys fail the mount instead of silently truncating. |
| **BASE shadow leak (HFS-125)** | **FIXED in V1.1 (host)**: unchanged BASE close allocates no backing run. |
| **Track-index lock (HFS-129)** | **FIXED in V1.1 (build)**: every exit releases/guards the mutex. |
| **Reselect negative artist (HFS-130)** | **FIXED in V1.1 (build)**: bounded accessors and sentinel handling. |
| **MBR abort on bad entry (HFS-131)** | **FIXED in V1.1 (host)**: malformed entry 0 no longer hides valid entry 1. |
| **Primary header only (HFS-132)** | **FIXED in V2.0 (host)**: a validated alternate header mounts read-only with `recovered=1`; both copies corrupt fails closed. |
| **Theme persistence (HFS-133)** | **FIXED in code V1.1 (device test pending)**: fsync + forced storage-idle commit; config.new promotion reproduced on host. |
| **Mount unwind (HFS-010)** | **FIXED in V1.1 (host)**: failed mounts unmount and the next mount is retried. |
| Adapter write-path default test | Not run by `make test`; `test_native_adapter` skips unless `HFS_RB_TESTS` is supplied. The temporary harness is not committed. |
| `check_integration.py` on macOS | Fails with clang/Mach-O/freestanding errors; Linux/GCC-oriented. |
| Firmware on hardware | **V2.1.1 confirmed on the test iPod**: overlay READY, startup, theme selection and playback work. |
| Bootloader DFU install | Not run in this session. |
| Full music database / Cover Flow pipeline on hardware | User-confirmed working on the test device; the full multi-device matrix remains pending. |
| Cover Flow track list on hardware | Mac index `seek`/`artist_seek` fields were host-verified; V2.1.1 was confirmed working on the test device. |

**Read-only device snapshot (2026-11-01):** native HFS+ passed `diskutil verifyVolume`; the overlay has 16 used entries, no root/`.rockbox`/`rockbox.ipod` tombstone, and all ranges are bitmap-owned. `.rockbox` exists natively but is hidden in the Rockbox file browser by the default `Show Files: Supported` dotfile filter. V2.1.1 boots with the overlay READY, settings persist after a clean reboot, and theme selection no longer panics. See `docs/ipod-device-investigation.md` and `docs/hardware-validation.md`.

**Conclusion:** V2.1.1 is the hardware-validated baseline. V3.0 Beta adds the adapter decomposition, v3 overlay format, persistent index cache and the documented bug fixes; it is host/build verified and awaits device/power validation. Six format/compatibility limits remain documented. See `docs/hfsplus-audit.md`, `docs/release-v3.0-beta.md` and `docs/hardware-validation.md`.

---

## 1. Release identity and scope

### 1.1 Supported

- iPod Classic 6G, `ipod6g`, 64 MiB RAM.
- Cleanly unmounted HFS+ version 4 data volume.
- macOS build and installation workflow.
- Music library in native HFS+ directories, e.g. `/Music/Artist/Year - Album/track.m4a`.
- Writable state in `/.rockpod-rw` (16 MiB minimum, 2 GiB capacity cap, fully allocated, inline extents).

### 1.2 Not supported

- Other iPod targets (Nano, Video, etc.).
- APFS, HFSX, FAT32 as the intended data volume (FAT32 remains a fallback path, not the release goal).
- Windows/Linux installation.
- Resource forks, symlinks, hardlinks, macOS compression, encryption, HFS wrappers.
- Extents-overflow files, including an overlay container that needs them.
- On-device database rebuild as the documented workflow.
- Unverified iPod Classic revisions, storage adapters, or capacity beyond the device’s normal limits.

### 1.3 Naming

- Public name: **V3.0 Beta 2 // Version V13.0**.
- `docs/VERSION`: `3.0.0-beta2-v13.0`.
- Built firmware version appends the UTC build date, e.g. `1.0-v12.0-261101`.
- Internal overlay magic/version: `RPOVL11`/`RPOVL12`/`RPOVL13`, format versions 1/2/3. This is **not** the public release number. New containers initialize as v3.
- The bootloader diagnostic text `HFS+ v8 ...` is an old label, not a release or bootloader version.

---

## 2. Artifact manifest

Build these from the same source tree and record every SHA-256.

| Artifact | Path | Type / expected |
|---|---|---|
| Main firmware | `build-ipod6g/rockbox.ipod` | Scrambled ARM `ip6g` image, ~788 KiB. Not an ELF. |
| Installation archive | `build-ipod6g/rockbox.zip` | Contains `.rockbox/rockbox.ipod`, codecs, plugins, themes, fonts, language files. |
| Bootloader | `build-ipod6g-bootloader/bootloader-ipod6g.ipod` | Scrambled ARM image, ~97 KiB. Built with `--thumb`. |
| DFU installer | `utils/mks5lboot/mks5lboot` | Native macOS Mach-O arm64 (or x86_64). |
| Database host tool | `build-ipod6g-database/database.ipod6g` | Native macOS Mach-O, ~180 KiB. See §7. |
| Cover Flow index helper | `tools/rockpod/build_rockpod_coverflow_index.py` | Python 3.9+, stdlib only. |
| Artwork cache helper | `tools/rockpod/build_rockpod_pictureflow_cache.py` | Python 3.9+, FFmpeg/FFprobe. |
| Reader inspector | `tools/hfsplus/hfsplus-inspect` | Host tool; regular image files only. |

Manifest commands:

```sh
shasum -a 256 \
  build-ipod6g/rockbox.ipod \
  build-ipod6g/rockbox.zip \
  build-ipod6g-bootloader/bootloader-ipod6g.ipod \
  utils/mks5lboot/mks5lboot > SHA256SUMS
```

Artifact sanity:

```sh
file build-ipod6g/rockbox.elf \
     build-ipod6g-bootloader/bootloader.elf \
     utils/mks5lboot/mks5lboot

unzip -l build-ipod6g/rockbox.zip | grep -E '\.rockbox/rockbox\.ipod$|\.rockbox/rocks/demos/pictureflow\.rock$'
unzip -l build-ipod6g/rockbox.zip | grep -c '\.DS_Store'   # should be 0 for release
```

Local verification build hashes (not release hashes; timestamps and source paths affect them):

| Artifact | SHA-256 |
|---|---|
| `rockbox.ipod` | `50397eec3efab6d48d13b48e26058ca4f517a66913db5ae50e5e0dbd0b7549a3` |
| `rockbox.zip` | `5ff003906c9ca5b1f54e4a1e5488cd73f4f1bc40681cae1364317d86ff9c6099` |
| `bootloader-ipod6g.ipod` | `ff8bcaca6f2287eda49370bed1d3af68c25f3cc3dfd578fd9f0917624f8c0d4a` |
| `database.ipod6g` | `cb1ae2c9f8ece24dc03b3105c9672bdd59df5b34f44cf671ae546ba577cf2202` |
| `mks5lboot` | `25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b` |

---

## 3. Toolchain setup (macOS)

### 3.1 Required

- macOS with Xcode command-line tools.
- GNU Make. The system `/usr/bin/make` is 3.81; Homebrew `make` 4.4.1 also worked. Use one consistently.
- Homebrew dependencies from the project README:

```sh
xcode-select --install
brew install make coreutils gnu-sed gawk texinfo automake autoconf libtool \
  flex bison xz pkg-config
```

- ARM toolchain: `arm-elf-eabi-gcc` 9.5.0 and GNU Binutils 2.38. A generic `arm-none-eabi` package is **not** automatically equivalent.
- For the database host tool: `brew install gcc@15 sdl2` (see §7 for the restored host glue and compatibility patches).
- For the artwork helper: `brew install python ffmpeg`.

Expose GNU tools:

```sh
export PATH="$(brew --prefix coreutils)/libexec/gnubin:$(brew --prefix gnu-sed)/libexec/gnubin:$(brew --prefix texinfo)/bin:$(brew --prefix flex)/bin:$(brew --prefix bison)/bin:$PATH"
```

If the Rockbox ARM toolchain is not installed:

```sh
bash tools/rockboxdev.sh --target=a \
  --prefix="$HOME/rockbox-toolchain" \
  --makeflags="-j$(sysctl -n hw.ncpu)"
export PATH="$HOME/rockbox-toolchain/bin:$PATH"
```

Verify:

```sh
arm-elf-eabi-gcc --version
arm-elf-eabi-ld --version
```

### 3.2 Source path

Use a path **without spaces**. Generated Makefiles contain absolute paths; moving the source root after configure requires a fresh build directory.

```sh
mkdir -p "$HOME/Developer"
cd "$HOME/Developer"
git clone https://github.com/deandromi/Rockpod-HFSPlus.git
cd Rockpod-HFSPlus
```

This checkout has no `.git`; if using an archive, extract to a simple path.

---

## 4. Build the firmware

```sh
mkdir -p build-ipod6g
cd build-ipod6g
../tools/configure --target=ipod6g --type=n
make -j"$(sysctl -n hw.ncpu)"
make -j"$(sysctl -n hw.ncpu)" zip
```

If `make` is not GNU Make 3.81+ or behaves oddly, use `gmake`.

Expected output:

- `build-ipod6g/rockbox.elf` — ELF 32-bit LSB ARM EABI5.
- `build-ipod6g/rockbox.ipod` — scrambled image; `file` reports `data`.
- `build-ipod6g/rockbox.zip` — installation archive.

Checks:

```sh
test -s rockbox.ipod
test -s rockbox.zip
unzip -t rockbox.zip | tail -1
unzip -l rockbox.zip | grep '\.rockbox/rockbox\.ipod$'
cat rockbox-info.txt | head -20
```

`rockbox-info.txt` should report target `ipod6g`, ID 71, memory 64, and the expected version prefix. In the local build it reported `Version: 3.0.0-beta-v13.0-261102` and `RAM usage: 5732648`.

**Release hygiene:** the local `rockbox.zip` contained `.rockbox/.DS_Store` copied from the source tree. Before a release, remove `/.DS_Store`, `firmware/.DS_Store`, `tools/.DS_Store`, and any other macOS metadata, or add a packaging exclusion. Rebuild the zip and confirm the count is zero.

---

## 5. Build the bootloader

The bootloader must be **Thumb**. In the project’s ARM GCC 9.5.0 test, the ARM-mode bootloader exceeded `MOVE_AREA` by 3,552 bytes; the Thumb build linked at ~97 KiB.

```sh
mkdir -p build-ipod6g-bootloader
cd build-ipod6g-bootloader
../tools/configure --target=ipod6g --type=b --thumb
make -j"$(sysctl -n hw.ncpu)"
```

Expected:

- `build-ipod6g-bootloader/bootloader.elf` — ARM ELF.
- `build-ipod6g-bootloader/bootloader-ipod6g.ipod` — scrambled image (103,144 bytes in the V3.0 Beta build).

Checks:

```sh
file bootloader.elf bootloader-ipod6g.ipod
grep -E '_movestart|_moveend' bootloader.map
strings -a bootloader-ipod6g.ipod | grep -E 'HFS\+|read-only test'
```

The local map reported:

```
_movestart = 0x22020800
_moveend   = 0x22038a60
```

Ensure `_moveend` is within `MOVE_AREA`. Building does not flash the device.

**Do not install an ARM-mode bootloader merely because it links.** Check the size first.

---

## 6. Build `mks5lboot`

```sh
make -C utils/mks5lboot -j"$(sysctl -n hw.ncpu)" CC=clang \
  LDOPTS="-framework IOKit -framework CoreFoundation"
```

Expected: `utils/mks5lboot/mks5lboot` is a native Mach-O executable.

Checks:

```sh
file utils/mks5lboot/mks5lboot
./utils/mks5lboot/mks5lboot --dfuscan
```

Without a device, `--dfuscan` should report “no DFU devices found”. That is a successful tool invocation.

Dry-run the installer image:

```sh
tmp=$(mktemp -d)
./utils/mks5lboot/mks5lboot --mkdfu-inst \
  build-ipod6g-bootloader/bootloader-ipod6g.ipod \
  "$tmp/test.dfu"
ls -lh "$tmp/test.dfu"
rm -rf "$tmp"
```

It should report model `Classic 6G`, platform `ipod6g`, RB name `ip6g`, RB number 71. A 98,912-byte bootloader produced a 107,056-byte DFU image locally.

Clang may emit `-Wunterminated-string-initialization` warnings from `mkdfu.c`; they are inherited and non-fatal. Do not treat them as a reason to edit the installer casually.

---

## 7. Build the Mac database host tool

### 7.1 Restored host filesystem glue

The source snapshot originally lacked `uisimulator/common/filesystem-sim.{c,h}`, which `tools/database/SOURCES` and `firmware/include/{file,dir}.h` require for `DBTOOL`. Those files have been restored from the official Rockbox repository (`https://github.com/rockbox/rockbox/tree/master/uisimulator/common`) and adapted to this older tree as described below.

Two compatibility edits are required for this older tree:

1. `uisimulator/common/filesystem-sim.{c,h}`: the upstream master names `sim_ffilesize`/`os_ffilesize` were changed to `sim_filesize`/`os_filesize`. This tree’s `firmware/include/file.h` maps `filesize` to `sim_filesize` and `firmware/target/hosted/filesystem-hosted.h` declares `os_filesize`.
2. `firmware/include/dir.h`: add the upstream mapping inside `!DIRFUNCTIONS_DEFINED`:

   ```c
   #ifndef dir_get_info
   #define dir_get_info    FS_PREFIX(dir_get_info)
   #endif
   ```

   Without it, `tagcache.o` calls `dir_get_info` while the simulator provides `sim_dir_get_info`.

The imported `filesystem-sim.c` still contains upstream `HAVE_DIRCACHE` blocks with `xxxxxx` placeholders. They are **not compiled** for `DBTOOL` because `__PCTOOL__` disables dircache in this configuration. Do not enable `HAVE_DIRCACHE` for this host tool without completing those blocks.

### 7.2 Build

The documented command is:

```sh
cd "$HOME/Developer/Rockpod-HFSPlus-build"
mkdir -p build-ipod6g-database
cd build-ipod6g-database
../tools/configure --target=ipod6g --type=d
```

`configure` may print `ld: unknown options: --version` and `test: ... integer expression expected`; these are inherited probe warnings and were non-fatal in the verified build. Then correct the generated Makefile for Apple Clang:

```sh
sed -i '' \
  -e 's#^export CC=.*#export CC=/usr/bin/clang#' \
  -e 's#^export HOSTCC=.*#export HOSTCC=/usr/bin/clang#' \
  -e 's#^export CPP=.*#export CPP=/usr/bin/clang -E#' \
  -e 's#^export AR=.*#export AR=/usr/bin/ar#' \
  -e 's/ -funit-at-a-time//g' \
  -e 's/ -Wimplicit-fallthrough=0//g' \
  Makefile
make -j"$(sysctl -n hw.ncpu)"
```

Reconfiguring regenerates `Makefile`, so repeat the `sed` corrections after every configure.

If headers changed but objects were not rebuilt (the generated `make.dep` can contain malformed target paths on macOS), use the cleanup in §7.5.

Expected output:

```sh
ls -lh database.ipod6g
file database.ipod6g
# must be Mach-O 64-bit executable arm64 (or x86_64), not ARM ELF
```

Local verification: 184,464 bytes, Mach-O arm64, SHA-256 `cb1ae2c9f8ece24dc03b3105c9672bdd59df5b34f44cf671ae546ba577cf2202`.

### 7.3 Smoke test

Run from a temporary iPod-like root; this does not touch a real iPod:

```sh
db="$(pwd)/build-ipod6g-database/database.ipod6g"
tmp=$(mktemp -d)
mkdir -p "$tmp/.rockbox" "$tmp/Music/Test"
cp tools/VOICE_PAUSE.wav "$tmp/Music/Test/test.wav"
(cd "$tmp" && "$db")
rc=$?
test "$rc" -eq 0
python3 - "$tmp" <<'PY'
import struct, sys
from pathlib import Path
p = Path(sys.argv[1]) / '.rockbox' / 'database_idx.tcd'
b = p.read_bytes()
magic, size, tracks, serial, commit, dirty = struct.unpack_from('<6I', b)
assert magic == 0x54434810 and tracks == 1 and dirty == 0
print(f'OK: {len(b)} bytes, {tracks} track, magic 0x{magic:08x}')
PY
rm -rf "$tmp"
```

Expected: exit 0, `database_idx.tcd` with magic `0x54434810`, one track, `dirty == 0`.

### 7.4 Keep in sync

The restored files are upstream GPL code. When updating Rockbox, re-check the `sim_ffilesize`/`os_ffilesize` naming and the `dir_get_info` mapping. A smaller `DBTOOL`-only path shim would be cleaner long-term, but the restored upstream glue is what makes the documented Mac workflow build today.

### 7.5 Known cleanup for stale host builds

If an old database build reports a missing generated header such as `time.h`, clean only inside the database build directory:

```sh
rm -f make.dep make.dep_
find . -type f -name '*.o' -delete
make -j"$(sysctl -n hw.ncpu)" dep
make -j"$(sysctl -n hw.ncpu)"
```

Do not run those commands in the source root or on the iPod.

---

## 8. Verify the Python helpers

```sh
python3 -m py_compile \
  tools/rockpod/build_rockpod_coverflow_index.py \
  tools/rockpod/build_rockpod_pictureflow_cache.py
```

Requirements:

- Python 3.9 or later.
- `ffmpeg` and `ffprobe` for the artwork cache.
- The iPod mounted in Apple Disk Mode at a known mount point.
- The same source checkout used for the database tool.

### 8.1 Cover Flow index helper

The helper reads an already committed Rockbox v12 database (`0x54434810`), not audio tags. It requires:

- `.rockbox/database_idx.tcd`, `database_1.tcd`, `database_7.tcd`;
- no `database_tmp.tcd`;
- at least one non-deleted track;
- a valid master index size (`24 + tracks*96`).

Staging (no iPod changes):

```sh
python3 tools/rockpod/build_rockpod_coverflow_index.py \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/iPod bestanden/Backups"
```

Install only after reviewing the report and overlay conflicts:

```sh
python3 tools/rockpod/build_rockpod_coverflow_index.py \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/iPod bestanden/Backups" \
  --install
```

The helper:

- refuses installation when active overlay entries have the output filenames;
- backs up existing output files locally;
- writes each output via a temporary file and read-back;
- rechecks the database hashes before installing.

### 8.2 Host validation: `album_data.seek`

`apps/plugins/pictureflow/pictureflow.c` uses `pf_idx.album_index[slide_index].seek` as a tagcache tag offset:

```c
tagcache_search_add_filter(&tcs, tag_album, pf_idx.album_index[slide_index].seek);
```

The helper packs:

```python
rows = b''.join(struct.pack('<5i', aoffset[a], roffset[r], pairs[(a,r)], r, a) ...)
```

`aoffset[a]`/`roffset[r]` are the new name-pool offsets used by `name_idx`/`artist_idx`; the final two fields are the original tag-file record offsets (`artist_seek = r`, `seek = a`). That is the contract the firmware expects.

Host verification performed during this review: a temporary iPod root with two album/artist pairs produced tag records at offsets `12` and `36`. The generated index rows were:

```text
(name_idx=0,  artist_idx=0,  year=0, artist_seek=12, seek=12)
(name_idx=10, artist_idx=11, year=0, artist_seek=36, seek=36)
```

Both `seek` values exist in `database_1.tcd`, both `artist_seek` values exist in `database_7.tcd`, and the name indexes resolve to the correct strings. No `seek` fix is required.

Remaining verification: install the generated index on hardware, open an album in Cover Flow, and confirm the track list and playback. If it fails, retain the index, report, and database files; do not delete the overlay.

### 8.3 Artwork cache helper

```sh
python3 tools/rockpod/build_rockpod_pictureflow_cache.py \
  --music "/Volumes/IPOD/Music" \
  --cache "/Volumes/IPOD/.rockbox/rocks/demos/pictureflow"
```

Behavior:

- scans album directories for square `cover.jpg`;
- tries audio files until it finds album + album_artist (falls back to artist);
- computes the same modified-FNV filename as the firmware;
- scales to 128×128, converts to non-dithered RGB565, writes PictureFlow’s transposed layout;
- builds every cover in a temporary directory first;
- aborts installation if any album fails;
- by default removes old album `.pfraw` files, but preserves `emptyslide.pfraw`, the album index and `._*` sidecars;
- `--keep-old` retains old album files while replacing matching names.

Risks:

- installation is staged but not atomic across the whole cache directory;
- it writes native HFS+ files and does not inspect overlay shadowing; an existing overlay copy can hide the new cache;
- it does not back up old `.pfraw` files; do that manually;
- use lowercase `cover.jpg` and square images.

---

## 9. Device preflight

### 9.1 Identify the device

```sh
diskutil list
diskutil info "/Volumes/IPOD"
```

Replace `/Volumes/IPOD` with the actual mount point. Identify by reported size and name, not by a disk number copied from an example.

Confirm:

- the volume is HFS+ (`Apple_HFS` / “Mac OS Extended”);
- the mount point exists and is a real mount (`mount | grep` or `diskutil info`);
- `.rockbox` exists;
- the volume is the intended iPod, not another Mac disk;
- free space is sufficient for the firmware archive, database and cache.

### 9.2 Backup

Before any install or update, back up these distinct parts:

1. `/Volumes/IPOD/Music` and other user files;
2. `/Volumes/IPOD/.rockbox`;
3. `/Volumes/IPOD/.rockpod-rw` (if present).

Example (use a destination with enough space):

```sh
backup="$HOME/Documents/iPod bestanden/Backups/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup"
ditto "/Volumes/IPOD/Music" "$backup/Music"
ditto "/Volumes/IPOD/.rockbox" "$backup/.rockbox"
[ -e "/Volumes/IPOD/.rockpod-rw" ] && cp -p "/Volumes/IPOD/.rockpod-rw" "$backup/.rockpod-rw"
```

Verify:

```sh
du -sh "$backup"
find "$backup" -type f | wc -l
shasum -a 256 "$backup/.rockpod-rw" 2>/dev/null || true
```

A backup that has not been read back is not a verified backup. Spot-check at least one file and the overlay size/hash.

### 9.3 Native volume check

```sh
diskutil verifyVolume "/Volumes/IPOD"
```

This checks native HFS+ only, not the overlay. If it reports errors, stop and retain the full output. Do not run a whole-disk repair, format or restore as a first response.

### 9.4 Overlay presence and conflicts

For a **new installation**, the container must not exist yet. Create it only then:

```sh
if [ -e "/Volumes/IPOD/.rockpod-rw" ]; then
    printf '%s\n' 'Existing overlay retained; no file created.'
else
    mkfile 1g "/Volumes/IPOD/.rockpod-rw"
fi
```

`mkfile` must finish successfully. Do not use its sparse option. A 1 GiB container is the practical starting point; requirements depend on library and cache size. The container must be fully allocated, at least 16 MiB, at most 2 GiB, and fit in 8 inline extents.

For an **existing installation**, do not recreate or replace the overlay. Use the firmware’s debug screen to check `READY`/stage 9/error 0/slot -1. For a read-only conflict check of names, reuse the existing parser:

```sh
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, 'tools/rockpod')
from build_rockpod_coverflow_index import overlay_conflicts
p = Path('/Volumes/IPOD/.rockpod-rw')
if p.exists():
    for item in overlay_conflicts(p):
        print(item)
else:
    print('No overlay container.')
PY
```

This only reads the overlay. Do not “repair” by writing.

For the full database backup/overlay-conflict procedure, use the Python script in `README.md` §“Build the Rockbox music database on the Mac”, step 3. It backs up native database files, verifies copies, and stops if database overlay entries could shadow them.

### 9.5 Finish transfers before DFU

```sh
sync
diskutil eject "/Volumes/IPOD"
```

Wait for successful ejection, then disconnect. **Always cleanly eject before entering DFU.** Entering DFU without a clean eject caused a `-4` mount error on the tested device.

---

## 10. Install firmware

From the directory containing `rockbox.zip`:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
```

The result must be:

```text
/Volumes/IPOD/.rockbox/rockbox.ipod
```

Not a nested `.rockbox` under a source folder. Stop if extraction reports an error. A missing `/Volumes/IPOD` usually means the iPod is not mounted; creating a directory with that name does not mount it.

Verify:

```sh
test -f "/Volumes/IPOD/.rockbox/rockbox.ipod"
ls -lh "/Volumes/IPOD/.rockbox/rockbox.ipod"
sync
diskutil eject "/Volumes/IPOD"
```

Firmware installation does not require a bootloader reinstall on every update. The bootloader is separate.

---

## 11. Install the HFS+-aware bootloader

### 11.1 Preconditions

- Firmware is already extracted to `.rockbox/rockbox.ipod`.
- The volume was cleanly ejected.
- The Mac has no Music/iTunes restore dialog or helper racing the DFU device.
- The bootloader artifact is the Thumb build and passes §5.
- The user has explicitly approved a NOR write for this device.

### 11.2 Pause Apple device helpers

Use the monitor script from `README.md` installation step 3, “Stop Apple device helpers and install the bootloader”, verbatim. It pauses `AMPDevicesAgent`, `AMPDeviceDiscoveryAgent`, `com.apple.amp.devicesui` and `iTunesHelper` with `SIGSTOP`, and resumes them with `SIGCONT` when stopped with Control-C. Do not close the window without stopping the monitor; that can leave helpers paused.

### 11.3 Enter DFU and verify

In a second Terminal, from the source root:

```sh
./utils/mks5lboot/mks5lboot --dfuscan --loop 1
```

Put the iPod in DFU: hold **Menu + Select** for about 12 seconds until the scan detects it. The screen remains black in DFU. The tested device reported:

```text
[05ac:1223]
mode: DFU
DFU device state: 2
```

A `WTF` result is a different recovery mode. Stop; do not install.

Stop the scan with Control-C after detection.

### 11.4 Install

```sh
./utils/mks5lboot/mks5lboot --bl-inst \
  ./build-ipod6g-bootloader/bootloader-ipod6g.ipod
```

Expected installer behavior:

- alive tone (2000 Hz / 100 ms);
- success dual tone (1000 Hz / 100 ms + 2000 Hz / 150 ms);
- automatic reboot.

Failure tones:

- 330 Hz / 500 ms: installation failed;
- three 330 Hz tones: NOR may be corrupted; restore via Apple/iTunes is the last resort.

**Do not add `--single`.** It destroys the existing Apple NOR boot path. **Do not unplug during installation.** Retain the complete installer output. Do not repeatedly send other bootloader images after a failure.

When finished, stop the Apple-device monitor with **Control-C** so paused helpers resume.

### 11.5 First boot

The device should boot Rockpod. If it shows an HFS+ mount error, see §15.

---

## 12. First-boot acceptance

Open **Debug menu -> View disk info**. A healthy initialized overlay must report:

```text
HFS+ overlay: READY
Overlay stage: 9
Overlay error: 0
Overlay slot: -1
Overlay reason: 0 none
```

Then verify, in order:

1. **Settings persistence**: change a visible setting, reboot, confirm it survived. This proves overlay writes, not just reads.
2. **Database playback**: open **Database**, choose an album, play a track.
3. **Cover Flow**: open **Cover Flow**, navigate albums, open an album, confirm the track list and playback.
4. **Now Playing album art**: confirm the cover renders at the theme’s requested dimensions. Now Playing and PictureFlow use different image paths; a correct `.pfraw` cache does not prove the Now Playing decoder is correct.
5. **Album art diagnostics**: open **View album art diagnostics** while playing. Record source path, loaded/current dimensions, and load/draw/now CRCs. Equal CRCs mean stable measured bytes; they do not by themselves prove the decoder is correct. The project’s reproducible test cover produced CRC `f404e0ca` before the JPEG repair and `85780f2f` after it, with dithering disabled; other dimensions or files produce different CRCs and are not interchangeable evidence.
6. **Free space**: check that the reported free space is plausible. `hfsplus_size()` returns the smaller of native free and overlay free when the overlay is ready.
7. **Clean shutdown**: stop playback, wait for disk spin-down, power off normally.

A successful firmware extraction, overlay initialization and bootloader install are three separate results. Completing one does not prove the others.

---

## 13. Music database and Cover Flow pipeline

For Stable V2.1.1, use:

**Mac database -> Mac PictureFlow index -> Mac artwork cache -> clean eject -> playback.**

Do not use **Initialize Now** or **Update Now** on the iPod as part of this workflow. On-device database initialization was extremely unstable on the tested device.

### 13.1 Settings

- **Settings -> General Settings -> Database -> Auto Update: No.**
- For initial database verification, leave **Load to RAM** off so disk-based access is checked first.
- Do not reboot into Rockpod between creating the database and installing the matching index/cache.

### 13.2 Build the database on the Mac

1. Mount the iPod in Apple Disk Mode.
2. Verify the mount point and `.rockbox`.
3. Back up native database files and check overlay conflicts (README step 3).
4. Run the native database tool **from the iPod volume root**:

```sh
database_tool="$HOME/Developer/Rockpod-HFSPlus-build/build-ipod6g-database/database.ipod6g"
log_dir="$HOME/Documents/iPod bestanden/Logs"
mkdir -p "$log_dir"
cd "/Volumes/IPOD" &&
test -d .rockbox &&
test -x "$database_tool" &&
set -o pipefail &&
caffeinate -i "$database_tool" 2>&1 | tee "$log_dir/rockpod-database-build.log"
```

Do not interrupt a database write. A full rebuild replaces the native tag database and may lose runtime statistics; the backup retains the previous files.

> The database tool is built and smoke-tested. Keep the restored `uisimulator/common` glue and the compatibility patches in §7.1 in place when rebuilding.

### 13.3 Build/install the Cover Flow index

Follow §8.1. The `seek` fields are host-validated per §8.2; still confirm the album track list and playback on hardware before marking the release verified.

### 13.4 Build the artwork cache

Follow §8.3. Back up the existing cache first. If any album fails, fix it and rerun; the helper will not install a partial set.

After installing a Mac-generated index/cache, do **not** use Cover Flow’s **Rebuild Cache**, **Update Cache**, or any on-device index rebuild. The on-device rebuild path contains S0/S1 buffer-overflow and untagged-album index bugs (HFS-112/114) and can corrupt memory/files. If the Mac index is missing or invalid, stop and rebuild it on the Mac; do not let the device rebuild it.

### 13.5 Finish

```sh
sync
diskutil eject "/Volumes/IPOD"
```

Disconnect, boot Rockpod, confirm overlay `READY`, then test Database and Cover Flow. After adding or retagging music, repeat the database rebuild and matching index/cache refresh.

---

## 14. Update procedure

1. Back up `.rockbox`, `.rockpod-rw`, music and any local cache.
2. Connect in Apple Disk Mode.
3. Extract the new `rockbox.zip` over the existing `.rockbox`.
4. Do **not** replace or recreate `.rockpod-rw` as a routine update. It contains current settings, playlists and database.
5. `sync`, `diskutil eject`, disconnect, boot.
6. Verify overlay `READY`/stage 9/error 0/slot -1 and one playback.
7. Rebuild the database/index/cache only if music or tags changed.

The internal overlay format is unchanged from the working development version. A firmware replacement normally leaves the HFS+-aware bootloader installed. A future release that requires a new bootloader or overlay format must say so explicitly.

---

## 15. Failure handling and recovery

### 15.1 Startup mount error `-4`

Meaning: the HFS+ volume header does not indicate a clean unmount, indicates inconsistency, or the journal has pending transactions requiring replay.

The bootloader may show:

```text
HFS+ v8 mount error: -4
volume requires consistency checking or journal replay
```

The `v8` text is an old diagnostic label. This error alone does not prove data loss.

Recovery:

1. Stop the Apple-device monitor with Control-C so paused helpers resume.
2. Enter Apple Disk Mode: reset with Menu + Select, then hold Select + Play/Pause when the screen turns black.
3. Connect to the Mac. Confirm the actual mount point.
4. Verify:

   ```sh
   diskutil info "/Volumes/IPOD"
   diskutil verifyVolume "/Volumes/IPOD"
   ```

5. If verification reports OK with exit code 0, eject (verification remounts the volume):

   ```sh
   diskutil eject "/Volumes/IPOD"
   ```

6. Wait for successful ejection, unplug USB, reset with Menu + Select, check startup and playback.

If verification reports errors or ejection fails: stop and retain the complete output. Do not format, restore, delete `.rockpod-rw`, or run a whole-disk partition repair solely because of this message.

This exact sequence restored the tested device after entering DFU without a clean eject.

### 15.2 Configuration write error `-83`

Check overlay status first. The development failure occurred when normal create/truncate operations were blocked because the overlay was disabled. It is not automatically a native filesystem corruption. If the overlay is `OFF`, inspect stages 1–10 before touching the container.

### 15.3 Overlay stage failures

| Stage | Interpretation |
|---:|---|
| 1 | `/.rockpod-rw` was not found; verify spelling and presence. |
| 2 | Container type, inline extents, size or layout failed. |
| 3–6 | Header or allocation bitmap read/validation failed. |
| 7–8 | Entry read/validation failed; the slot identifies the record. |
| 9, error 0 | Ready. |
| 10 | Fresh-container initialization was attempted or failed. |
| 11 | Whole-overlay validation failed; `Overlay slot` is the entry index (`-1` for a table-wide failure) and `Overlay reason` names the check. |

Before any repair:

- make a full copy of `.rockpod-rw` in Disk Mode;
- do not delete it;
- do not zero its header;
- use the debug values and a read-only inspection to distinguish format corruption from storage damage;
- preserve the decoded stage-8 or stage-11 fields (id, parent, flags, kind, name length, data start/blocks) and the stage-11 reason.

The documented development recovery cleared one invalid entry’s used flags while retaining the bitmap and other entries, and a write-range guard was added. That was specific to one damaged overlay; it is not a universal repair command.

### 15.4 Bootloader failure

- Retain the complete installer output.
- Identify the tone: single 330 Hz = failure; three 330 Hz = possible NOR corruption.
- Do not loop through random bootloader images.
- If NOR is corrupted, Apple/iTunes restore is the last resort; it erases the device and the Rockbox bootloader.
- DFU is a transport for installation and execution; this installer does not recover the original bootloader file from an already flashed iPod.

### 15.5 Good Cover Flow, damaged Now Playing art

The two renderers use different image paths. Inspect **View album art diagnostics** and compare load/draw/now CRCs and the source path. Regenerating `.pfraw` files does not repair a Now Playing JPEG decode issue.

### 15.6 `tools/root.make` missing at an old path

The source was moved after configure generated the Makefile. Use a fresh build directory and reconfigure. Do not patch the generated Makefile paths by hand.

### 15.7 `patch` asks to reverse a change

The release already contains the development fixes. Old iterative patches are not an installation step. Stop and inspect before applying anything.

### 15.8 Connecting in Rockbox USB mode corrupts data

This is a long-standing stock Rockbox USB mass-storage issue on multiple iPods, not a defect introduced by this HFS+ fork. The device owner reports that connecting while Rockpod is running reliably causes corruption; Apple Disk Mode is the safe path.

Operational rules:

- Use Apple Disk Mode for all file transfer, backup and database work.
- Do not use Rockbox USB mode as a substitute for Disk Mode.
- Do not conclude that a disk is healthy merely because it mounts in Rockbox USB mode.
- V2.0 adds the overlay header checksum and block-1 backup (HFS-040) on top of the V1.1 bitmap/entry validation and commit ordering (HFS-001/002).
- If a device was connected in Rockbox USB mode, cleanly eject it and reboot; if startup reports `-4`, follow §15.1. Do not bypass the consistency checks or delete the overlay.

---

## 16. Verification matrix

| Gate | Method | Pass criteria | Evidence |
|---|---|---|---|
| Host reader tests | `make -C tools/hfsplus test` | API + 20 reader + 7 partition + 30 adapter tests pass; native adapter either passes or is explicitly skipped | Terminal log |
| Reader sanitizers | ASan/UBSan with platform-correct flags | No ASan/UBSan reports | Terminal log |
| Real HFS+ image | `hfsplus-inspect --scan` on macOS-created HFS+ and HFS+J images | `info`, `ls`, `cat` work | Commands + output |
| Firmware build | configure + make + zip | `rockbox.elf` ARM, `rockbox.ipod` present, zip valid | `rockbox-info.txt`, hashes |
| Bootloader build | configure + make `--thumb` | `_moveend` within `MOVE_AREA`; ~97 KiB | Map + size |
| DFU image | `mks5lboot --mkdfu-inst` | Correct model/platform/RB metadata | Output |
| Artifact hygiene | `unzip -l` | `.rockbox/rockbox.ipod` present; no `.DS_Store` | Zip listing |
| Database tool | configure + sed + make | Native Mach-O executable; smoke test creates a valid `database_idx.tcd` | `file` output + smoke test |
| Overlay consistency | Host fault/corruption harness | Entry ranges fully allocated, non-overlapping; no live file overwritten | **PASS in V1.1 (host)** |
| Overlay diagnostics | Host adapter `diag` mode on corrupted images | Stage 11 reports the failing slot and reason name for range, ID, parent, name, size and tombstone failures | **PASS (host)** |
| Overlay long names | Host adapter test with a 765-byte HFS+ name | No overflow; reject or bound to `FAT_DIRENTRY_NAME_MAX` | **PASS in V1.1 (host)** |
| Directory rename namespace | Host adapter test: delete/create child, rename parent | Deleted child stays hidden; new child stays visible | **PASS in V1.1 (host)** |
| Bootloader native fallback | Hide `/.rockbox` in overlay on a copy; boot | Bootloader still loads native `/.rockbox/rockbox.ipod` | **PASS in code (hardware boot test pending)** |
| Base/overlay identity | Two open handles for one base file; rename/move flows | One logical object, one shadow; `fat_file_is_same`/`fat_dir_is_parent` agree | **PASS in V2.0 (host)** |
| PictureFlow corrupt cache/index | Feed truncated/hostile `.pfraw` and album index | Reject without memory errors; report cache failure | **FIXED (host build; device cache test pending)** |
| CNID reuse / tombstone identity | Reuse a deleted base CNID with a new name | New object is not hidden by the old tombstone | **PASS in V1.1 (host)** |
| HFS direntry timestamps | Read directory metadata | Plausible create/modify time, not 1980/zero | **PASS in V2.0 (host)** |
| Partition scanner adversarial cases | MBR 512/4096, raw-vs-MBR, GPT dual-unit, bad entry before valid | Real HFS+ partition selected in every case | **PASS in V1.1 (host)** |
| Non-overlay EOF write | Host adapter test: write past native size | Rejected before modifying data | **PASS in V1.1 (host)** |
| Mount unwind | Inject fast-index failure | Volume unmounted; next mount succeeds | **PASS in V1.1 (host)** |
| Invalid overlay fail-closed | Corrupt overlay version/header; attempt native same-size write | Mount reports overlay failure; no native write occurs; clear diagnostic | **PASS in V2.0 (host)** |
| Overlay ID collision | Patch an entry ID to a native CNID; open both names | IDs rejected/normalized; native name opens native data | **PASS in V2.0 (host)** |
| BASE shadow close/write | Rename native file; close without writing; then write | No block leak; bitmap ownership matches entry | **PASS in V2.0 (host)** |
| Catalog key order | Change `.rockbox` parent key in a copy; list root | Corruption reported; `.rockbox` never silently omitted | **PASS in V2.0 (host)** |
| PictureFlow track-index lock | Repeated track-list/context-menu transitions; force tagcache failure | No deadlock; mutex held only while the index is valid | **PASS in V2.0 (build)** |
| PictureFlow no-artist reselect | Load an index with `artist_idx = -1`; change sort/reselect | No read before `artist_names`; no crash | **PASS in V2.0 (build)** |
| Alternate volume header | Corrupt primary header on a copy; mount | Read-only recovery or explicit failure; never silent write | **PASS in V2.0 (host)** |
| Theme persistence | Select theme → wait for storage idle → verify `.new` → clean power-off → reboot | Theme survives reboot | **FIXED in code; device reboot test pending (HFS-133)** |
| DFU scan | `mks5lboot --dfuscan` | Device detected in DFU state 2 | Terminal output |
| Bootloader install | `--bl-inst` (no `--single`) | Success tones + reboot | Output, tone observation |
| Overlay init | Debug disk info | `READY`, stage 9, error 0, slot -1 | Screen record |
| Settings persistence | Change + reboot | Setting survives | Screen record |
| Database playback | Database -> album -> track | Audio plays | Device observation |
| Cover Flow | Cover Flow -> album -> track | Covers, track list and playback work | Device observation |
| Now Playing art | Play track | Cover renders at theme dimensions | Device observation |
| Art diagnostics | View album art diagnostics | Load/draw/now CRCs recorded; source path correct | Screen record |
| Clean eject | `sync` + `diskutil eject` | Eject succeeds before DFU/power-off | Terminal log |
| Update | Extract new zip, keep overlay | Boot + playback + settings survive | Device observation |

Do not mark a row “pass” from a different build or a different device.

---

## 17. Release checklist

### 17.1 Source and docs

- [ ] `docs/VERSION` is the intended release version.
- [ ] `agents.md` and `shipping.md` match current behavior and limits.
- [ ] No development build directories in the release archive.
- [ ] No `.DS_Store`, `.orig`, `.rej`, local binaries or editor backups in the archive.
- [ ] All licenses and credits retained (`COPYING`, `docs/CREDITS`, theme notices).
- [ ] Known issues documented, including the host-validated Cover Flow index and the restored database-tool glue.

### 17.2 Build

- [x] No open S0 findings in `docs/hfsplus-audit.md` (V1.1 fixed HFS-001/002/003/004/112/114).
- [ ] ARM toolchain verified (`arm-elf-eabi-gcc` 9.5.0, binutils 2.38).
- [ ] Firmware built; `rockbox.ipod` and `rockbox.zip` present.
- [ ] Bootloader built with `--thumb`; size within `MOVE_AREA`.
- [ ] `mks5lboot` built and `--mkdfu-inst` dry-run passed.
- [ ] Database tool built as native Mach-O and smoke-tested with a temporary iPod root.
- [ ] Host reader tests and ASan/UBSan passed.
- [ ] Real HFS+ and HFS+J image read test passed.

### 17.3 Artifacts

- [ ] `SHA256SUMS` generated.
- [ ] Zip contains `.rockbox/rockbox.ipod`, codecs, plugins, themes, fonts, language files.
- [ ] Bootloader is scrambled and accepted by `mks5lboot`.
- [ ] Release notes state the exact firmware version, bootloader requirement and overlay format compatibility.

### 17.4 Device

- [ ] Device identified and mount point verified.
- [ ] Music, `.rockbox` and `.rockpod-rw` backed up and read back.
- [ ] Native volume verified; clean eject performed.
- [ ] Overlay constraints checked (existing or new).
- [ ] Bootloader install approved and supervised (or autonomy gate in §18 satisfied).
- [ ] First-boot overlay state is `READY`/9/0/-1.
- [ ] Settings persistence, Database playback, Cover Flow and Now Playing art verified.
- [ ] Recovery path known and written down.

### 17.5 Post-release

- [ ] Record device model/revision, storage adapter, capacity, macOS version, toolchain, artifact hashes.
- [ ] Record every verification command and output location.
- [ ] Mark unverified combinations explicitly.
- [ ] Open follow-up issues for the hardware Cover Flow track-list check and the long-term database-tool shim cleanup.

---

## 18. Autonomy policy for agents

The user may eventually authorize autonomous installation. Autonomy is **earned per release and per device class**, not assumed.

### 18.1 Current status

**Not yet authorized for unsupervised install.** Reasons:

- open S0 findings HFS-001 (overlay bitmap/entry overlap), HFS-002 (free-before-sync ordering), HFS-003 (long-name overflow) and HFS-004 (directory rename namespace) can cause silent data loss or memory corruption;
- open S0/S1 findings in PictureFlow cache/index parsing (HFS-082/083/084/103/112/114) and CNID-reuse tombstones (HFS-090) can crash, overflow or hide files;
- a present-but-invalid overlay silently allows native writes (HFS-123), overlay IDs can collide with native CNIDs (HFS-124), and catalog key-order corruption can hide `.rockbox` without an error (HFS-126);
- the bootloader can be blocked by an overlay tombstone/entry hiding `.rockbox` or `rockbox.ipod`; it should mount native-only for recovery (HFS-117/118);
- the Mac Cover Flow index has not been exercised on device yet;
- no hardware run was performed in this session;
- the adapter write path has no committed host integration test.

Firmware extraction and update on an already-installed, verified device may be performed autonomously once the release gates in §17 are satisfied. **Bootloader NOR writes require explicit per-device approval** until a supervised dry run has passed.

### 18.2 Conditions for autonomous install

All of the following must be true:

1. The user explicitly authorized autonomous installation for this device and session.
2. The device is identified by model, capacity, mount point and `diskutil info`; the identity is recorded.
3. A verified backup of music, `.rockbox` and `.rockpod-rw` exists and has been read back.
4. The release artifacts match the published manifest hashes.
5. The overlay layout is known and healthy, or absent for a deliberate new install.
6. No overlay conflict affects database/index/cache outputs.
7. The device is in the expected mode (Disk Mode for file copy; DFU state 2 for bootloader install).
8. The volume was cleanly ejected before DFU.
9. The commands come from this runbook; no `--single`, no format, no restore, no overlay deletion.
10. The agent logs every command and stops on the first deviation.

### 18.3 Hard stops

Abort and ask the user if:

- the device identity or mount point is ambiguous;
- a backup is missing or cannot be read back;
- `diskutil verifyVolume` reports errors;
- the overlay stage is not 9/error 0, or the layout is unknown;
- a database/index/cache overlay conflict is reported;
- the device reports DFU `WTF` or an unexpected USB product ID;
- a bootloader install emits the failure tone;
- a post-install check fails;
- the only remaining remedy is destructive (format, restore, delete overlay, `--single`).

### 18.4 Post-install report template

```text
Device: <model/revision, capacity, storage adapter>
macOS: <version>  Toolchain: <gcc/ld versions>
Artifacts: <paths + SHA-256>
Backup: <path, size, readback result>
Install mode: <firmware update | new install | bootloader install>
DFU: <product id, state>
Overlay before/after: <stage/error/slot>
Checks:
  - settings persistence: PASS/FAIL
  - database playback: PASS/FAIL
  - Cover Flow: PASS/FAIL
  - Now Playing art: PASS/FAIL
  - art diagnostics CRCs: <values>
  - clean eject: PASS/FAIL
Anomalies: <none / exact messages>
Unverified: <list>
```

---

## 19. Known issues / release notes block

Copy and update this block in release notes:

```markdown
## Rockpod HFS+ Stable V1.1 // Version V12.1

Target: iPod Classic 6G (ipod6g) only.
Filesystem: HFS+ version 4, clean unmount required. HFSX/APFS unsupported.
Overlay: /.rockpod-rw, 16 MiB minimum, 2 GiB capacity cap, 2048 entries, 4096-byte blocks.
Bootloader: Thumb build required; dual-boot only; do not use --single.
Database: build on macOS; keep on-device Auto Update off.

Known issues:
- Mac Cover Flow index seek/artist_seek fields are host-validated against
  tag-table offsets; confirm album track lists on hardware.
- Database host tool builds via restored upstream uisimulator/common glue;
  keep those files in sync with the matching Rockbox generation.
- CRITICAL: overlay can report READY while entries are absent from the
  allocation bitmap; the allocator can overwrite live files (HFS-001).
- CRITICAL: overlay frees blocks before persisting the new entry state;
  power loss/I/O error can leave dangling or overlapping blocks (HFS-002).
- CRITICAL: base-file long names can overflow fat_direntry; ASan confirmed
  a stack overflow at hfsplus_rb.c:1203 (HFS-003).
- CRITICAL: renaming a base directory can make deleted children reappear
  and newly created overlay children disappear (HFS-004).
- CRITICAL: the bootloader loads the overlay too. A stale tombstone can
  hide /.rockbox or /.rockbox/rockbox.ipod; a damaged overlay header can
  trigger fresh initialization at boot (HFS-117/118/119). Mount native-only
  in the bootloader.
- CRITICAL: a present-but-invalid overlay is silently ignored; the volume
  still permits same-size native writes (HFS-123). Fail closed instead.
- CRITICAL: overlay entry IDs are not range/identity checked; an ID that
  collides with a native CNID can redirect a native path to overlay data
  (HFS-124). Validate IDs and parent/name identity.
- CRITICAL: catalog key parent ordering is not validated; one bad key can
  make .rockbox disappear from listings with rc=0 (HFS-126). This is a
  plausible direct cause of the reported incident.
- Tombstones ignore the name and HFS+ CNIDs can be reused; .rockbox can be
  hidden by an old tombstone (HFS-090/119).
- Closing a renamed BASE shadow without writing leaks overlay blocks; the
  next write leaks another run (HFS-125, confirmed 32 KiB + 32 KiB).
- PictureFlow duplicate removal skips the first artist because it treats
  artist name offset 0 as "no artist" (HFS-127).
- Cover Flow install conflict checks match literal names only; a
  mismatched-name tombstone can hide the installed index (HFS-128).
- PictureFlow track-index locking leaks on early return/error and
  `free_borrowed_tracks()` unlocks unconditionally; repeated track-list or
  context-menu transitions can deadlock (HFS-129).
- Base and overlay IDs are not unified. Two handles for one base file can
  create duplicate shadows and divergent writes; parent/prefix checks can
  misclassify a shadowed directory (HFS-025/108/109).
- PictureFlow cache/index bounds, save checks and empty-slide handling are
  fixed in V1.1 (HFS-082/083/084/087/103/104/106/116).
- On-device PictureFlow index rebuilding is fixed in V1.1 (HFS-112/113/114/120). Keep using the Mac-generated index until the hardware reboot test is recorded.
- Deleted-base tombstone matching and CNID reuse handling are fixed in V1.1 (HFS-090).
- Partition scanner ordering/dual-unit handling is fixed and covered by host tests (HFS-006/007/008/131).
- Non-overlay EOF extension is rejected before writing (HFS-009, V1.1).
- Overlay commit ordering and ATA storage flush are fixed (HFS-039); the format still has no checksum/backup header (HFS-040). Back up .rockpod-rw and never power off during writes.
- Overlay ID hash can lose entries after long create/delete churn until a
  remount (HFS-088, confirmed after 5,000 cycles: readdir sees the file,
  open returns not-found).
- HFS+ reader is exact-byte, no Unicode normalization/case folding.
- HFS+ file/dir timestamps are not exposed; dir_get_info shows a bogus
  mtime (HFS-085).
- View disk info returns before SSD/power/SMART/flush lines on HFS+ builds
  (HFS-081).
- Mac .pfraw filenames now use <Untagged> consistently (HFS-075, V1.1).
- Adapter write path lacks a committed host integration test.
- One iPod Classic 6G tested; other revisions/adapters unverified.
- Stock Rockbox USB mass-storage corruption is a separate upstream issue;
  use Apple Disk Mode for transfers.
```

---

## 20. References

- `README.md` — user-facing architecture, installation, database, diagnostics.
- `agents.md` — invariants, overlay format, code-quality review, change recipes.
- `BUGFIXES.md` — actionable bugfix list, fix order and acceptance criteria.
- `docs/hfsplus-audit.md` — evidence-backed bug audit and fix order.
- `docs/VERSION` — release version.
- `utils/mks5lboot/README` — DFU installer and dual-boot behavior.
- `tools/hfsplus/tests/` — host fixtures and adapter harness.
- `tools/rockpod/` — Mac Cover Flow index and artwork cache.
- Apple TN1150 — HFS Plus Volume Format.
- Apple File System Programming Guide — File System Details.
- Microsoft — File System Functionality Comparison.
