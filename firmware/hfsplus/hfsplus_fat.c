/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * FAT/VFS adapter boundary: capability queries, error mapping, close/truncate,
 * read/write and direntry formatting.  No overlay byte layout lives here.
 */
#include "hfsplus_rb.h"
#include "hfsplus_state.h"
#include "hfsplus_fat.h"
#include "hfsplus_io.h"
#include "hfsplus_namespace.h"
#include "hfsplus_overlay_alloc.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_overlay_store.h"
#include "hfsplus_time.h"
#include "hfsplus_ro.h"
#include "fs_attr.h"
#include "string-extra.h"
#include <string.h>
#include <limits.h>

bool fat_is_readonly(const struct fat_file *file)
{
    /* Historical name retained because the FAT compatibility layer uses it to
     * identify HFS+ objects.  v11 may still be writable through the overlay. */
    return file && hfsplus_mounted(IF_MV_VOL(file->volume));
}

bool hfsplus_writable(const struct fat_file *file)
{
    if (!hfs_valid_file(file))
        return false;
    return hfsplus_mounted(IF_MV_VOL(file->volume)) &&
           !hfs_states[IF_MV_VOL(file->volume)].force_readonly &&
           hfs_states[IF_MV_VOL(file->volume)].overlay.ready;
}

bool hfsplus_can_write_existing(const struct fat_file *file)
{
    if (!hfs_valid_file(file) || !hfsplus_mounted(IF_MV_VOL(file->volume)) ||
        file->hfs_kind != 2)
        return false;
    if (hfs_states[IF_MV_VOL(file->volume)].force_readonly)
        return false;
    if (hfs_states[IF_MV_VOL(file->volume)].overlay.ready)
        return true;
    if (hfs_ro_fork_writable(&hfs_states[IF_MV_VOL(file->volume)].volume,
                             &file->hfs_data) != HFS_RO_OK)
        return false;
    uint64_t blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        blocks += file->hfs_data.extents[i].count;
    return blocks == file->hfs_data.blocks;
}

