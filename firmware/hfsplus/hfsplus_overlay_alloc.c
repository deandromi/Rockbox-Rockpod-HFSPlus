/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Overlay allocation: bitmap ownership, run allocation/free, growth policy and
 * materialization.  Persist-before-free ordering lives here; callers must not
 * free a run before the new entry state is durable.
 */
#include "hfsplus_overlay_alloc.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_ro.h"
#include <string.h>

static int overlay_range_owned(const struct hfs_overlay *o,
                               uint32_t start, uint32_t blocks,
                               const struct hfs_overlay_entry *except);

/* A 16 KiB copy buffer reduces a large realloc or materialization from one
 * 4 KiB ATA transaction per block to one per four blocks.  Firmware uses the
 * dedicated aligned buffer; the bootloader never reaches this code and keeps
 * s->scratch as the fallback so the state layout stays small. */
static uint8_t *overlay_copy_buffer(struct hfs_state *s)
{
#ifdef BOOTLOADER
    return s->scratch;
#else
    return s->copy_buffer;
#endif
}

void overlay_set_bit(struct hfs_overlay *o, uint32_t block, int used)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (used)
        o->bitmap[block >> 3] |= mask;
    else
        o->bitmap[block >> 3] &= (uint8_t)~mask;
}

uint32_t overlay_free_blocks(const struct hfs_overlay *o)
{
    return o->ready ? o->free_blocks : 0;
}

static int overlay_alloc_run(struct hfs_state *s, uint32_t blocks,
                             uint32_t *start)
{
    struct hfs_overlay *o = &s->overlay;
    if (!blocks || blocks > o->free_blocks ||
        blocks > o->total_blocks - o->data_start)
        return HFS_RO_BUFFER;

    uint32_t begin = o->alloc_hint;
    if (begin < o->data_start || begin >= o->total_blocks)
        begin = o->data_start;

    for (unsigned int pass = 0; pass < 2; ++pass) {
        uint32_t from = pass ? o->data_start : begin;
        uint32_t to = pass ? begin : o->total_blocks;
        uint32_t run = 0;
        for (uint32_t b = from; b < to; ++b) {
            if (!overlay_bit(o, b)) {
                if (++run == blocks) {
                    uint32_t first = b + 1u - run;
                    if (overlay_range_owned(o, first, blocks, NULL)) {
                        run = 0;
                        continue;
                    }
                    for (uint32_t j = 0; j < blocks; ++j)
                        overlay_set_bit(o, first + j, 1);
                    int rc = overlay_sync_bitmap_range(s, first, blocks);
                    if (rc) {
                        for (uint32_t j = 0; j < blocks; ++j)
                            overlay_set_bit(o, first + j, 0);
                        return rc;
                    }
                    o->free_blocks -= blocks;
                    o->alloc_hint = first + blocks;
                    if (o->alloc_hint >= o->total_blocks)
                        o->alloc_hint = o->data_start;
                    *start = first;
                    return HFS_RO_OK;
                }
            } else {
                run = 0;
            }
        }
    }
    return HFS_RO_BUFFER;
}

static int overlay_range_owned(const struct hfs_overlay *o,
                               uint32_t start, uint32_t blocks,
                               const struct hfs_overlay_entry *except)
{
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        const struct hfs_overlay_entry *e = &o->entries[i];
        if (e == except || !(e->flags & HFS_OVL_USED) ||
            (e->flags & HFS_OVL_TOMBSTONE) || !e->data_blocks)
            continue;
        if (start < e->data_start + e->data_blocks &&
            e->data_start < start + blocks)
            return 1;
    }
    return 0;
}

static int overlay_entry_owns(const struct hfs_overlay_entry *e,
                              uint32_t start, uint32_t blocks)
{
    return e && (e->flags & HFS_OVL_USED) &&
           !(e->flags & HFS_OVL_TOMBSTONE) && blocks &&
           start >= e->data_start &&
           start - e->data_start <= e->data_blocks &&
           blocks <= e->data_blocks - (start - e->data_start);
}

/* Free a run only when the entry (or a just-allocated rollback token) owns
 * every block.  The bitmap alone is not enough: a corrupt entry must never be
 * able to release another file's live blocks. */
int overlay_free_run(struct hfs_state *s,
                     struct hfs_overlay_entry *owner,
                     uint32_t start, uint32_t blocks, int require_owner)
{
    struct hfs_overlay *o = &s->overlay;
    if (!blocks)
        return HFS_RO_OK;
    if (start < o->data_start || start >= o->total_blocks ||
        blocks > o->total_blocks - start)
        return HFS_RO_FORMAT;
    if (require_owner && !overlay_entry_owns(owner, start, blocks))
        return HFS_RO_FORMAT;
    if (overlay_range_owned(o, start, blocks, require_owner ? owner : NULL))
        return HFS_RO_FORMAT;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (!overlay_bit(o, start + i))
            return HFS_RO_FORMAT;
    }
    for (uint32_t i = 0; i < blocks; ++i)
        overlay_set_bit(o, start + i, 0);
    int rc = overlay_sync_bitmap_range(s, start, blocks);
    if (rc) {
        for (uint32_t i = 0; i < blocks; ++i)
            overlay_set_bit(o, start + i, 1);
        return rc;
    }
    o->free_blocks += blocks;
    if (start < o->alloc_hint)
        o->alloc_hint = start;
    return HFS_RO_OK;
}

