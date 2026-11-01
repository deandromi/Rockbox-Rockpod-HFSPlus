# Rockpod HFS+

**Stable V2.0 // Version V12.2**

Rockpod HFS+ is a Rockbox/Rockpod build for the **iPod Classic 6G (`ipod6g`)** that boots from HFS+ and keeps writable state in a preallocated `/.rockpod-rw` container. It is for Mac-formatted iPods that should stay HFS+.

Tested on one iPod Classic 6G: playback, database, Cover Flow, settings and album art. It is not a guarantee for every Classic revision, storage adapter or HFS+ layout.

## Why this exists

FAT32 was unreliable on the tested iPod. Rockpod HFS+ keeps the music library as ordinary HFS+ files, leaves Apple's catalog and journal untouched, and isolates Rockbox writes in the overlay. macOS can still read the music volume.

## Quick facts

| Item | Value |
|---|---|
| Target | iPod Classic 6G / `ipod6g` (target ID 71) |
| Filesystem | HFS+ version 4; APFS and HFSX are not supported |
| Boot | Separate HFS+-aware bootloader in NOR flash |
| Safe transfer mode | **Apple Disk Mode** |
| Writable state | `/.rockpod-rw`, 4 KiB blocks, 2,048 entries max |
| Install platform | macOS |
| Release | [V2.0](https://github.com/deandromi/Rockbox-Rockpod-HFSPlus/releases/tag/v2.0) |

V2.0 resolves all remaining audit findings; six are documented format limits. See [`BUGFIXES.md`](BUGFIXES.md) and [`docs/release-v2.0.md`](docs/release-v2.0.md).

## Compatibility and limits

- macOS only for installation, database generation and recovery.
- The volume must be HFS+ and cleanly unmounted. Pending journal replay is refused; it is never replayed.
- The reader supports up to eight inline fork extents. Files needing the extents-overflow tree are rejected.
- Name lookup is exact-byte. HFS+ Unicode normalization/case folding and full `/`↔`:` name mapping are not implemented.
- Resource forks, symlinks, hardlinks, compression, encryption and HFS wrappers are unsupported.
- The `ipod6g` build supports one HFS+ data partition. Extended/logical MBR partitions are not traversed.
- Overlay: at least 16 MiB, at most 2 GiB, 2,048 entries. Keep bulk music in the native HFS+ library.
- Do **not** use Rockbox USB mass-storage mode for transfers. Use Apple Disk Mode.
- Do **not** use on-device **Database Initialize/Update** or PictureFlow **Rebuild/Update Cache**. Use the Mac workflow below.
- `.rockbox` is a dot directory: use **Show Files → All** in Rockbox and `Cmd+Shift+.` in Finder.

## Installation

Download the release assets, or build them from source. Back up the iPod and `.rockpod-rw` first.

### 1. Enter Apple Disk Mode

Connect the iPod, then verify the mounted HFS+ volume:

```sh
diskutil list
diskutil info /Volumes/IPOD
```

If the mount point differs, replace `/Volumes/IPOD` everywhere.

### 2. Create the overlay only for a new install

```sh
if [ -e "/Volumes/IPOD/.rockpod-rw" ]; then
    echo "Existing overlay retained."
else
    mkfile 1g "/Volumes/IPOD/.rockpod-rw"
fi
```

`mkfile` must finish successfully. Do not use a sparse file.

### 3. Install the bootloader (first install or bootloader update)

The bootloader lives in NOR flash and is separate from `.rockbox`. A firmware-only update does not replace it.

```sh
shasum -a 256 bootloader-ipod6g.ipod
```

Expected V2.0 hash:

```text
beeffd5ed8ad09ab4b52083a49b3d7a9f9d4d1329b440887bd333f69f1409855
```

Eject the volume, stop Apple device helpers, then scan for DFU:

```sh
./mks5lboot --dfuscan --loop 1
```

Hold **Menu + Select** for about 12 seconds, release at the black screen, and keep the cable connected. Stop the scanner with **Control-C** once DFU is detected. The Apple-device monitor and full DFU procedure are in [`shipping.md`](shipping.md).

Install and let the iPod reboot:

```sh
./mks5lboot --bl-inst bootloader-ipod6g.ipod
```

Do **not** add `--single`; that removes the Apple NOR boot path.

### 4. Install the firmware

Re-enter Apple Disk Mode, mount `/Volumes/IPOD`, then:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
sync
diskutil eject "/Volumes/IPOD"
```

`rockbox.zip` contains the complete `.rockbox` tree and must be extracted, not just `rockbox.ipod`.

### 5. First boot

1. Rockpod starts normally.
2. Open **Debug → Disk Info** and confirm:
   ```text
   HFS+ overlay: READY
   Overlay stage: 9
   Overlay error: 0
   ```
3. Select a theme, wait for disk idle, power off through the Rockbox menu, and boot again. The theme should persist.
4. Test playback through **Database** and Cover Flow.

## Updating

A firmware-only update is:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
sync
diskutil eject "/Volumes/IPOD"
```

Install a new bootloader only when the release notes say so. Use Apple Disk Mode, never Rockbox USB mode.

## Music and Cover Flow

Use ordinary album folders:

```text
/Music/Artist/Year - Album/track.m4a
/Music/Artist/Year - Album/cover.jpg
```

Build the Rockbox database on the Mac. Run this from the iPod root, not with a music-directory argument:

```sh
cd "/Volumes/IPOD"
caffeinate -i /path/to/database.ipod6g
```

Then generate and install the PictureFlow index and artwork:

```sh
python3 tools/rockpod/build_rockpod_coverflow_index.py \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/rockpod-backups" \
  --install

python3 tools/rockpod/build_rockpod_pictureflow_cache.py \
  --music "/Volumes/IPOD/Music" \
  --cache "/Volumes/IPOD/.rockbox/rocks/demos/pictureflow"
```

Finish with:

```sh
sync
diskutil eject "/Volumes/IPOD"
```

Keep **Auto Update** off. Rebuild the database and matching index/cache on the Mac after adding music. Detailed backup and overlay-conflict checks are in [`shipping.md`](shipping.md).

## Recovery

- **Overlay stage 9, error 0**: overlay is ready.
- **Stages 1–6**: container lookup/header/bitmap validation failed.
- **Stages 7–8**: an overlay entry is invalid; the debug slot identifies it.
- **Mount error -4**: the native HFS+ volume needs consistency checking or journal replay.
- **`.rockbox` missing**: do not delete or recreate the overlay. Back up `.rockpod-rw`, run `diskutil verifyVolume "/Volumes/IPOD"`, and inspect the debug values.
- **Wrong or damaged bootloader**: reinstall over DFU; firmware files alone cannot repair NOR.

A damaged overlay must be copied and inspected before repair. Deleting `.rockpod-rw` discards settings, databases, playlists and overlay files.

## Build on macOS

Use a source path without spaces. Full prerequisites and the release runbook are in [`shipping.md`](shipping.md).

```sh
brew install make coreutils gnu-sed gawk texinfo automake autoconf libtool \
  flex bison xz pkg-config gcc@15 sdl2 python ffmpeg
export PATH="$(brew --prefix coreutils)/libexec/gnubin:$(brew --prefix gnu-sed)/libexec/gnubin:$(brew --prefix texinfo)/bin:$(brew --prefix flex)/bin:$(brew --prefix bison)/bin:$PATH"
```

ARM toolchain:

```sh
bash tools/rockboxdev.sh --target=a \
  --prefix="$HOME/rockbox-toolchain" \
  --makeflags="-j$(sysctl -n hw.ncpu)"
export PATH="$HOME/rockbox-toolchain/bin:$PATH"
```

Firmware:

```sh
mkdir -p build-ipod6g && cd build-ipod6g
../tools/configure --target=ipod6g --type=n
make -j"$(sysctl -n hw.ncpu)"
make -j"$(sysctl -n hw.ncpu)" zip
```

Thumb bootloader:

```sh
mkdir -p build-ipod6g-bootloader && cd build-ipod6g-bootloader
../tools/configure --target=ipod6g --type=b --thumb
make -j"$(sysctl -n hw.ncpu)"
```

Database host tool:

```sh
tools/rockpod/build-database-macos.sh
```

DFU utility and image:

```sh
make -C utils/mks5lboot -j"$(sysctl -n hw.ncpu)" CC=clang \
  LDOPTS="-framework IOKit -framework CoreFoundation"
./utils/mks5lboot/mks5lboot --mkdfu-inst \
  build-ipod6g-bootloader/bootloader-ipod6g.ipod \
  build-ipod6g-bootloader/bootloader-ipod6g.dfu
```

## Verification

```sh
make -C tools/hfsplus test
make -C tools/hfsplus sanitize
python3 tools/hfsplus/tests/check_integration.py
```

V2.0 passes 19 reader tests, 7 partition tests and 15 adapter tests, including ASan/UBSan. Firmware, bootloader, database tool, `mks5lboot`, DFU and `rockbox.zip` build successfully. The release hashes are in [`docs/release-v2.0.md`](docs/release-v2.0.md).

## License and credits

Rockpod HFS+ is based on [Rockpod by nuxcodes](https://github.com/nuxcodes/rockpod), itself a fork of [Rockbox](https://www.rockbox.org). Rockbox and this project are licensed under GPL-2.0-or-later; see [`COPYING`](COPYING) and [`docs/CREDITS`](docs/CREDITS).

The HFS+ layer, bootloader changes and audit fixes are project-specific. The original Rockpod features remain credited to their authors.
