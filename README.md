# Rockpod HFS+

**Stable V2.0 // Version V12.2**

Rockpod HFS+ is a Rockbox/Rockpod build for the **iPod Classic 6G (`ipod6g`)** that boots from HFS+ and keeps writable state in a preallocated `/.rockpod-rw` container. It is for Mac-formatted iPods that should stay HFS+.

Built for the iPod Classic 6G. Compatibility depends on the Classic revision, storage adapter and HFS+ layout; test on your own device before relying on it.

## Why this exists

FAT32 can be unreliable on some iPods. Rockpod HFS+ keeps the music library as ordinary HFS+ files, leaves Apple's catalog and journal untouched, and isolates Rockbox writes in the overlay. macOS can still read the music volume.

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
- Overlay: 1 GiB recommended, 16 MiB minimum, 2 GiB maximum, 2,048 entries. The container cannot be resized in place. Keep bulk music in the native HFS+ library.
- Do **not** use Rockbox USB mass-storage mode for transfers. Use Apple Disk Mode.
- Do **not** use on-device **Database Initialize/Update** or PictureFlow **Rebuild/Update Cache**. Use the Mac workflow below.
- `.rockbox` is a dot directory: use **Show Files → All** in Rockbox and `Cmd+Shift+.` in Finder.

## Installation

Download the release assets, or build them from source. Back up the iPod and `.rockpod-rw` first.

### 1. Back up, enter Disk Mode, and format HFS+

**Formatting erases the iPod.** Back up first. The HFS+ bootloader and Rockpod firmware are then installed separately.

Enter Apple Disk Mode:

1. Disconnect the iPod.
2. Hold **Menu + Select** until the screen resets or goes black.
3. Immediately hold **Select + Play/Pause** until **Disk Mode** appears.
4. Connect the USB cable and wait for the volume to mount.

Back up the entire volume, including hidden files such as `.rockpod-rw`:

```sh
backup="$HOME/Documents/ipod-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup"
ditto "/Volumes/IPOD" "$backup/IPOD"
diskutil verifyVolume "/Volumes/IPOD"
```

If `/Volumes/IPOD` is not the mount point, use the actual path. Do not skip the backup: a failed DFU install or an Apple firmware restore can erase the iPod.

A new installation must use HFS+. If the iPod is already HFS+ and contains a working Rockpod install, skip the format command and continue at step 2.

Format with the terminal, not Disk Utility. Identify the correct whole disk by its size and model, not by a disk number copied from this example:

```sh
diskutil list
diskutil info /dev/diskN
```

Replace `diskN` with the iPod's whole disk, then:

```sh
diskutil unmountDisk /dev/diskN
sudo diskutil eraseDisk JHFS+ IPOD APM /dev/diskN
```

This creates a clean Apple Partition Map with one journaled HFS+ volume. It removes the Apple firmware partition and all data. The HFS+ bootloader is required to boot Rockpod afterwards. If your macOS version rejects `APM`, use `GPT` in the same command. If you need the Apple firmware/dual-boot layout, restore the iPod with Finder before starting over.

### 2. Create the overlay only for a new install

```sh
if [ -e "/Volumes/IPOD/.rockpod-rw" ]; then
    echo "Existing overlay retained."
else
    mkfile 1g "/Volumes/IPOD/.rockpod-rw"
fi
```

`mkfile` must finish successfully. Do not use a sparse file.

### 3. Stop Apple device helpers and install the bootloader

The bootloader lives in NOR flash and is separate from `.rockbox`. A firmware-only update does not replace it.

First verify the image:

```sh
shasum -a 256 bootloader-ipod6g.ipod
```

Expected V2.0 hash:

```text
beeffd5ed8ad09ab4b52083a49b3d7a9f9d4d1329b440887bd333f69f1409855
```

Eject the volume before DFU. Keep the USB cable connected.

**Terminal 1 — Apple-device monitor.** Run this in a separate window and leave it open. It pauses the Apple helpers that would otherwise claim the iPod during DFU, and resumes them when you press **Control-C**.

