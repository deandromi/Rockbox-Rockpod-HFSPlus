# iPod Classic 6G — read-only device investigation

Date: 2026-11-01
Device: Apple iPod Classic 6G, `disk4`, partition `disk4s1`
Volume: `IPOD`, 127.8 GB, HFS+ Journaled, mounted at `/Volumes/IPOD`
Firmware: `.rockbox/rockbox.ipod`, 806,540 bytes, `-261004`
Scope: read-only inspection; no firmware, bootloader, overlay, or music files were written or repaired.

## 1. Preservation

The following were copied off the device before any deeper inspection:

- `/.rockpod-rw` — 1,073,741,824 bytes, SHA-256
  `503528ddab96277047890574735f1a4b63a619d0f69328d85eef0a3228f47e5c`
- the complete `/.rockbox` tree (about 70 MB)
- root test files `db_create_probe.lua`, `HFSWRITE.TEST`, `V10WRITE.lua`, `V10WRITE.TEST`
- `diskutil` output, mount table, root listing, overlay parse report and file manifest

Durable snapshot:
`/Users/deandromink/Documents/iPod bestanden/ipod-preserve-20261101`

The working copy is `/tmp/ipod-preserve.ZXT55Y`. The overlay copy was made before unmount/remount and before `diskutil verifyVolume`.

## 2. Native HFS+ state

`diskutil verifyVolume /Volumes/IPOD` ran a full `fsck_hfs -fn -x` and reported:

```text
Checking Journaled HFS Plus volume
Checking extents overflow file
Checking catalog file
Checking multi-linked files
Checking catalog hierarchy
Checking extended attributes file
Checking volume bitmap
Checking volume information
The volume IPOD appears to be OK
File system check exit code is 0
```

The historical `/var/log/fsck_hfs.log` entries for this iPod (`rdisk4s1`) around 2026-10-03/04 also show `QUICKCHECK ONLY; FILESYSTEM CLEAN`, plus a full check on 2026-10-04 22:21 that ended `The volume IPOD appears to be OK`. No native HFS+ catalog or allocation damage was found in the current snapshot or in those logs.

Key native identities from `stat`:

| Path | inode / CNID | size | birth | mtime |
|---|---:|---:|---|---|
| `/.rockbox` | 190 | 1,292 | 2026-07-31 17:22:38 | 2026-10-04 20:16:55 |
| `/.rockbox/rockbox.ipod` | 24,112 | 806,540 | 2026-10-04 20:16:45 | 2026-10-04 20:16:45 |
| `/.rockpod-rw` | 1,592 | 1,073,741,824 | 2026-10-03 01:00:16 | 2026-10-04 18:18:56 |
| `/Music` | 2,136 | 7,922 | — | 2026-10-03 09:59 |
| `/.rockbox/rocks/demos` | 567 | 918 | — | — |
| `/.rockbox/fonts` | 609 | 646 | — | — |
| `/.rockbox/rocks/demos/pictureflow.cfg` | 20,694 | 110 | — | — |

`/.rockbox` is a normal native directory. It was not recreated recently; its birth date is 2026-07-31. It is visible to macOS with a full `ls -la`.

## 3. Overlay state

The preserved `/.rockpod-rw` was parsed with the same layout as the audit tools:

```text
magic          RPOVL11
version        1
block size     4096
entries        2048
total blocks   262144 (1 GiB)
bitmap bytes   32768
data_start     169
next_id        0x4000006a
generation     43
used entries   16
free blocks    259307 (about 1012.9 MiB)
```

The 16 used entries are all settings, playlists, font files and PictureFlow configuration. Their parent IDs are the native CNIDs of the current directories:

- `parent=190` = `/.rockbox`
- `parent=567` = `/.rockbox/rocks/demos`
- `parent=609` = `/.rockbox/fonts`

There are:

- **no root entries (`parent == 2`)**;
- **no tombstone for `.rockbox`**;
- **no `rockbox.ipod` entry or tombstone**;
- **no overlay entry or tombstone whose name contains `rockbox`**.

There is one tombstone: `pictureflow.cfg`, parent 567, base 20694, flags `USED|TOMBSTONE`. It hides the native `pictureflow.cfg`; a live BASE overlay entry with the same name provides the current settings. This is unrelated to `.rockbox`.

All 16 used entry ranges are inside the data area, fully marked used in the bitmap, and non-overlapping. No current HFS-001/HFS-002 inconsistency was found. The container is fully allocated natively (2,097,152 blocks of 512 bytes = 1 GiB), so HFS-122 is not triggered by this snapshot.

## 4. The simple non-corruption explanation: dotfiles are hidden

Rockbox's file browser defaults to **`Show Files: Supported`**. `apps/filetree.c` filters any entry whose name starts with `.` unless the filter is `SHOW_ALL`:

```c
if (*c->dirfilter != SHOW_ALL &&
    ((entry->d_name[0]=='.') || (info.attribute & ATTR_HIDDEN))) {
    continue;
}
```

The default is set in `apps/settings_list.c`:

```c
CHOICE_SETTING(0, dirfilter, LANG_FILTER, SHOW_SUPPORTED, "show files", ...)
```