static int overlay_try_extend(struct hfs_state *s, struct hfs_overlay_entry *e,
                              uint32_t needed)
{
    struct hfs_overlay *o = &s->overlay;
    if (needed <= e->data_blocks)
        return HFS_RO_OK;
    if (!e->data_blocks)
        return HFS_RO_BUFFER;
    uint32_t extra = needed - e->data_blocks;
    uint32_t end = e->data_start + e->data_blocks;
    if (extra > o->free_blocks || end > o->total_blocks ||
        extra > o->total_blocks - end)
        return HFS_RO_BUFFER;
    for (uint32_t i = 0; i < extra; ++i)
        if (overlay_bit(o, end + i))
            return HFS_RO_BUFFER;
    if (overlay_range_owned(o, end, extra, e))
        return HFS_RO_BUFFER;
    for (uint32_t i = 0; i < extra; ++i)
        overlay_set_bit(o, end + i, 1);
    int rc = overlay_sync_bitmap_range(s, end, extra);
    if (rc) {
        for (uint32_t i = 0; i < extra; ++i)
            overlay_set_bit(o, end + i, 0);
        return rc;
    }
    o->free_blocks -= extra;
    e->data_blocks = needed;
    o->alloc_hint = end + extra;
    if (o->alloc_hint >= o->total_blocks)
        o->alloc_hint = o->data_start;
    return HFS_RO_OK;
}

static int overlay_realloc(struct hfs_state *s, struct hfs_overlay_entry *e,
                           uint32_t needed, int *entry_synced)
{
    if (entry_synced)
        *entry_synced = 0;
    if (needed <= e->data_blocks)
        return HFS_RO_OK;
    if (e->data_blocks) {
        int erc = overlay_try_extend(s, e, needed);
        if (!erc)
            return HFS_RO_OK;
        if (erc != HFS_RO_BUFFER)
            return erc;
    }

    uint32_t oldstart = e->data_start, oldblocks = e->data_blocks;
    if (e->size > (uint64_t)oldblocks * HFS_OVL_BLOCK)
        return HFS_RO_FORMAT;

    uint32_t newstart;
    int rc = overlay_alloc_run(s, needed, &newstart);
    if (rc)
        return rc;
    uint8_t *copy = overlay_copy_buffer(s);
    uint64_t copied = 0;
    while (copied < e->size) {
        size_t chunk = HFS_IO_BOUNCE_SIZE;
        if (e->size - copied < chunk)
            chunk = (size_t)(e->size - copied);
        rc = overlay_container_read(s,
            (uint64_t)oldstart * HFS_OVL_BLOCK + copied, copy, chunk);
        if (rc)
            break;
        rc = overlay_container_write(s,
            (uint64_t)newstart * HFS_OVL_BLOCK + copied, copy, chunk);
        if (rc)
            break;
        copied += chunk;
    }
    if (rc) {
        overlay_free_run(s, NULL, newstart, needed, 0);
        return rc;
    }
    /* Publish the new backing run before releasing the old one.  If the entry
     * write fails, restore the old run and free the new allocation. */
    e->data_start = newstart;
    e->data_blocks = needed;
    rc = overlay_sync_entry(s, (unsigned int)(e - s->overlay.entries));
    if (!rc && entry_synced)
        *entry_synced = 1;
    if (rc) {
        e->data_start = oldstart;
        e->data_blocks = oldblocks;
        overlay_free_run(s, NULL, newstart, needed, 0);
        return rc;
    }
    if (oldblocks) {
        e->data_start = oldstart;
        e->data_blocks = oldblocks;
        rc = overlay_free_run(s, e, oldstart, oldblocks, 1);
        e->data_start = newstart;
        e->data_blocks = needed;
        return rc;
    }
    return HFS_RO_OK;
}

static uint32_t overlay_growth_target(const struct hfs_overlay *o,
                                      uint32_t current, uint32_t needed)
{
    if (needed <= current)
        return current;
    uint32_t target = current;
    if (!target)
        target = HFS_OVL_GROW_MIN_BLOCKS;
    while (target < needed) {
        uint32_t grow = target / 2u;
        if (grow < HFS_OVL_GROW_MIN_BLOCKS)
            grow = HFS_OVL_GROW_MIN_BLOCKS;
        if (target > o->total_blocks - grow) {
            target = needed;
            break;
        }
        target += grow;
    }
    if (target < needed)
        target = needed;
    return target;
}

