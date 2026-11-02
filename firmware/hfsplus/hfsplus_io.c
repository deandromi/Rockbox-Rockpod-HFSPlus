/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sector-addressed I/O for the HFS+ adapter.  Everything here works on raw
 * byte offsets and the aligned bounce buffer; no overlay or FAT policy.
 */
#include "hfsplus_io.h"
#include "hfsplus_ro.h"
#include "storage.h"
#include <string.h>

static int fork_overlaps(const struct hfs_ro_fork *a,
                         const struct hfs_ro_fork *b)
{
    for (unsigned int i = 0; i < 8; ++i) {
        uint64_t as = a->extents[i].start, ac = a->extents[i].count;
        if (!ac)
            continue;
        for (unsigned int j = 0; j < 8; ++j) {
            uint64_t bs = b->extents[j].start, bc = b->extents[j].count;
            if (bc && as < bs + bc && bs < as + ac)
                return 1;
        }
    }
    return 0;
}

static int write_inline_extents(struct hfs_state *s,
                                const struct hfs_ro_entry *entry,
                                uint64_t offset, const void *buffer,
                                size_t length)
{
    const uint8_t *in = buffer;
    for (unsigned int i = 0; i < 8 && length; ++i) {
        const struct hfs_ro_extent *e = &entry->data.extents[i];
        uint64_t bytes = (uint64_t)e->count * s->volume.block_size;
        if (offset >= bytes) {
            offset -= bytes;
            continue;
        }
        size_t chunk = length;
        if (bytes - offset < chunk)
            chunk = (size_t)(bytes - offset);
        uint64_t physical = s->volume.base +
            (uint64_t)e->start * s->volume.block_size + offset;
        if (device_write(&s->io, physical, in, chunk))
            return HFS_RO_IO;
        in += chunk;
        length -= chunk;
        offset = 0;
    }
    return length ? HFS_RO_FORMAT : HFS_RO_OK;
}

int write_existing_inline(struct hfs_state *s,
                          const struct hfs_ro_entry *entry,
                          uint64_t offset, const void *buffer,
                          size_t length)
{
    if (!entry || entry->kind != 2 || entry->unsupported ||
        offset > entry->data.size || length > entry->data.size - offset)
        return HFS_RO_ARGUMENT;
    /* Never trust a catalog fork that is not allocated or that overlaps the
     * HFS+ metadata/overlay container. */
    if (hfs_ro_fork_writable(&s->volume, &entry->data) != HFS_RO_OK)
        return HFS_RO_UNSUPPORTED;
    if (s->overlay.ready && entry != &s->overlay.container &&
        fork_overlaps(&entry->data, &s->overlay.container.data))
        return HFS_RO_FORMAT;

    uint64_t inline_blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        inline_blocks += entry->data.extents[i].count;
    if (inline_blocks != entry->data.blocks)
        return HFS_RO_UNSUPPORTED;

    return write_inline_extents(s, entry, offset, buffer, length);
}

/* Fast path for the preallocated overlay container.  Its extents, size and
 * metadata isolation were validated once at stage 2; re-reading the native
 * allocation bitmap for every 320-byte entry write only burns power. */
int write_container_inline(struct hfs_state *s, uint64_t offset,
                           const void *buffer, size_t length)
{
    const struct hfs_ro_entry *entry = &s->overlay.container;
    if (entry->kind != 2 || entry->unsupported ||
        offset > entry->data.size || length > entry->data.size - offset)
        return HFS_RO_ARGUMENT;
    return write_inline_extents(s, entry, offset, buffer, length);
}

int setup_io(struct hfs_io *io, int drive)
{
    struct storage_info info;
    storage_get_info(drive, &info);
    if (info.sector_size < 512 || info.sector_size > sizeof(io->buffer) ||
        (info.sector_size & (info.sector_size - 1)) ||
        info.num_sectors > UINT64_MAX / info.sector_size)
        return HFS_RO_UNSUPPORTED;
    io->drive = drive;
    io->sector_size = info.sector_size;
    io->size = (uint64_t)info.num_sectors * info.sector_size;
    return 0;
}

int hfsplus_signature_at(const struct hfs_io *io, uint64_t offset)
{
    uint8_t sig[2];
    if (offset > io->size || io->size - offset < 1536)
        return 0;
    if (device_read((void *)io, offset + 1024, sig, sizeof(sig)))
        return 0;
    return sig[0] == 'H' && (sig[1] == '+' || sig[1] == 'X');
}

int device_read(void *ctx, uint64_t offset, void *buffer, size_t length)
{
    struct hfs_io *io = ctx;
    uint8_t *out = buffer;
    if (offset > io->size || length > io->size - offset)
        return -1;

    /* Unaligned prefix. */
    if (length && offset % io->sector_size) {
        uint32_t within = offset % io->sector_size;
        size_t chunk = io->sector_size - within;
        if (chunk > length)
            chunk = length;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(out, io->buffer + within, chunk);
        out += chunk;
        offset += chunk;
        length -= chunk;
    }

    /* Bulk aligned middle. Keep DMA on the aligned bounce buffer. */
    unsigned int max_sectors = sizeof(io->buffer) / io->sector_size;
    while (length >= io->sector_size) {
        unsigned int sectors = (unsigned int)(length / io->sector_size);
        if (sectors > max_sectors)
            sectors = max_sectors;
        size_t bytes = (size_t)sectors * io->sector_size;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 sectors, io->buffer))
            return -1;
        memcpy(out, io->buffer, bytes);
        out += bytes;
        offset += bytes;
        length -= bytes;
    }

    /* Unaligned tail. */
    if (length) {
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(out, io->buffer, length);
    }
    return 0;
}

int device_write(void *ctx, uint64_t offset, const void *buffer,
                 size_t length)
{
    struct hfs_io *io = ctx;
    const uint8_t *in = buffer;
    if (offset > io->size || length > io->size - offset)
        return -1;

    if (length && offset % io->sector_size) {
        uint32_t within = offset % io->sector_size;
        size_t chunk = io->sector_size - within;
        if (chunk > length)
            chunk = length;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(io->buffer + within, in, chunk);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  1, io->buffer))
            return -1;
        in += chunk;
        offset += chunk;
        length -= chunk;
    }

    unsigned int max_sectors = sizeof(io->buffer) / io->sector_size;
    while (length >= io->sector_size) {
        unsigned int sectors = (unsigned int)(length / io->sector_size);
        if (sectors > max_sectors)
            sectors = max_sectors;
        size_t bytes = (size_t)sectors * io->sector_size;
        memcpy(io->buffer, in, bytes);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  sectors, io->buffer))
            return -1;
        in += bytes;
        offset += bytes;
        length -= bytes;
    }

    if (length) {
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(io->buffer, in, length);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  1, io->buffer))
            return -1;
    }
    return 0;
}
