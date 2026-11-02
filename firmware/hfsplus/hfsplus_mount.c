/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mount lifecycle and partition scan.  The bootloader uses the native-only
 * path; firmware additionally loads and validates the overlay.
 */
#include "config.h"
#include "hfsplus_rb.h"
#include "hfsplus_state.h"
#include "hfsplus_diag.h"
#include "hfsplus_io.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_overlay_load.h"
#include "hfsplus_index_cache.h"
#include "hfsplus_partition.h"
#include "storage.h"
#include "mutex.h"
#include "fs_attr.h"
#include <limits.h>
#include <string.h>

static struct hfs_io scan_io;
static int scan_had_hfs;
static int last_error;
static int last_stage;
static uint64_t usb_hfs_offset;
static uint32_t usb_hfs_unit;

void hfsplus_init(void)
{
    last_error = 0;
    last_stage = 0;
    usb_hfs_offset = 0;
    usb_hfs_unit = 0;
    for (unsigned int i = 0; i < HFS_NUM_VOLUMES; ++i) {
        hfs_ro_unmount(&hfs_states[i].volume);
#ifdef BOOTLOADER
        /* BSS is already zero and the bootloader never loads an overlay.
         * Avoid rewriting the 708 KiB overlay member on every init. */
        hfs_states[i].overlay.ready = 0;
#else
        memset(&hfs_states[i].overlay, 0, sizeof(hfs_states[i].overlay));
#endif
        memset(&hfs_states[i].diag, 0, sizeof(hfs_states[i].diag));
        hfs_states[i].diag.ovl_slot = -1;
        hfs_states[i].diag.op_active = -1;
        hfs_states[i].force_readonly = 0;
        mutex_init(&hfs_states[i].mutex);
    }
}

bool hfsplus_mounted(int volume)
{
    return (unsigned int)volume < HFS_NUM_VOLUMES && hfs_states[volume].volume.mounted;
}

/* Present the HFS+ data partition as Apple_HFS in the USB MBR view.
 *
 * Some iPods were formatted over an existing FAT32 partition, so the raw MBR
 * still says 0x0B/0x0C even though the partition now contains HFS+.  Apple
 * Disk Mode synthesizes an Apple_HFS MBR; a plain whole-disk USB export does
 * not, and macOS then tries FAT and reports the disk as unreadable.  Patch
 * only the in-memory sector that is sent to the host: native MBR bytes are
 * never modified. */