```sh
python3 - <<'PYMONITOR'
import os
import signal
import subprocess
import time

names = {
    "AMPDevicesAgent", "AMPDeviceDiscoveryAgent",
    "com.apple.amp.devicesui", "iTunesHelper",
}
paused = {}

def processes():
    output = subprocess.check_output(
        ["ps", "-u", str(os.getuid()), "-o", "pid=,stat=,comm="], text=True,
    )
    result = {}
    for line in output.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) == 3:
            pid, state, command = parts
            result[int(pid)] = (state, os.path.basename(command))
    return result

def stop_monitor(signum, frame):
    raise KeyboardInterrupt

signal.signal(signal.SIGTERM, stop_monitor)
print("Apple-device monitor active. Stop with Control-C.", flush=True)
try:
    while True:
        for pid, (state, name) in processes().items():
            if name in names and "T" not in state:
                paused[pid] = name
                try:
                    os.kill(pid, signal.SIGSTOP)
                    print(f"Paused: {name} ({pid})", flush=True)
                except (ProcessLookupError, PermissionError):
                    paused.pop(pid, None)
        time.sleep(0.25)
except KeyboardInterrupt:
    pass
finally:
    current = processes()
    for pid, name in paused.items():
        if current.get(pid, (None, None))[1] == name:
            try:
                os.kill(pid, signal.SIGCONT)
            except (ProcessLookupError, PermissionError):
                pass
    print("Monitor stopped; paused helpers resumed.")
PYMONITOR
```

Do not close this window without stopping it with **Control-C**.

**Terminal 2 — DFU scanner and install.** Put the iPod into DFU by holding **Menu + Select** for about 12 seconds, releasing at the black screen:

```sh
./mks5lboot --dfuscan --loop 1
```

The scanner must report a DFU device. Stop the scanner with **Control-C**, then install:

```sh
./mks5lboot --bl-inst bootloader-ipod6g.ipod
```

Do **not** add `--single`; that removes the Apple NOR boot path. After the success tone and reboot, stop the Apple-device monitor with **Control-C**. The full DFU procedure and troubleshooting are in [`shipping.md`](shipping.md).

### 4. Install the firmware

Re-enter Apple Disk Mode, mount `/Volumes/IPOD`, then:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
sync
diskutil eject "/Volumes/IPOD"
```

`rockbox.zip` contains the complete `.rockbox` tree and must be extracted, not just `rockbox.ipod`.

### 5. Build the Rockbox database on the Mac (mandatory)

Database, Cover Flow and normal playback depend on the `.rockbox/database_*.tcd` files. Build them on the Mac. **Do not use on-device Database Initialize/Update.**

Store music in ordinary album folders:

```text
/Music/Artist/Year - Album/track.m4a
/Music/Artist/Year - Album/cover.jpg
```

Use the release `database.ipod6g` asset, or build the host tool from the source checkout. Building it needs `gcc@15` and `sdl2` (see [Build on macOS](#build-on-macos)):

```sh
tools/rockpod/build-database-macos.sh
# output: build-ipod6g-database/database.ipod6g
```

Mount the iPod in Apple Disk Mode, then back up the native database files and check that the overlay is not hiding them. Run this from the source checkout:

```sh
backup="$HOME/Documents/rockpod-backups/database"
mkdir -p "$backup"
cd "/Volumes/IPOD"
cp .rockbox/database*.tcd "$backup"/ 2>/dev/null || true

cd "/path/to/Rockpod-HFSPlus"
python3 - <<'PYOVERLAY'
from pathlib import Path
import sys
sys.path.insert(0, "tools/rockpod")
import overlay_check

names = {"database_idx.tcd", "database_1.tcd", "database_7.tcd",
         "database_8.tcd", "database_tmp.tcd"}
conflicts = overlay_check.conflicts(Path("/Volumes/IPOD/.rockpod-rw"), names)
if conflicts:
    for entry in conflicts:
        print(f"overlay slot {entry['slot']}: {entry['name']}")
    raise SystemExit("STOP: resolve overlay database conflicts before building")