int overlay_materialize_base(struct hfs_state *s,
                             struct hfs_overlay_entry *e,
                             const struct hfs_ro_fork *fork)
{
    if (!(e->flags & HFS_OVL_BASE))
        return HFS_RO_OK;
    /* A previous close on a BASE shadow may have allocated an unused run;
     * release it before taking ownership of the native data. */
    if (e->data_blocks) {
        uint32_t start = e->data_start, blocks = e->data_blocks;
        int rc = overlay_free_run(s, e, start, blocks, 1);
        if (rc)
            return rc;
        e->data_start = 0;
        e->data_blocks = 0;
    }
    uint32_t needed = (uint32_t)((e->size + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    uint32_t capacity = overlay_growth_target(&s->overlay, 0, needed);
    if (needed) {
        uint32_t start;
        int rc = overlay_alloc_run(s, capacity, &start);
        if (rc)
            return rc;
        struct hfs_ro_entry src;
        memset(&src, 0, sizeof(src));
        src.kind = 2;
        src.data = *fork;
        uint8_t *copy = overlay_copy_buffer(s);
        uint64_t copied = 0;
        while (copied < e->size) {
            size_t chunk = HFS_IO_BOUNCE_SIZE, done = 0;
            if (e->size - copied < chunk)
                chunk = (size_t)(e->size - copied);
            rc = hfs_ro_pread(&s->volume, &src, copied, copy, chunk, &done);
            if (rc || done != chunk) {
                overlay_free_run(s, NULL, start, capacity, 0);
                return rc ? rc : HFS_RO_IO;
            }
            rc = overlay_container_write(s,
                (uint64_t)start * HFS_OVL_BLOCK + copied, copy, chunk);
            if (rc) {
                overlay_free_run(s, NULL, start, capacity, 0);
                return rc;
            }
            copied += chunk;
        }
        e->data_start = start;
        e->data_blocks = capacity;
    }
    e->flags &= ~HFS_OVL_BASE;
    return HFS_RO_OK;
}

int overlay_ensure_capacity(struct hfs_state *s,
                            struct hfs_overlay_entry *e,
                            uint64_t bytes, int *entry_synced)
{
    if (bytes > (uint64_t)(s->overlay.total_blocks - s->overlay.data_start) *
                HFS_OVL_BLOCK)
        return HFS_RO_BUFFER;
    uint32_t needed = (uint32_t)((bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    if (!needed || needed <= e->data_blocks)
        return HFS_RO_OK;
    uint32_t target = overlay_growth_target(&s->overlay, e->data_blocks, needed);
    int rc = overlay_realloc(s, e, target, entry_synced);
    if (rc == HFS_RO_BUFFER && target != needed)
        rc = overlay_realloc(s, e, needed, entry_synced);
    return rc;
}

int overlay_shrink(struct hfs_state *s, struct hfs_overlay_entry *e,
                   uint64_t bytes)
{
    uint32_t needed = (uint32_t)((bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    if ((e->flags & HFS_OVL_BASE) || needed >= e->data_blocks)
        return HFS_RO_OK;
    uint32_t old = e->data_blocks;
    uint32_t oldstart = e->data_start;
    uint64_t oldsize = e->size;
    unsigned int idx = (unsigned int)(e - s->overlay.entries);
    if (!needed) {
        /* Persist the empty entry first; a failed sync must not free the
         * blocks that the on-disk entry still references. */
        e->data_start = 0;
        e->data_blocks = 0;
        e->size = bytes;
        int rc = overlay_sync_entry(s, idx);
        if (rc) {
            e->data_start = oldstart;
            e->data_blocks = old;
            e->size = oldsize;
            return rc;
        }
        e->data_start = oldstart;
        e->data_blocks = old;
        rc = overlay_free_run(s, e, oldstart, old, 1);
        e->data_start = 0;
        e->data_blocks = 0;
        return rc;
    }

    /* Keep moderate slack. Database and PictureFlow files are commonly opened,
     * grown and closed repeatedly; trimming them to the exact last block made
     * v11 allocate again on the very next append. */
    if (old - needed <= HFS_OVL_SHRINK_HYSTERESIS || needed * 2u >= old)
        return HFS_RO_OK;
    uint32_t target = overlay_growth_target(&s->overlay, 0, needed);
    if (target > old)
        target = needed;
    e->data_blocks = target;
    e->size = bytes;
    int rc = overlay_sync_entry(s, idx);
    if (rc) {
        e->data_blocks = old;
        e->size = oldsize;
        return rc;
    }
    e->data_blocks = old;
    e->size = oldsize;
    rc = overlay_free_run(s, e, oldstart + target, old - target, 1);
    e->data_blocks = target;
    e->size = bytes;
    return rc;
}
