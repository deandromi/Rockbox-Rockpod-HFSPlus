# Rockpod HFS+

**Stable V1.0 // Version V12.0**

Rockpod HFS+ is a firmware variant based on [Rockpod by nuxcodes](https://github.com/nuxcodes/rockpod), itself a fork of [Rockbox](https://www.rockbox.org), for the iPod Classic `ipod6g` target. It adds an HFS+ reader, an HFS+-aware bootloader, and a writable overlay for firmware settings, databases, playlists and other files created on the player. The release incorporates the development fixes through v12.17; the public release name is Stable V1.0 // Version V12.0.

The working firmware has been built on macOS and tested on one iPod Classic 6G. Playback through the database and Cover Flow, configuration writes, and Now Playing album art have been confirmed on that device. “Stable” identifies this project's first working release. It does not certify every iPod Classic revision, storage adapter or HFS+ volume layout.

**I made this HFS+ version of rockpod/rockbox on myself, A.I. was only used for bug fixing and the final commit to the repository.** The reason I started tinkering with rockpod was because FAT32 would be extremely unstable on my iPod and would occasionally corrupt my file system. Even if you don't have any problems with FAT32 on your iPod whatsoever, I very much recommend trying this firmware and bootloader out. Because eventually, in my experience, *FAT32 always corrupts*. And please keep in mind, this installing process requires formatting which wipes any previous information, music, photography etc. from your iPod. 

## Contents

- [Architecture](#architecture)
- [HFS+ and FAT32](#hfs-and-fat32)
- [Compatibility and limits](#compatibility-and-limits)
- [Installation](#installation)
- [Building on macOS](#building-on-macos)
- [Music, database and album art](#music-database-and-album-art)
- [Updating](#updating)
- [Diagnostics and recovery](#diagnostics-and-recovery)
- [Development and verification](#development-and-verification)
- [License and sources](#license-and-sources)

## Architecture

### Bootloader and firmware

The bootloader is a separate program installed in the iPod's NOR flash. It starts the normal Rockpod firmware from `/.rockbox/rockbox.ipod`. An ordinary Rockbox bootloader that cannot read HFS+ cannot be made HFS+-aware by replacing `.rockbox`. The HFS+ bootloader must be built and installed separately for the initial conversion.

Firmware installation consists of extracting `rockbox.zip` into the root of the mounted iPod. Firmware updates normally do not require another bootloader installation. The supplied source uses `bootloader/ipod-s5l87xx.c` for the Classic bootloader and `utils/mks5lboot` for its DFU installer.

### Native volume and writable overlay

The base music library and files copied by macOS remain ordinary HFS+ files. Rockpod reads the HFS+ catalog and file extents through its filesystem adapter. Files created or resized by Rockpod are stored in a preallocated file named `/.rockpod-rw`; the adapter merges these entries with the base namespace.

The overlay stores its own allocation bitmap, entry table, file data and deletion records. It does not make native changes to the HFS+ catalog, allocation bitmap or journal. The base namespace can therefore remain readable by macOS while Rockpod stores its writable state in the container.

The adapter also retains an older path for same-size writes to existing files with supported inline extents. The distinction is relevant to backup and recovery: an unavailable overlay prevents normal file creation and growth, but does not turn every possible adapter write into a read-only operation.

macOS sees `.rockpod-rw` as one file. It does not expose the overlay's internal files in Finder. A `config.cfg` visible in `.rockbox` on the Mac can therefore differ from the configuration currently selected by Rockpod. An overlay entry can also shadow a base file with the same logical path. Copying a file onto the Mac-visible volume does not automatically remove that overlay entry.

### Performance implementation

The HFS+ adapter builds a directory index at mount, caches catalog nodes and transfers aligned disk data in batches of up to 16 KiB. Overlay lookups use RAM indexes for identifiers and parent directories. A cached free-block count and allocation hint reduce repeated bitmap scans. Allocation grows geometrically, and metadata updates are grouped around close and synchronization operations.

These are implementation choices intended to reduce repeated I/O. They are not a benchmark showing that HFS+ is faster than FAT32. The current source reads and validates overlay table entries individually.

## HFS+ and FAT32

HFS+ is Apple's “Mac OS Extended” filesystem. FAT32 is the conventional filesystem for Windows-formatted Rockbox iPods. Both can store an ordinary music library; the reason to use this project is compatibility with an existing Mac-formatted HFS+ iPod and its Mac-based workflow.

| Property | HFS+ | FAT32 | Consequence for this release |
|---|---|---|---|
| macOS workflow | Native Mac OS Extended format | Supported by macOS | HFS+ allows a supported Mac-formatted library to remain HFS+. |
| Catalog organization | B-tree catalog and allocation extents | Directory entries and allocation chains | Rockpod adds catalog caching and a directory index; relative speed has not been measured. |
| Journaling | Available for volume metadata | No filesystem journal | Rockpod accepts only supported clean journal states and does not replay the journal. Overlay writes do not gain native HFS+ journal protection. |
| File size | HFS+ data-fork sizes use 64-bit fields | Individual files are limited to less than 4 GiB | The compatibility adapter and overlay still impose their own limits. This release is not a promise of unrestricted large-file playback. |
| Filename model | Unicode names and HFS+ comparison rules | Long filenames with FAT rules | This reader converts UTF-16 to UTF-8 but does not implement complete HFS+ normalization and case folding. |
| Other operating systems | Additional software can be required | Broad native support | FAT32 remains the simpler choice for a mixed Windows/macOS workflow. |
| Rockbox write implementation | Custom adapter and container | Existing native FAT implementation | HFS+ here has additional limitations and a smaller hardware test history. |

HFS+ is could be a remedy for failing storage or a loose cable on the inside of the iPod. But if possible, please fix the hardware first before you start tinkering with the software of your iPod.

## Compatibility and limits

### Device and volume

- The firmware target is `ipod6g`, with 64 MiB of memory in this source configuration. This is not an installation package for a Nano, Video or another Rockbox target.
- The confirmed hardware test is an iPod Classic 6G. Other Classic revisions and storage combinations require testing.
- HFS+ version 4 is supported; APFS and HFSX are not supported volume formats.
- The reader contains APM, MBR, primary GPT and raw-volume parsing. The adapter also contains iPod-specific sector-layout handling. This does not establish that every combination of partition map and sector size is supported on hardware.
- The volume must have been unmounted cleanly. Supported journaled volumes must have an internal, initialized and empty journal with valid metadata. Pending transactions are not replayed.
- The reader uses up to eight inline data-fork extents. Files requiring the extents-overflow tree are unsupported, including an overlay container whose allocation cannot be described by its inline extents.
- Resource forks, filesystem symlinks, hardlinks, macOS compression, encryption and HFS wrappers are unsupported.
- The adapter retains size and naming restrictions from its FAT compatibility interface.

Changing the filesystem does not establish support for a larger disk than the device's storage and Apple firmware paths can address. Capacities above a particular iPod revision's normal limits require separate verification.

### Overlay

- The container must be an ordinary, fully allocated HFS+ file of at least 16 MiB.
- Overlay blocks are 4,096 bytes.
- The implementation uses at most 524,288 blocks: 2 GiB, including metadata.
- The entry table contains 2,048 slots. It is not an unlimited second music filesystem.
- A 1 GiB container is a practical starting point for configuration and databases; actual requirements depend on library and cache size.
- Keep bulk music and album files in the native HFS+ namespace through the Mac. Monitor overlay space when building large caches on the player.

The persistent format retains the `RPOVL11` identifier and format version 1. Its name is an internal compatibility identifier, not the public release version. A damaged overlay must be backed up and inspected before repair. Zeroing its header causes the firmware's fresh-container initialization path to run and can destroy access to the previous overlay contents.

## Installation

**This release supports HFS+ installation on macOS only.** The instructions, volume preparation and DFU workflow below require a Mac. Windows and Linux installation are outside this release's supported scope.

### Preparation

Use a supported iPod Classic with working Apple firmware and a clean HFS+ data volume. Back up the music library and `.rockbox`. If `.rockpod-rw` already exists, back it up as well. These are distinct parts of the filesystem state.

Connect the iPod in Apple Disk Mode. From a reset with Menu + Select, release the reset combination when the screen goes black and hold Select + Play/Pause to enter Disk Mode. Verify the volume in Finder and inspect its format with:

```sh
diskutil list
diskutil info "/Volumes/IPOD"
```

Replace `/Volumes/IPOD` throughout the instructions if the actual mounted name differs. Identify the device by its reported size and name, not by a disk number copied from an example. A fresh Apple restore erases the device and should be used only as part of a deliberate new installation. This release does not include a tested automated partition-conversion procedure. Keep the Apple firmware and partition layout intact when preparing the HFS+ data volume.

### Firmware files

Obtain the firmware archive from this repository's release downloads when a packaged release is available, or build it using the instructions below. A GitHub source-code archive is not `rockbox.zip`: source code must be built first.

From the directory containing the built or downloaded `rockbox.zip`:

```sh
unzip -o rockbox.zip -d "/Volumes/IPOD"
```

The resulting path must be `/Volumes/IPOD/.rockbox/rockbox.ipod`, not a nested `.rockbox` under a source folder. Stop if extraction reports an error. A missing `/Volumes/IPOD` often means the iPod is not mounted at that path; creating a directory with that name does not mount the iPod.

### New overlay container

Do this only for a new installation without an existing `.rockpod-rw`. The shell guard prevents replacing an existing container:

```sh
if [ -e "/Volumes/IPOD/.rockpod-rw" ]; then
    printf '%s\n' 'Existing overlay retained; no file created.'
else
    mkfile 1g "/Volumes/IPOD/.rockpod-rw"
fi
```

`mkfile` must finish successfully. Do not use its sparse-file option. The allocation must fit within the supported inline extents. On the first firmware start, Rockpod initializes an uninitialized container; it does not create or extend the native HFS+ container itself.

Finish filesystem transfers before the DFU installation:

```sh
sync
diskutil eject "/Volumes/IPOD"
```

### Bootloader installation

Build `bootloader-ipod6g.ipod` with `--thumb` and build `mks5lboot` as described below. This Thumb bootloader was installed on the tested HFS+ iPod Classic 6G; startup and music playback were confirmed after a clean volume eject. It is a new build, not a recovered copy of the previously installed bootloader.

Close Music/iTunes and restore dialogs. After a successful volume eject, run this temporary Apple-device monitor in a separate Terminal window. It pauses the named helpers as they appear, rather than repeatedly terminating helpers that macOS may relaunch. Process names vary between macOS versions; verify the actual DFU mode with the scan below. The monitor does not disable a service permanently.

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

Keep this window open during scanning and installation. Stop the monitor with **Control-C** afterwards so it resumes the helpers it paused; do not simply close the window. Start the DFU scan in a second Terminal window, from the source repository:

```sh
./utils/mks5lboot/mks5lboot --dfuscan --loop 1
```

Hold Menu + Select for approximately 12 seconds until the iPod enters DFU and the scan detects it; the screen remains black in DFU. The tested device reported `[05ac:1223]`, `mode: DFU`, and `DFU device state: 2`. A `WTF` result is a different recovery mode; do not proceed with these installation steps in that mode. Stop the scan with Control-C. Finder's restore prompt is not required for bootloader installation. Then install the HFS+-aware bootloader:

```sh
./utils/mks5lboot/mks5lboot --bl-inst \
  ./build-ipod6g-bootloader/bootloader-ipod6g.ipod
```

This invokes the normal dual-boot installer. Do not add `--single` to these instructions: that option destroys the existing Apple NOR boot path. The installer documentation describes an initial alive tone, a success tone and automatic reboot. If installation fails, retain its complete output rather than repeatedly sending other bootloader images.

When installation finishes, stop the Apple-device monitor with Control-C to resume the helpers.

DFU is a transport for installation and execution. This installer does not offer a command to recover the original bootloader file from an already flashed iPod.

### First start

Start Rockpod and open the debug disk-information screen. A working initialized overlay should report:

```text
HFS+ overlay: READY
Overlay stage: 9
Overlay error: 0
Overlay slot: -1
```

Confirm that a setting survives restart, that music opens through the database and Cover Flow, and that Now Playing renders the album art correctly. Firmware installation, writable-overlay initialization and bootloader installation are separate operations; completing one does not prove that the others succeeded.

## Building on macOS

### Source location

Use a source path without spaces. The generated Rockbox Makefiles contain absolute paths, so moving an existing build directory can leave it pointing at its old source root. Create fresh build directories after changing the source location.

```sh
mkdir -p "$HOME/Developer"
cd "$HOME/Developer"
git clone https://github.com/deandromi/Rockpod-HFSPlus.git
cd Rockpod-HFSPlus
```

For a source archive, extract it into a similarly simple path. Keeping a personal storage folder with spaces is possible through a stable symlink, but the instructions use a direct source path to avoid that dependency.

### Dependencies and ARM compiler

The working firmware build used `arm-elf-eabi-gcc` 9.5.0 and GNU Binutils 2.38. Apple's Clang compiles host tools, not the ARM firmware in this build configuration. A generic `arm-none-eabi` package is not automatically an equivalent Rockbox toolchain.

Install Apple's command-line tools if absent:

```sh
xcode-select --install
```

With Homebrew available, install the toolchain build dependencies:

```sh
brew install make coreutils gnu-sed gawk texinfo automake autoconf libtool \
  flex bison xz pkg-config
```

Expose the GNU and keg-only programs to the build script:

```sh
export PATH="$(brew --prefix coreutils)/libexec/gnubin:$(brew --prefix gnu-sed)/libexec/gnubin:$(brew --prefix texinfo)/bin:$(brew --prefix flex)/bin:$(brew --prefix bison)/bin:$PATH"
```

If the Rockbox ARM toolchain is already installed, add its `bin` directory to `PATH` and verify it:

```sh
arm-elf-eabi-gcc --version
arm-elf-eabi-ld --version
```

To build that toolchain from the supplied script:

```sh
bash tools/rockboxdev.sh --target=a \
  --prefix="$HOME/rockbox-toolchain" \
  --makeflags="-j$(sysctl -n hw.ncpu)"
export PATH="$HOME/rockbox-toolchain/bin:$PATH"
```

The script downloads upstream compiler sources and builds them. Its structured patches under `tools/toolchain-patches` are dependencies, not abandoned Rockpod update patches. Read the script's error output if a dependency, download or compiler configuration fails. A preexisting toolchain can be reused; rebuilding it is not part of every firmware update.

### Firmware

From the source root:

```sh
mkdir -p build-ipod6g
cd build-ipod6g
../tools/configure --target=ipod6g --type=n
make -j"$(sysctl -n hw.ncpu)"
make -j"$(sysctl -n hw.ncpu)" zip
```

Run the next command only after the preceding command succeeds. `rockbox.ipod` is the main firmware image; `rockbox.zip` is the complete installation archive with codecs, plugins and packaged themes. Copying only the main image is not a complete initial installation.

### Bootloader

From the source root:

```sh
mkdir -p build-ipod6g-bootloader
cd build-ipod6g-bootloader
../tools/configure --target=ipod6g --type=b --thumb
make -j"$(sysctl -n hw.ncpu)"
```

The output is `build-ipod6g-bootloader/bootloader-ipod6g.ipod`. Use `--thumb` for the bootloader: the ARM-mode build exceeded `MOVE_AREA` by 3,552 bytes with ARM GCC 9.5.0; the Thumb build linked successfully and produced an approximately 97 KiB image. The Thumb bootloader was subsequently installed and confirmed to boot and play music on the tested device. Building it does not flash the device. The configure target name is `ipod6g`; target identifier 71 is not the configure menu number.

Build the Mac DFU utility separately from the source root:

```sh
make -C utils/mks5lboot -j"$(sysctl -n hw.ncpu)" CC=clang \
  LDOPTS="-framework IOKit -framework CoreFoundation"
```

Its macOS backend uses IOKit and CoreFoundation. The resulting executable is `utils/mks5lboot/mks5lboot`.

### macOS database host tool

Build the music database on the Mac for this release. On-device database initialization was extremely unstable on the tested iPod, so it is not the installation or maintenance workflow documented here. The host program is separate from the ARM firmware: it scans audio tags and commits native `.rockbox/database_*.tcd` files while the iPod is mounted in Apple Disk Mode.

Use the source checkout in a path without spaces. The macOS `strlcpy`/`strlcat` compatibility fix and computer-tool `logf` fix are already included; do not apply the old development patches again. Configure currently uses the inherited Homebrew GCC selection on macOS before the generated host Makefile is corrected to native Clang. Install the native configure dependencies if absent:

```sh
brew install gcc@15 sdl2
```

Create a dedicated database build directory, separate from the firmware and bootloader builds:

```sh
cd "$HOME/Developer/Rockpod-HFSPlus-build" &&
mkdir -p build-ipod6g-database &&
cd build-ipod6g-database &&
../tools/configure --target=ipod6g --type=d
```

Correct only this host build's generated Makefile. This avoids accidentally building a Mac program with the ARM compiler and removes flags unsupported by Apple's Clang:

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
ls -lh database.ipod6g
file database.ipod6g
```

`database.ipod6g` must be a native macOS executable, rather than an ARM firmware image. Regenerating the Makefile overwrites the compiler corrections; repeat them after reconfiguration. If reusing an old host build produces a missing dependency such as `build-ipod6g-database/time.h`, clear that build directory's generated dependencies and objects before rebuilding:

```sh
rm -f make.dep make.dep_
find . -type f -name '*.o' -delete
make -j"$(sysctl -n hw.ncpu)" dep &&
make -j"$(sysctl -n hw.ncpu)"
```

These cleanup commands belong inside `build-ipod6g-database`, not the source root or the iPod. The database tool is built once; reuse it for subsequent music-library rebuilds. Follow the Mac database procedure below to run it.

## Music, database and album art

The inherited Rockpod source also provides its Cover Flow interface, dynamic album-art colors, SSD-aware power management and MFi digital audio support for the Classic. These are upstream Rockpod features, not features invented by the HFS+ adapter. Compatibility with individual digital docks and all inherited features was not established by the HFS+ playback tests.

Store music in ordinary album directories on the base HFS+ volume, for example:

```text
/Music/Artist/Year - Album/track.m4a
/Music/Artist/Year - Album/cover.jpg
```

### Build the Rockbox music database on the Mac

The Rockbox music database contains the audio tags used by **Database** and Cover Flow. Build it with the native `database.ipod6g` program described above. The Python **index** script then creates PictureFlow's album index from the completed music database; the Python **cache** script creates the `.pfraw` covers. Neither Python script scans and builds the Rockbox music database itself.

For Stable V1.0, use **Mac database → Mac PictureFlow index → Mac artwork cache → clean eject → playback**. Do not use **Initialize Now** or **Update Now** on the iPod as part of this workflow: rebuilding there was extremely unstable on the tested device. In **Settings → General Settings → Database**, keep **Auto Update** set to **No**. For the initial database verification, leave **Load to RAM** off so that disk-based database access is checked first; loading to RAM is a separate setting, not a database-building step.

1. Finish copying the music to `/Music`. Use consistent **Album** and **Album Artist** tags; the artwork script falls back to **Artist** when Album Artist is absent. Put a square `cover.jpg` in each album directory for the artwork generator.
2. Mount the iPod in **Apple Disk Mode** on the Mac. Verify its actual mount point and that `.rockbox` exists. These commands assume `/Volumes/IPOD`.
3. Before a full rebuild, back up the native database files and check for overlay entries that could hide the replacements. The following command reads the overlay without changing it, copies all matching native files to the Documents backup folder, verifies those copies, and only then removes the native originals. It stops before changing iPod files if database overlay entries or an unknown overlay layout are found.

```sh
python3 - <<'PYDB'
from pathlib import Path
import shutil
import struct
import tempfile

root = Path('/Volumes/IPOD')
rb = root / '.rockbox'
if not root.is_mount() or not rb.is_dir():
    raise SystemExit('STOP: mount the iPod in Apple Disk Mode and verify .rockbox.')

overlay = root / '.rockpod-rw'
if overlay.exists():
    with overlay.open('rb') as f:
        header = f.read(40)
        if len(header) != 40 or header[:8] != b'RPOVL11\0':
            raise SystemExit('STOP: unknown overlay header; no files changed.')
        version, block, count, total, bitmap = struct.unpack_from('<5I', header, 8)
        if ((version, block, count) != (1, 4096, 2048)
                or total * block > overlay.stat().st_size
                or bitmap != (total + 7) // 8):
            raise SystemExit('STOP: unknown overlay layout; no files changed.')
        f.seek((1 + (bitmap + block - 1) // block) * block)
        conflicts = []
        for slot in range(count):
            entry = f.read(320)
            if len(entry) != 320:
                raise SystemExit('STOP: truncated overlay table; no files changed.')
            flags = struct.unpack_from('<I', entry, 8)[0]
            if not flags & 1:
                continue
            length = struct.unpack_from('<H', entry, 38)[0]
            if length > 260:
                raise SystemExit('STOP: invalid overlay entry; no files changed.')
            name = entry[40:40 + length].decode('utf-8')
            if name.startswith('database_'):
                conflicts.append((slot, name))
        if conflicts:
            for slot, name in conflicts:
                print(f'Overlay database entry: slot {slot}: {name}')
            raise SystemExit('STOP: database overlay entries may shadow the Mac files; no files changed.')

files = sorted(rb.glob('database_*.tcd'))
marker = rb / 'database_commit.ignore'
if marker.exists():
    files.append(marker)
if any(not p.is_file() or p.is_symlink() for p in files):
    raise SystemExit('STOP: unexpected database file type; no files changed.')
parent = Path.home() / 'Documents/iPod bestanden/Backups'
parent.mkdir(parents=True, exist_ok=True)
backup = Path(tempfile.mkdtemp(prefix='rockpod-database-', dir=parent))
for source in files:
    dest = backup / source.name
    shutil.copy2(source, dest)
    if source.read_bytes() != dest.read_bytes():
        raise SystemExit('STOP: backup verification failed; originals retained.')
for source in files:
    source.unlink()
print(f'Backed up {len(files)} native database files to {backup}')
PYDB
```

If that check stops, do not continue with the host build. An overlay entry can contain an old database file or deletion record that hides a Mac-written replacement. Removing native files in Finder does not remove such an entry. Preserve `.rockpod-rw` and diagnose the reported slots before proceeding; do not wipe the overlay, because it also holds settings and playlists. This is a conservative filename check, not a general overlay repair tool.

4. Run the native tool **from the iPod volume root**, keeping the Mac awake and saving its output in Documents:

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

The program scans from the mounted player's root, including `/Music`, and commits the database on the Mac. It does not accept a music-directory argument. Leave the iPod connected until it finishes; do not interrupt a database write. A full rebuild replaces the native tag database and can lose runtime statistics from that database; the backup retains its previous files. If the tool reports a failure, retain the log and backup rather than continuing to boot a partially built database.

5. Keep the iPod mounted. Follow the index and artwork commands below in that order. The index script validates that the native music database is non-empty, compatible, committed and has no pending temporary database. Do not reboot into Rockpod between creating the database and installing the matching index/cache.
6. Synchronize, eject and disconnect as described below. Start Rockpod, confirm **HFS+ overlay: READY**, then test playback through **Database** and Cover Flow. After adding or retagging music, repeat the Mac database rebuild and refresh the matching PictureFlow index/cache; keep on-device **Auto Update** disabled.

macOS sees the base HFS+ files, rather than the merged view used by Rockpod. A Mac-visible `database_idx.tcd` alone therefore does not establish that it is the database used by the player. The overlay check above is part of the host workflow for an existing installation.

### Optional Mac helpers for Cover Flow

The repository includes the two host scripts in [`tools/rockpod`](tools/rockpod). They serve different purposes:

| Script | Input | Output |
| --- | --- | --- |
| [`build_rockpod_coverflow_index.py`](tools/rockpod/build_rockpod_coverflow_index.py) | An existing, committed Rockbox music database visible to macOS | `pictureflow_album.idx`, `pictureflow.cfg`, `emptyslide.pfraw`, and a local validation report |
| [`build_rockpod_pictureflow_cache.py`](tools/rockpod/build_rockpod_pictureflow_cache.py) | Album directories containing audio tags and `cover.jpg` | Album `.pfraw` artwork files; the existing album index is left unchanged |

Use these helpers while the iPod is mounted in **Apple Disk Mode** on the Mac. Back up `.rockbox` and `.rockpod-rw` before replacing the cache. Replace `/Volumes/IPOD` in the commands if the volume has another name. Python 3.9 or later is required; the artwork script also requires FFmpeg and FFprobe:

```sh
brew install python ffmpeg
```

The index helper requires native `.rockbox/database_idx.tcd`, `database_1.tcd` and `database_7.tcd`, with the v12 database format (`0x54434810`), at least one track, and no pending `database_tmp.tcd`. These must be the same database files used by Rockpod. **If the current database exists only in the overlay, or overlay entries shadow the native database, this helper cannot build the correct index from the Mac-visible files.** It does not extract database files from `.rockpod-rw`. In that case, stop and resolve the database overlay conflict before using the Mac helpers. A native index created from an older base database can point to the wrong album or track.

First validate and stage an index without changing the iPod. Use the same checkout as the database tool; the staging files and backups are saved under Documents:

```sh
rockpod_source="$HOME/Developer/Rockpod-HFSPlus-build"

python3 "$rockpod_source/tools/rockpod/build_rockpod_coverflow_index.py" \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/iPod bestanden/Backups"
```

If the Python scripts are in the original Documents source folder, set `rockpod_source="$HOME/Documents/iPod bestanden/rockpod-hfsplus-ipod6g-v12"` instead; Python can run from paths containing spaces. The script prints the staging directory containing the three generated files and `report.json`. Without `--install`, it does not modify the iPod. Once the native database is known to be current, install the index with:

```sh
python3 "$rockpod_source/tools/rockpod/build_rockpod_coverflow_index.py" \
  --ipod "/Volumes/IPOD" \
  --output-dir "$HOME/Documents/iPod bestanden/Backups" \
  --install
```

Installation backs up existing index/config/fallback files locally and reads back the replacements. It refuses installation when active overlay entries with those output filenames could hide the new files. That check does not establish that the native music database is current; the database visibility requirement above still applies. If it reports a conflict, stop and retain the report. Deleting the overlay is not a routine solution.

Build and install the album artwork next:

```sh
python3 "$rockpod_source/tools/rockpod/build_rockpod_pictureflow_cache.py" \
  --music "/Volumes/IPOD/Music" \
  --cache "/Volumes/IPOD/.rockbox/rocks/demos/pictureflow"
```

The artwork script builds all covers in a temporary directory before replacing album cache files. If any album fails, it does not install any generated covers. It reports missing or unusable tags and detects conflicting cache hashes. It writes 128 × 128 little-endian RGB565 images in PictureFlow's transposed layout, using the album/album-artist hashes for filenames. Use square `cover.jpg` files to avoid stretching. By default it removes old album `.pfraw` files before copying the new ones; it preserves `emptyslide.pfraw`, the album index and macOS `._` sidecars. Add `--keep-old` to retain existing album files while replacing files with matching generated names. This is a staged generation followed by per-file installation, rather than an atomic replacement of the entire cache directory.

The artwork helper writes native HFS+ files and does not inspect or modify overlay cache entries. Existing overlay copies can still hide those replacements on the player. If artwork on the Mac and player differs, account for overlay shadowing before regenerating files again.

Finish with:

```sh
sync
diskutil eject "/Volumes/IPOD"
```

Disconnect and start Rockpod, then open Cover Flow. **Rebuild Cache** or **Update Cache** on the player can replace the Mac-generated artwork with artwork generated by Rockpod. To preserve the Mac-generated cache, rerun the host helpers after relevant database/artwork changes and avoid those on-device cache commands. These `.pfraw` files affect Cover Flow; Now Playing continues to load external or embedded artwork separately.

### Now Playing album art

Now Playing and PictureFlow/Cover Flow use different image paths. Now Playing loads external or embedded album art into the playback buffer. PictureFlow uses its own cached `.pfraw` images and index. Regenerating `.pfraw` files does not repair a JPEG decoder error in Now Playing, and a working Now Playing cover does not prove that the PictureFlow cache is current.

The Settings menu's album-art preference selects external image files or embedded images first, with the fallback behavior implemented in `apps/playback.c`. External image searches include track, album and cover filenames and size-specific variants. The actual requested dimensions come from the theme: a `cover.120x120.bmp` is not proof that a theme requesting 112 × 112 used that file.

The JPEG repair in this release handles supported full-resolution color component grouping and selects the quantization tables identified by the JPEG header. The reproducible test cover generated an incorrect 112 × 112 bitmap with CRC `f404e0ca` before the repair and a correct bitmap with CRC `85780f2f` after it, with dithering disabled in the tested playback loader. Other dimensions or image files produce different CRCs.

To inspect the actual file being loaded, open **View album art diagnostics** while a track is playing. It reports the source path, decoded dimensions, first pixels and checksums at load, draw and inspection. Equal checksums indicate stability of the measured pixel bytes; they do not by themselves establish that the decoder generated the correct image.

## Updating

Back up `.rockbox` and `.rockpod-rw`, connect through Apple Disk Mode, extract the new `rockbox.zip`, synchronize, eject and restart. Do not replace or recreate the overlay as a routine update: it can contain the current settings, playlists and database.

The internal overlay format is unchanged from the working development version. A firmware replacement normally leaves the existing HFS+-aware bootloader installed. A future release that requires a new bootloader or overlay format must state that explicitly.

## Diagnostics and recovery

| Symptom | Check and interpretation |
|---|---|
| Configuration write error `-83` | Check overlay status first. This development failure occurred while normal create/truncate operations were blocked because the overlay had been disabled. |
| Overlay stage 1 | The adapter was locating `.rockpod-rw`; verify its existence and spelling. |
| Overlay stage 2 | Container type, allocation, inline extents or size did not pass validation. |
| Overlay stages 3–6 | Header or allocation bitmap loading/validation failed. |
| Overlay stages 7–8 | Reading or validating an entry failed; the slot identifies the relevant record. |
| Overlay stage 9, error 0 | Overlay initialization/loading completed. |
| Overlay stage 10 | Fresh-container initialization was being performed or failed. |
| Good Cover Flow, damaged Now Playing art | Inspect the Now Playing source and decoded pixels; the two renderers do not share the same image-loading path. |
| `tools/root.make` missing at an old path | The source was moved after configure generated the Makefile. Reconfigure in a fresh build directory. |
| `patch` asks whether to reverse a change | The release contains the development fixes already. Old iterative patches are not an installation step. |
| `/Volumes/IPOD` missing or permission denied | Verify the actual mount with `diskutil list` and Finder before copying. |

The development overlay corruption was reproduced in the bytes read on the Mac. A targeted recovery cleared one invalid entry's used flags while retaining the bitmap and other entries, and a write-range guard was added. The cause of the original overwrite has not been established. That recovery procedure was specific to one damaged overlay; it is not a universal repair command.

Before repairing a damaged overlay, make a full copy while the player is in Apple Disk Mode. Do not delete `.rockpod-rw` to resolve an unexplained error. The firmware's debug values and a read-only inspection of the container are needed to distinguish a format problem from storage damage. Recreating the container discards its writable state.

For a volume verification on macOS:

```sh
diskutil verifyVolume "/Volumes/IPOD"
```

This checks the native filesystem, not the internal overlay format. Native HFS+ repair and overlay repair are different operations. A missing Finder file can also be an overlay-only file rather than a file lost from the base catalog.

### Startup mount error -4

The HFS+ reader returns `-4` when the volume header does not indicate a clean unmount, indicates inconsistency, or the journal contains transactions requiring replay. The bootloader may display the inherited text `HFS+ v8 mount error: -4` and `volume requires consistency checking or journal replay`. The `v8` text is an old diagnostic label, not proof that an old bootloader was installed. This error alone does not establish data loss.

This occurred on the tested device after entering DFU without a clean eject. macOS verification reported the volume was OK; safely ejecting it and rebooting restored normal startup and playback. Do not bypass the reader's consistency checks.

1. Stop the Apple-device monitor with Control-C so the paused helpers resume.
2. Enter Apple Disk Mode: reset with Menu + Select, then hold Select + Play/Pause when the screen turns black. Connect the iPod to the Mac.
3. Confirm the actual mounted name, then inspect and verify the volume:

   ```sh
   diskutil info "/Volumes/IPOD"
   diskutil verifyVolume "/Volumes/IPOD"
   ```

4. If verification reports the volume is OK with exit code 0, eject it. Verification remounts the volume, so a successful check alone does not replace this step:

   ```sh
   diskutil eject "/Volumes/IPOD"
   ```

5. Wait for successful ejection, unplug USB, and reset with Menu + Select. Release the buttons and check startup and music playback.

If verification reports errors or ejection fails, stop and retain the complete output. Do not format, restore, delete `.rockpod-rw`, or run a whole-disk partition repair solely because of this bootloader message. The tested disk also reported unused whole-disk space; that notice did not prevent successful volume verification or playback.

Always finish transfers and successfully eject the volume **before** entering DFU. Keep the USB cable connected for the DFU installation; ejecting a volume and unplugging the cable are different operations.

## Development and verification

The repository contains source, build scripts, required assets and retained upstream licenses. Development build directories, `.orig` copies, local executables, macOS resource metadata, AI instruction files and obsolete standalone update patches are excluded. Android packaging, the upstream manual and simulator packaging are outside this Classic hardware release's scope.

The main implementation files are:

| Path | Role |
|---|---|
| `firmware/hfsplus/hfsplus_ro.c` | HFS+ reader and catalog indexing |
| `firmware/hfsplus/hfsplus_partition.c` | Partition-map detection |
| `firmware/hfsplus/hfsplus_rb.c` | Rockbox compatibility adapter and writable overlay |
| `bootloader/ipod-s5l87xx.c` | Classic bootloader integration |
| `utils/mks5lboot` | Bootloader DFU installer |
| `apps/recorder/jpeg_load.c` | JPEG decoding repair |
| `apps/buffering.c` | Playback image loading and pixel diagnostics |
| `apps/debug_menu.c` | Disk and album-art diagnostic screens |
| `tools/hfsplus/tests` | Synthetic host-reader fixtures and development adapter checks |

Run the reader's host tests on macOS:

```sh
make -C tools/hfsplus -j"$(sysctl -n hw.ncpu)" test
```

The native-reader tests use constructed images and do not prove complete hardware compatibility. The older adapter integration fixtures were written for the read-only prototype and include assumptions that no longer match the writable adapter; their failures must not be presented as a passing complete filesystem test. Host syntax checks are separate from an ARM firmware or bootloader link.

The JPEG repair was reproduced using the uploaded cover and the actual decoder source in a host harness. Twenty comparisons on conventional JPEG samples retained identical pixel CRCs, and 35 decoder runs passed AddressSanitizer in that harness. On-device playback and artwork were then confirmed on the working iPod. These results do not cover all JPEG encodings or malformed files.

## License and sources

Rockpod HFS+ retains the upstream Rockpod/Rockbox authorship and component license notices. See [COPYING](COPYING), [docs/CREDITS](docs/CREDITS), the source-file headers and the licenses within individual libraries and fonts. This project is not affiliated with Apple.

The included adwaitapod_dark_simplified and Themify 2 themes are derived from themes by [Dook](https://github.com/D0-0K), with the upstream modifications retained. Their CC-BY-SA attribution and the notices included with bundled theme assets remain applicable. Upstream Rockpod credits [ipod-gadget](https://github.com/oandrew/ipod-gadget), [rockbox-mojyack](https://github.com/mojyack/rockbox) and Apple's MFi accessory specification as references for its digital audio implementation. Retaining credits is separate from removing obsolete development notes.

Technical background:

- [Apple Technical Note TN1150: HFS Plus Volume Format](https://developer.apple.com/library/archive/technotes/tn/tn1150.html)
- [Apple File System Programming Guide: File System Details](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemDetails/FileSystemDetails.html)
- [Microsoft: File System Functionality Comparison](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison)
- [mks5lboot documentation](utils/mks5lboot/README)

The format references describe their respective filesystems. The compatibility and overlay restrictions documented here describe this source implementation and take precedence over the general filesystem capabilities.