int hfsplus_closewrite(struct fat_filestr *filestr, uint32_t size,
                       struct fat_direntry *fatentp)
{
    if (!hfs_valid_stream(filestr) || filestr->fatfilep->hfs_kind != 2)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(filestr->fatfilep->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    struct fat_file *file = filestr->fatfilep;
    if (file->hfs_overlay) {
        unsigned int idx = file->hfs_overlay_index;
        if (idx >= HFS_OVL_MAX_ENTRIES ||
            !(s->overlay.entries[idx].flags & HFS_OVL_USED) ||
            s->overlay.entries[idx].id != (uint32_t)file->firstcluster) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        struct hfs_overlay_entry *e = &s->overlay.entries[idx];
        if ((e->flags & HFS_OVL_BASE) && size > e->size) {
            rc = overlay_materialize_base(s, e, &file->hfs_data);
            if (rc)
                goto out;
        }
        if (!(e->flags & HFS_OVL_BASE)) {
            rc = overlay_ensure_capacity(s, e, size, NULL);
            if (rc)
                goto out;
            rc = overlay_shrink(s, e, size);
            if (rc)
                goto out;
        }
        /* A BASE entry still references the immutable native data fork.  Do
         * not allocate a backing run merely because a write handle was closed
         * without changing the size. */
        e->size = size;
        e->mtime = hfs_now();
        rc = overlay_sync_entry(s, idx);
        if (!rc) {
            file->hfs_data.size = size;
            if (fatentp)
                fill_overlay_fatent(e, fatentp);
        }
    } else if ((uint64_t)size != file->hfs_data.size) {
        /* Without the overlay only v10-style in-place, same-size writes are
         * safe. */
        rc = HFS_RO_UNSUPPORTED;
    } else if (fatentp) {
        struct hfs_ro_entry base;
        int frc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster,
                                 &base);
        if (frc) {
            rc = frc;
        } else {
            fat_empty_fat_direntry(fatentp);
            strlcpy((char *)fatentp->name, base.name,
                    sizeof(fatentp->name));
            fatentp->attr = ATTR_ARCHIVE;
            fatentp->filesize = (uint32_t)base.data.size;
            fatentp->firstcluster = (int32_t)base.id;
            set_fat_time(fatentp, hfs_time_to_unix(base.modified ?
                                                   base.modified :
                                                   base.created));
        }
    }
out:
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

bool hfsplus_overlay_ready(int volume)
{
    return (unsigned int)volume < HFS_NUM_VOLUMES && hfs_states[volume].overlay.ready;
}

int hfsplus_sector_size(int volume)
{
    return hfs_valid_volume(volume) ? hfs_states[volume].io.sector_size : 0;
}

bool hfsplus_size(int volume, sector_t *size, sector_t *free)
{
    if (!hfsplus_mounted(volume))
        return false;
    struct hfs_state *s = &hfs_states[volume];
    const struct hfs_ro_volume *v = &s->volume;
    if (size)
        *size = (uint64_t)v->blocks * v->block_size / 1024;
    if (free) {
        uint64_t native_free = (uint64_t)v->free_blocks * v->block_size / 1024;
        if (s->overlay.ready) {
            mutex_lock(&s->mutex);
            uint64_t overlay_free = (uint64_t)overlay_free_blocks(&s->overlay) *
                                    HFS_OVL_BLOCK / 1024;
            mutex_unlock(&s->mutex);
            /* New data is allocated from the overlay, not the immutable
             * native volume.  Reporting min() understated the usable space. */
            (void)native_free;
            *free = overlay_free;
        } else {
            *free = native_free;
        }
    }
    return true;
}

int hfsplus_truncate(const struct fat_filestr *filestr)
{
    if (!hfs_valid_stream(filestr))
        return HFS_RO_ARGUMENT;
    struct fat_file *file = filestr->fatfilep;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (!file->hfs_overlay) {
        struct hfs_ro_entry base;
        rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (rc)
            goto out;
        rc = overlay_shadow_base(s, file, (uint32_t)file->dircluster, base.name);
        if (rc)
            goto out;
    }
    /* Exact byte length is committed by hfsplus_closewrite().  Deferring the
     * physical shrink also avoids throwing away the partial final sector. */
    rc = 1;
out:
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

int hfsplus_seek(struct fat_filestr *stream, unsigned long sector)
{
    if (!hfs_valid_stream(stream))
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(stream->fatfilep->volume)];
    uint64_t bytes = stream->fatfilep->hfs_data.size;
    mutex_lock(&s->mutex);
    if (stream->fatfilep->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, stream->fatfilep);
        if (e)
            bytes = e->size;
    }
    mutex_unlock(&s->mutex);
    uint64_t sectors = (bytes + s->io.sector_size - 1) / s->io.sector_size;
    if (sector > sectors)
        return FAT_SEEK_EOF;
    stream->lastsector = sector;
    stream->eof = false;
    return 0;
}