The current overlay `config.cfg` contains only volume/scrollbar/brightness/QS settings; it has no `show files` override. Therefore **`.rockbox` is intentionally not shown in the Rockbox Files browser with the current settings**, even though it exists. macOS Finder likewise hides dotfiles by default. This alone can explain “one day I could not find `.rockbox`” if the user was looking in the file browser or Finder rather than booting.

There is evidence of a settings change/reset in the overlay: `config.cfg.old` (492 bytes) contains theme, font, foreground/background, iconset, WPS/SBS and backdrop settings, while the current `config.cfg` (173 bytes) contains only volume, scrollbar, brightness and quick-screen entries and no `show files` line. A settings reset or a fresh config write would restore the default `Show Files: Supported` filter and make `.rockbox` disappear from the file browser’s visible list.

This does **not** explain a boot failure or a missing boot file. In the current overlay there is no tombstone or shadow for `.rockbox` or `rockbox.ipod`, so the bootloader should find the native firmware.

## 5. Interpretation

Current evidence points away from native HFS+ corruption and away from an active overlay tombstone:

1. Native catalog/volume checks are clean.
2. `.rockbox` exists as a native directory with a long-lived CNID and is visible to macOS.
3. The overlay is structurally consistent and has no root entries, `.rockbox` tombstone, or `rockbox.ipod` shadow.
4. `.rockbox` is filtered from the Rockbox Files browser by default because it is a dotfile.
5. The current overlay entries attach to the current `.rockbox` CNID (190), so the overlay is not orphaned from the directory.

A normal Rockbox boot succeeded after the snapshot, so the current `.rockbox` and native `rockbox.ipod` are loadable. `.rockbox` is still hidden in the file browser by the default dotfile filter; setting **Show Files → All** should make it visible.

### Theme persistence

The user also reports that selecting a theme applies immediately but that after a reboot the player returns to cabbiev2. The snapshot contains evidence of the same layering problem:

- current overlay `config.cfg`: 173 bytes, only `volume`, `scrollbar`, `brightness` and quick-screen entries;
- `config.cfg.old`: 492 bytes, containing `wps: /.rockbox/wps/SNARTYNEON.wps`, `sbs`, font, iconset, colours and backdrop;
- no `config.cfg.new` in the overlay.

Rockbox's normal settings path writes changed settings to `config.cfg.new` on the storage-idle callback, then promotes `.new` to `config.cfg` at the next boot. On HFS+ that create/close/rename path runs through the overlay (`hfsplus_create_file`, `hfsplus_closewrite`, `hfsplus_rename`), which has the HFS-002/025/091 failure modes. The exact lost stage is still open; it may also be a reset before the idle flush or a settings reset after a firmware/config change. This is logged as **HFS-133**.

The historical disappearance cannot be proven from the current snapshot. Possible historical causes remain:

- a tombstone or shadow that existed earlier and was later removed or rebuilt;
- a settings reset that changed `show files` from All to the default Supported;
- a transient reader/index failure (for example HFS-126) at an earlier mount;
- an earlier overlay state that was fresh-initialized after header damage (HFS-040/HFS-117).

The overlay header has `generation=43`; it has no history or checksum of previous generations, so those earlier states are not recoverable from the container.

## 6. Raw catalog check — blocked by macOS

A read-only dump of the native catalog was attempted to run `hfs_ro` against the real device image. The admin prompt was accepted, but even as root the raw device open failed with:

```text
PermissionError: [Errno 1] Operation not permitted: '/dev/rdisk4s1'
```

The same restriction applies to `dd` and to `fsck_hfs` when launched directly. macOS restricts raw block-device access to entitled tools; `diskutil verifyVolume` and its `fsck_hfs -fn -x` helper are allowed, but Python/dd are not. Retrying the password does not change this. A raw catalog dump would require Full Disk Access for the invoking terminal/Python or a signed entitled tool; it is not necessary for the current verification because the native fsck already validates the catalog hierarchy.

## 7. Safe next steps

1. **Do not repair, format, delete or recreate** `/.rockpod-rw` or `/.rockbox`. The snapshot above is the rollback copy.
2. To see `.rockbox` in Rockbox: **Settings → General Settings → File View → Show Files → All**. In Finder use `Cmd+Shift+.`.
3. **Theme persistence test (HFS-133):** select a theme, wait at least 15 seconds or until disk activity stops, check that `/.rockbox/config.cfg.new` exists and contains `wps`/`sbs`/font lines, then power off through the Rockbox menu (not a hard reset). Reboot and check whether the theme survives. If `.new` never appears, the settings idle flush is the problem; if it appears but is not promoted, the overlay rename is the problem. Preserve `config.cfg`, `config.cfg.new` and `config.cfg.old` before and after.
4. If the problem recurs, capture:
   - `/.rockpod-rw` before and after;
   - the bootloader error screen;
   - `diskutil verifyVolume`;
   - `log show`/`fsck_hfs.log` around the event;
   - whether `Show Files` was All or Supported.
5. Do not install firmware/bootloader changes until the S0/S1 findings in `BUGFIXES.md` are fixed and a supervised boot test passes. Theme/settings fixes (HFS-133) must be tested on the device, not only in the host harness.