void hfsplus_usb_fix_mbr(unsigned char *mbr, unsigned int unit)
{
    if (!mbr || !unit || unit < 512 || (unit & (unit - 1)))
        return;

    uint64_t base = usb_hfs_offset;
    if (!base && hfs_states[0].volume.mounted)
        base = hfs_states[0].volume.base;
    if (!base)
        return;

    int fallback = -1;
    uint32_t fallback_blocks = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        unsigned char *p = mbr + 446 + i * 16;
        if (p[4] != 0x0b && p[4] != 0x0c)
            continue;
        uint32_t start = (uint32_t)p[8] | (uint32_t)p[9] << 8 |
                         (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
        uint32_t blocks = (uint32_t)p[12] | (uint32_t)p[13] << 8 |
                          (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
        if (!start || !blocks)
            continue;
        if ((uint64_t)start * unit == base) {
            p[4] = 0xaf;
            return;
        }
        if (blocks > fallback_blocks) {
            fallback = (int)i;
            fallback_blocks = blocks;
        }
    }

    /* The USB block unit can differ from the MBR unit during a mode change.
     * In that case the mounted HFS+ volume is still the largest FAT-typed
     * entry, so present that one as Apple_HFS. */
    if (fallback >= 0)
        mbr[446 + (unsigned int)fallback * 16 + 4] = 0xaf;
}

bool hfsplus_had_hfs_candidate(void)
{
    return scan_had_hfs != 0;
}

int hfsplus_last_error(void)
{
    return last_error;
}

int hfsplus_last_stage(void)
{
    return last_stage;
}

int hfsplus_partitions(int drive, struct partinfo *parts, int capacity,
                       int *multiplier)
{
    struct hfs_partition found[MAX_PARTITIONS_PER_DRIVE];
    last_error = 0;
    last_stage = 0;
    scan_had_hfs = 0;
    memset(parts, 0, sizeof(*parts) * capacity);
    int rc = setup_io(&scan_io, drive);
    if (rc) {
        last_error = rc;
        last_stage = 1;
        return rc;
    }
    /* iPod Classic / iFlash partition geometry:
     *
     * The on-disk MBR stores partition start/size values in the iPod's
     * 4096-byte virtual-sector units even when the ATA layer reports 512-byte
     * logical sectors.  Parse the Apple_HFS entry directly from the MBR and
     * convert its LBA values using 4096 bytes.  hfsplus_mount() still performs
     * all HFS+ signature and metadata validation. */
    uint8_t mbr[512];
    rc = HFS_RO_UNSUPPORTED;
    if (!device_read(&scan_io, 0, mbr, sizeof(mbr)) &&
        mbr[510] == 0x55 && mbr[511] == 0xaa) {
        for (unsigned int i = 0; i < 4; ++i) {
            const uint8_t *p = mbr + 446 + i * 16;
            if (p[4] != 0xaf)
                continue;
            uint64_t begin = (uint32_t)p[8] | (uint32_t)p[9] << 8 |
                             (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
            uint64_t blocks = (uint32_t)p[12] | (uint32_t)p[13] << 8 |
                              (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
            uint64_t offset = begin * 4096u;
            uint64_t length = blocks * 4096u;
            if (!begin || !blocks || offset >= scan_io.size ||
                length > scan_io.size - offset)
                continue;
            /* Do not accept the 4096-byte interpretation merely because an
             * Apple_HFS type byte exists.  A 512-byte-unit MBR can otherwise
             * hide the real partition. */
            if (!hfsplus_signature_at(&scan_io, offset))
                continue;
            found[0].offset = offset;
            found[0].length = length;
            found[0].unit = 4096;
            found[0].type = 0xaf;
            rc = 1;
            break;
        }
    }

    /* Target policy fallback: Disk Mode can hide the USB-written MBR from
     * the ATA view.  Generic builds compile this out. */
    if (rc != 1) {
        rc = HFS_RO_UNSUPPORTED;
#ifdef HFS_FALLBACK_OFFSET
        const uint64_t base = HFS_FALLBACK_OFFSET;
        uint8_t sig[2];
        if (scan_io.size > base + 1536 &&
            !device_read(&scan_io, base + 1024, sig, sizeof(sig)) &&
            sig[0] == 'H' && sig[1] == '+') {
            found[0].offset = base;
            found[0].length = scan_io.size - base;
            found[0].unit = 4096;
            found[0].type = 0xaf;
            rc = 1;
        }
#endif
        if (rc != 1)
            rc = hfs_partition_scan(device_read, &scan_io, scan_io.size,
                                    scan_io.sector_size, found,
                                    MAX_PARTITIONS_PER_DRIVE);
    }
    if (rc < 0) {
        last_error = rc;
        last_stage = 1;
        return rc;
    }
    if (rc > capacity)
        return HFS_RO_BUFFER;
    *multiplier = 1;
    for (int i = 0; i < rc; ++i) {
        if (found[i].type == 0xaf)
            scan_had_hfs = 1;
        parts[i].start = found[i].offset / scan_io.sector_size;
        parts[i].size = found[i].length / scan_io.sector_size;
        parts[i].type = found[i].type;
        int mult = found[i].unit / scan_io.sector_size;
        if (i && mult != *multiplier)
            return HFS_RO_UNSUPPORTED; /* mixed units cannot be represented */
        *multiplier = mult;
    }
    if (scan_had_hfs) {
        uint64_t largest = 0;
        for (int i = 0; i < rc; ++i) {
            if (found[i].length > largest) {
                largest = found[i].length;
                usb_hfs_offset = found[i].offset;
                usb_hfs_unit = found[i].unit;
            }
        }
    }
    return rc;
}

static int hfsplus_mount_indexes(struct hfs_state *s)
{
#ifndef BOOTLOADER
    /* A v3 overlay may carry a validated directory/CNID index cache.  The
     * bootloader never builds or uses indexes. */
    int rc = hfs_index_cache_load(s);
    s->diag.index_cache = rc == HFS_RO_OK;
    if (!rc)
        return HFS_RO_OK;
    rc = hfs_ro_enable_fast_index(&s->volume, s->catalog_cache,
                                  sizeof(s->catalog_cache));
    if (!rc)
        (void)hfs_index_cache_save(s);
    return rc;
#else
    (void)s;
    return HFS_RO_OK;
#endif
}

static void hfsplus_mount_overlay(struct hfs_state *s)
{
#ifdef BOOTLOADER
    /* Native-only: never publish or trust the overlay.  The member is zero
     * BSS and is not cleared again here; only the readiness gate matters. */
    s->overlay.ready = 0;
    s->force_readonly = 1;
#else
    int rc = overlay_load(s);
    if (rc) {
        /* A missing or invalid container must not silently fall back to
         * writing the native HFS+ catalog.  Keep the volume readable for
         * diagnostics/recovery, but disable every write path. */
        memset(&s->overlay, 0, sizeof(s->overlay));
        s->force_readonly = 1;
    }
#endif
}

int hfsplus_mount(int volume, int drive, sector_t start, sector_t count)
{
    /* Let the FAT fallback handle volumes beyond this target's HFS+ limit. */
    if ((unsigned int)volume >= HFS_NUM_VOLUMES)
        return HFSPLUS_NOT_HFS;
    if (hfsplus_mounted(volume))
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[volume];
    last_error = 0;
    last_stage = 0;
    s->diag.last_error = 0;
    s->diag.last_stage = 0;
    s->diag.ovl_error = 0;
    s->diag.ovl_stage = 0;
    s->diag.ovl_slot = -1;
    s->diag.ovl_reason = (int)HFS_OVL_REASON_NONE;
    s->diag.index_cache = 0;
    s->diag.flush_ops = 0;
    memset(s->diag.op_count, 0, sizeof(s->diag.op_count));
    memset(s->diag.flush_op_count, 0, sizeof(s->diag.flush_op_count));
    s->diag.op_active = -1;
    /* A previous volume may have populated the path cache. */
    s->name_cache_gen++;
    s->force_readonly = 0;
    int rc = setup_io(&s->io, drive);
    if (rc) {
        last_error = rc;
        last_stage = 2;
        s->diag.last_error = rc;
        s->diag.last_stage = 2;
        return rc;
    }
    if (!count || start > s->io.size / s->io.sector_size ||
        count > s->io.size / s->io.sector_size - start) {
        last_error = HFS_RO_FORMAT;
        last_stage = 2;
        s->diag.last_error = HFS_RO_FORMAT;
        s->diag.last_stage = 2;
        return HFS_RO_FORMAT;
    }
    uint64_t base = (uint64_t)start * s->io.sector_size;
    uint64_t length = (uint64_t)count * s->io.sector_size;
    uint8_t sig[2];
    if (length < 1536 || device_read(&s->io, base + 1024, sig, sizeof(sig))) {
        last_error = HFS_RO_IO;
        last_stage = 2;
        s->diag.last_error = HFS_RO_IO;
        s->diag.last_stage = 2;
        return HFSPLUS_NOT_HFS;
    }
    if (sig[0] != 'H' || sig[1] != '+') {
        last_error = sig[1] == 'X' ? HFS_RO_UNSUPPORTED : HFS_RO_FORMAT;
        last_stage = 2;
        s->diag.last_error = last_error;
        s->diag.last_stage = 2;
        return HFSPLUS_NOT_HFS;
    }
    mutex_lock(&s->mutex);
    rc = hfs_ro_mount(&s->volume, device_read, &s->io, base, length,
                      s->scratch, sizeof(s->scratch));
    if (!rc)
        hfsplus_mount_overlay(s);
    if (!rc)
        rc = hfsplus_mount_indexes(s);
    if (rc) {
        /* Never leave a partially initialized volume published as mounted. */
#ifndef BOOTLOADER
        memset(&s->overlay, 0, sizeof(s->overlay));
#else
        s->overlay.ready = 0;
#endif
        s->force_readonly = 0;
        hfs_ro_unmount(&s->volume);
    }
    mutex_unlock(&s->mutex);
    if (rc) {
        last_error = rc;
        last_stage = 2;
        s->diag.last_error = rc;
        s->diag.last_stage = 2;
    }
    return rc;
}

void hfsplus_unmount(int volume)
{
    if ((unsigned int)volume < HFS_NUM_VOLUMES) {
        struct hfs_state *s = &hfs_states[volume];
#ifndef BOOTLOADER
        if (s->overlay.ready && s->overlay.header_dirty) {
            mutex_lock(&s->mutex);
            int rc = overlay_sync_header(s);
            if (!rc)
                s->overlay.header_dirty = 0;
            else {
                s->diag.ovl_error = rc;
                s->diag.ovl_stage = 10;
            }
            mutex_unlock(&s->mutex);
        }
#endif
#ifndef BOOTLOADER
        memset(&s->overlay, 0, sizeof(s->overlay));
#else
        s->overlay.ready = 0;
#endif
        s->name_cache_gen++;
        s->force_readonly = 0;
        hfs_ro_unmount(&s->volume);
    }
}