long hfsplus_readwrite(struct fat_filestr *stream, unsigned long sectors,
                       void *buffer, bool write)
{
    if (!hfs_valid_stream(stream))
        return HFS_RO_ARGUMENT;
    if (!sectors || (!write && stream->eof))
        return 0;
    struct fat_file *file = stream->fatfilep;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(file->volume)];
    if (sectors > LONG_MAX / s->io.sector_size)
        return HFS_RO_ARGUMENT;
    size_t requested = sectors * s->io.sector_size;
    uint64_t offset = (uint64_t)stream->lastsector * s->io.sector_size;

    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    size_t got = 0;

    if (write && s->overlay.ready && !file->hfs_overlay) {
        struct hfs_ro_entry base;
        rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (rc)
            goto out;
        rc = overlay_shadow_base(s, file, (uint32_t)file->dircluster, base.name);
        if (rc)
            goto out;
    }

    if (file->hfs_overlay) {
        struct hfs_overlay_entry *oe = overlay_file_entry(s, file);
        if (!oe) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        if (write) {
            if (oe->flags & HFS_OVL_BASE) {
                rc = overlay_materialize_base(s, oe, &file->hfs_data);
                if (rc)
                    goto out;
                /* Persist the new backing run before data writes. Size remains
                 * the last committed value until close/fsync. */
                rc = overlay_sync_entry(s, file->hfs_overlay_index);
                if (rc)
                    goto out;
            }
            uint64_t end = offset + requested;
            if (end < offset) {
                rc = HFS_RO_ARGUMENT;
                goto out;
            }
            uint32_t old_start = oe->data_start, old_blocks = oe->data_blocks;
            int entry_synced = 0;
            rc = overlay_ensure_capacity(s, oe, end, &entry_synced);
            if (rc)
                goto out;
            if (!entry_synced &&
                (old_start != oe->data_start || old_blocks != oe->data_blocks)) {
                rc = overlay_sync_entry(s, file->hfs_overlay_index);
                if (rc)
                    goto out;
            }
            rc = overlay_data_write(s, oe, offset, buffer, requested);
            if (rc)
                goto out;
            if (end > oe->size)
                oe->size = end;
            /* Do not rewrite the 320-byte entry for every sector. close/fsync
             * commits the logical size; allocation changes above are already
             * persisted before data is exposed. */
            file->hfs_data.size = oe->size;
            got = requested;
        } else {
            if (offset >= oe->size) {
                got = 0;
                goto success;
            }
            got = requested;
            if (oe->size - offset < got)
                got = (size_t)(oe->size - offset);
            if (oe->flags & HFS_OVL_BASE) {
                struct hfs_ro_entry base;
                memset(&base, 0, sizeof(base));
                base.kind = 2;
                base.data = file->hfs_data;
                size_t done = 0;
                rc = hfs_ro_pread(&s->volume, &base, offset, buffer, got, &done);
                if (rc || done != got) {
                    rc = rc ? rc : HFS_RO_IO;
                    goto out;
                }
            } else if (got) {
                rc = overlay_data_read(s, oe, offset, buffer, got);
                if (rc)
                    goto out;
            }
        }
    } else {
        struct hfs_ro_entry e;
        memset(&e, 0, sizeof(e));
        e.kind = file->hfs_kind;
        e.data = file->hfs_data;
        if (write) {
            /* Never write a prefix of an extending request: VFS would later
             * report a size change and hfsplus_closewrite() would fail after
             * the native data had already been modified. */
            if (!hfsplus_can_write_existing(file) ||
                offset > e.data.size ||
                requested > e.data.size - offset) {
                rc = HFS_RO_UNSUPPORTED;
                goto out;
            }
            rc = write_existing_inline(s, &e, offset, buffer, requested);
            if (rc)
                goto out;
            got = requested;
        } else {
            rc = hfs_ro_pread(&s->volume, &e, offset, buffer, requested, &got);
            if (rc)
                goto out;
        }
    }

success:
    rc = HFS_RO_OK;
out:
    mutex_unlock(&s->mutex);
    if (rc)
        return hfs_file_rc(rc);

    size_t transferred = write ? sectors :
        (got + s->io.sector_size - 1) / s->io.sector_size;
    size_t padded = transferred * s->io.sector_size;
    if (!write && padded > got)
        memset((uint8_t *)buffer + got, 0, padded - got);
    stream->lastsector += transferred;
    uint64_t logical_size = file->hfs_data.size;
    if (file->hfs_overlay) {
        mutex_lock(&s->mutex);
        struct hfs_overlay_entry *oe = overlay_file_entry(s, file);
        if (oe)
            logical_size = oe->size;
        mutex_unlock(&s->mutex);
    }
    stream->eof = offset + got >= logical_size;
    return (long)transferred;
}

unsigned int hfsplus_cluster_size(int volume)
{
    return hfs_valid_volume(volume) ? hfs_states[volume].volume.block_size : 0;
}