print("No database overlay conflicts.")
PYOVERLAY
```

If the check stops, do not continue. Resolve the reported overlay entries using [`shipping.md`](shipping.md); do not delete `.rockpod-rw`.

Run the database tool **from the iPod root**, not with a music-directory argument. Keep the Mac awake:

```sh
backup="$HOME/Documents/rockpod-backups/database"
mkdir -p "$backup"
# Use an absolute path. Examples:
# built from source:  /path/to/source/build-ipod6g-database/database.ipod6g
# release asset:      /path/to/rockpod-v2.0/database.ipod6g
database_tool="/absolute/path/to/database.ipod6g"
cd "/Volumes/IPOD"
test -d .rockbox
test -x "$database_tool"
set -o pipefail
caffeinate -i "$database_tool" 2>&1 | tee "$backup/database-build.log"
```

Wait for the tool to finish and commit the database. Do not interrupt a database write. Then:

```sh
sync
```

### 6. Optional: Cover Flow index and artwork

Skip this section if you do not use Cover Flow. The index generator requires a committed native database from step 5; the artwork generator requires `cover.jpg` files and FFmpeg.

From the source checkout:

```sh
python3 tools/rockpod/build_rockpod_coverflow_index.py \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/rockpod-backups" \
  --install

python3 tools/rockpod/build_rockpod_pictureflow_cache.py \
  --music "/Volumes/IPOD/Music" \
  --cache "/Volumes/IPOD/.rockbox/rocks/demos/pictureflow"
```

The index script backs up the native index/config files, validates the overlay and refuses to install if a shadow or tombstone could hide the replacements. The artwork script builds all covers first and keeps old files on failure. Detailed conflict handling is in [`shipping.md`](shipping.md).

Finish with:

```sh
sync
diskutil eject "/Volumes/IPOD"
```

### 7. First boot

1. Rockpod starts normally.
2. Open **Debug → Disk Info** and confirm:
   ```text
   HFS+ overlay: READY
   Overlay stage: 9
   Overlay error: 0
   ```
3. Select a theme, wait for disk idle, power off through the Rockbox menu, and boot again. The theme should persist.
4. Confirm **Database** playback and, if installed, Cover Flow.

## Updating

A firmware-only update is:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
sync
diskutil eject "/Volumes/IPOD"
```

Install a new bootloader only when the release notes say so. After adding or retagging music, repeat the mandatory database step and, if used, the optional Cover Flow step. Use Apple Disk Mode, never Rockbox USB mode.

## Recovery

- **Overlay stage 9, error 0**: overlay is ready.
- **Stages 1–6**: container lookup/header/bitmap validation failed.
- **Stages 7–8**: an overlay entry is invalid; the debug slot identifies it.
- **Mount error -4**: the native HFS+ volume needs consistency checking or journal replay.
- **`.rockbox` missing**: do not delete or recreate the overlay. Back up `.rockpod-rw`, run `diskutil verifyVolume "/Volumes/IPOD"`, and inspect the debug values.
- **Wrong or damaged bootloader**: reinstall over DFU; firmware files alone cannot repair NOR.

A damaged overlay must be copied and inspected before repair. Deleting `.rockpod-rw` discards settings, databases, playlists and overlay files.

### If the iPod will not boot

1. Re-enter Apple Disk Mode and reinstall `rockbox.zip`.
2. If the bootloader is damaged, reinstall it over DFU using step 3.
3. As a last resort, restore the Apple firmware with Finder. This erases the iPod and all Rockpod data.

Do not delete `.rockpod-rw` as a first response; it contains settings, databases, playlists and overlay files.

### Finder does not show Rockpod-created files

macOS sees only the native HFS+ tree. Files created or changed inside Rockpod live in `.rockpod-rw` and are not merged into Finder. To inspect or back them up while the iPod is in Apple Disk Mode:

```sh
python3 tools/rockpod/export_overlay.py --ipod "/Volumes/IPOD"
```

The export is read-only and writes a normal folder under `~/Documents` with the reconstructed files and a `manifest.tsv`. It does not merge the files back into the iPod. A file whose parent directory is no longer available is placed under `by-cnid-<id>/` so it is not lost.

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
