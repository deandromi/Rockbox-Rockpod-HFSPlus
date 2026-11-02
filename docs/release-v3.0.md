# Rockpod HFS+ V3.0 — v3 overlay, power and adapter decomposition

Release tag: `v3.0.0`
Public version string: `3.0.0-v13.0`
Status: stable firmware release. Device boot and overlay READY confirmed on the
test iPod; the formal power-loss/power-measurement matrix remains tracked in
issue #77.

## V3.0 REQUIRES A NEW v3 OVERLAY

**This is not optional.** V3.0 does not migrate v1/v2 overlays.

- A V3.0 installation must run on a **new v3 `/.rockpod-rw`** container.
- Existing v1/v2 containers are backup/recovery material only. The firmware can
  still read them for recovery, but they are **not a supported V3.0
  configuration** and do not get the v3 index cache or high-water reads.
- Back up the old overlay, move it off the iPod, and create a new one. Settings,
  playlists, tagcache data and generated artwork stored only in the old overlay
  are not imported until a migration tool exists.
- If you already created a v3 overlay during the V3.0 beta, keep it and verify
  **Debug → Disk Info → Overlay format: v3**.

Install steps are in `README.md` §2. The short version, in Apple Disk Mode:

```sh
backup="$HOME/rockpod-overlay-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup"
if [ -e "/Volumes/IPOD/.rockpod-rw" ]; then
    ditto "/Volumes/IPOD/.rockpod-rw" "$backup/.rockpod-rw"
    mv "/Volumes/IPOD/.rockpod-rw" "$backup/.rockpod-rw.removed"
fi
mkfile 1g "/Volumes/IPOD/.rockpod-rw"
sync
```

Then install `rockbox.zip` and the matching bootloader. On first boot the
firmware initializes the new container as v3. Verify:

```text
HFS+ overlay: READY
Overlay format: v3
Overlay stage: 9
Overlay error: 0
Index cache: MISS   (first boot, then HIT)
```

If Disk Info reports `Overlay format: v2 (V3.0 requires new overlay)`, the old
container is still in place; V3.0 is not running the required overlay.

## V3.0 feature set

### v3 overlay format (`RPOVL13`)

- Resumable `INIT` marker; power loss during first initialization no longer
  leaves an unrecoverable container.
- Fresh-init detection scans every bitmap, entry-table and cache block.
- High-water entry bound avoids reading all 2,048 slots.
- 256 KiB reserved directory/CNID index cache keyed by native volume generation.
- Extended header checksum covers the v3 fields.
- v1/v2 containers remain readable for recovery, but are not migrated.

### Adapter decomposition

The 2,797-line `hfsplus_rb.c` is gone. The adapter is split into I/O, format,
allocation, store, validator, loader, namespace, FAT, index-cache, mount, state,
diagnostics and time modules.

### Power and efficiency

- Container writes no longer revalidate the immutable container fork or reread
  the native allocation bitmap on every write.
- Persistent v3 index cache avoids the full catalog scan on ordinary boots.
- High-water bound cuts entry-table reads.
- Removed a redundant entry sync after successful realloc.
- Debug → Disk Info reports overlay format, index-cache HIT/MISS and live ATA
  read/write/flush counters.
- The v3 overlay is required for these benefits.

### Bug fixes

- Disk Info no longer wraps at 32 lines; the list capacity is 64 and
  `simplelist_addline()`/`setline()` are bounded. This fixes the screen that
  showed only “Fixed device / Removeable media”.
- Overlay timestamps no longer mix Unix and HFS epochs.
- Duplicate overlay IDs are validated independently of legacy shadowing.
- v1 backup probing works with a corrupt primary magic.
- Unmount reports header-sync failure.
- Native readdir uses a bounded name copy.

## Verification

- 20 reader tests, 7 partition tests and 30 host adapter tests pass.
- ASan/UBSan passes on macOS.
- macOS host integration mode passes.
- Firmware, Thumb bootloader, `rockbox.zip`, DFU image, `mks5lboot` and
  `database.ipod6g` build.
- The V3.0 firmware booted on the test iPod with overlay READY. The full
  power-loss and power-measurement matrix is still issue #77.

## Assets and SHA-256

```text
fae14c90e8d667731a981093553f09cfd654f834902ecc801f511d493828938c  rockbox.ipod
5dbcabd79eb3e8fe4e0b3be6b62a70bdd89c25979bcce4ea5f0a3a5b5887ee6c  rockbox.zip
78cf341f97a0ec828a8627926bd348f67275f19b9d603682c32cdaf80891dc7e  bootloader-ipod6g.ipod
ecdd7fabb34410ab3a112647bc173c1a1621b6c038504825dfc0a740c0dae7d2  bootloader-ipod6g.dfu
25e9b69874d51f9781468959b4d79164e8c9ee261e3ea49a85e1795986f5523b  mks5lboot
8028ee85132c0905cb35ea494c88bbefcfe2d1a3d870aedc3b58c1be0274c0f0  database.ipod6g
```

## Known limits

- V3.0 requires a new v3 overlay; no v2→v3 migration tool is included.
- Single HFS+ volume per `ipod6g` build; HFSX/APFS are not supported.
- Clean HFS+ only; no journal replay.
- Eight inline extents; extents-overflow trees fail closed.
- Exact-byte names; no Unicode normalization/case folding.
