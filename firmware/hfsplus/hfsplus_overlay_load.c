/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Overlay load: header/backup recovery, fresh initialization, bitmap and
 * chunked entry-table reads.  The bootloader never loads or trusts the
 * overlay, so this translation unit is empty there.
 */
#include "hfsplus_overlay_load.h"
#include "hfsplus_diag.h"
#include "hfsplus_overlay_alloc.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_overlay_store.h"
#include "hfsplus_ro.h"
#include <string.h>

#ifndef BOOTLOADER

static int overlay_init_fresh(struct hfs_state *s, struct hfs_overlay *o,
                              struct hfs_overlay_validation *vr)
{
    s->diag.ovl_stage = 10;
    o->version = HFS_OVL_VERSION_3;
    o->bitmap_start = 2u;
    o->cache_blocks = HFS_OVL_CACHE_BLOCKS;
    o->cache_start = o->bitmap_start + o->bitmap_blocks + o->entry_blocks;
    o->data_start = o->cache_start + o->cache_blocks;
    o->entry_high_water = 0;
    o->state = HFS_OVL_STATE_INIT;
    o->next_id = HFS_OVL_ID_BASE;
    o->generation = 0;
    o->header_recovered = 0;
    o->ready = 0;
    if (o->data_start + 16u >= o->total_blocks)
        return overlay_diag_fail(s, 2, HFS_RO_BUFFER, -1);

    /* Persist the init marker before touching the table.  A power loss after
     * this point is recoverable by rerunning the same idempotent sequence. */
    int rc = overlay_sync_header(s);
    if (rc)
        return overlay_diag_fail(s, 10, rc, -1);

    memset(o->bitmap, 0, sizeof(o->bitmap));
    memset(o->entries, 0, sizeof(o->entries));
    for (uint32_t b = 0; b < o->data_start; ++b)
        overlay_set_bit(o, b, 1);

    memset(s->scratch, 0, sizeof(s->scratch));
    uint64_t left = (uint64_t)(o->entry_blocks + o->cache_blocks) *
                    HFS_OVL_BLOCK;
    uint64_t off = (uint64_t)(o->bitmap_start + o->bitmap_blocks) *
                   HFS_OVL_BLOCK;
    while (left) {
        size_t chunk = left > sizeof(s->scratch) ?
                       sizeof(s->scratch) : (size_t)left;
        rc = overlay_container_write(s, off, s->scratch, chunk);
        if (rc)
            return overlay_diag_fail(s, 10, rc, -1);
        off += chunk;
        left -= chunk;
    }
    rc = overlay_sync_bitmap(s);
    if (rc)
        return overlay_diag_fail(s, 10, rc, -1);
    rc = overlay_validate(o, vr);
    if (rc)
        return overlay_diag_validation(s, vr);
    overlay_rebuild_indexes(o);
    o->state = HFS_OVL_STATE_READY;
    o->ready = 1;
    rc = overlay_sync_header(s);
    if (rc) {
        o->ready = 0;
        return overlay_diag_fail(s, 10, rc, -1);
    }
    s->diag.ovl_stage = 9;
    return HFS_RO_OK;
}

int overlay_load(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    struct hfs_overlay_validation vr;
    s->diag.ovl_error = 0;
    s->diag.ovl_stage = 1; /* lookup /.rockpod-rw */
    s->diag.ovl_slot = -1;
    s->diag.ovl_reason = (int)HFS_OVL_REASON_NONE;
    memset(o, 0, sizeof(*o));

    struct hfs_ro_entry container;
    int rc = hfs_ro_lookup(&s->volume, "/" HFS_OVL_NAME, &container);
    if (rc)
        return overlay_diag_fail(s, 1, rc, -1);

    s->diag.ovl_stage = 2; /* validate container/extents */
    uint64_t inline_blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        inline_blocks += container.data.extents[i].count;
    if (container.kind != 2 || container.unsupported ||
        inline_blocks != container.data.blocks ||
        container.data.size < 16u * 1024u * 1024u ||
        hfs_ro_fork_writable(&s->volume, &container.data) != HFS_RO_OK)
        return overlay_diag_fail(s, 2, HFS_RO_UNSUPPORTED, -1);

    o->container = container;
    o->total_blocks = (uint32_t)(container.data.size / HFS_OVL_BLOCK);
    if (o->total_blocks > HFS_OVL_MAX_BLOCKS)
        o->total_blocks = HFS_OVL_MAX_BLOCKS;
    o->bitmap_bytes = (o->total_blocks + 7u) / 8u;
    o->bitmap_blocks = (o->bitmap_bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK;
    o->entry_blocks = (HFS_OVL_MAX_ENTRIES * HFS_OVL_ENTRY_SIZE +
                       HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK;
    o->version = HFS_OVL_VERSION_3;
    o->bitmap_start = 2u;
    o->cache_blocks = HFS_OVL_CACHE_BLOCKS;
    o->cache_start = o->bitmap_start + o->bitmap_blocks + o->entry_blocks;
    o->data_start = o->cache_start + o->cache_blocks;
    o->entry_high_water = HFS_OVL_MAX_ENTRIES;
    o->state = HFS_OVL_STATE_READY;
    if (o->data_start + 16u >= o->total_blocks)
        return overlay_diag_fail(s, 2, HFS_RO_BUFFER, -1);

    s->diag.ovl_stage = 3; /* read overlay header */
    rc = overlay_container_read(s, 0, s->scratch, HFS_OVL_BLOCK);
    if (rc)
        return overlay_diag_fail(s, 3, rc, -1);
    int hrc = overlay_parse_header(s, s->scratch, 0);
    if (hrc) {
        uint8_t *backup = s->scratch + HFS_OVL_BLOCK;
        bool primary_v1 = !memcmp(s->scratch, HFS_OVL_MAGIC_V1, 7);
        /* Probe both backup layouts: v2's full block 1 and v1's 64-byte copy
         * at offset 2048.  A corrupt primary magic must not stop us trying the
         * legacy copy. */
        uint64_t backup_off[2] = { HFS_OVL_BLOCK, 2048u };
        size_t backup_len[2] = { HFS_OVL_BLOCK, 64u };
        if (primary_v1) {
            backup_off[0] = 2048u;
            backup_len[0] = 64u;
            backup_off[1] = HFS_OVL_BLOCK;
            backup_len[1] = HFS_OVL_BLOCK;
        }
        for (unsigned int n = 0; n < 2 && hrc; ++n) {
            rc = overlay_container_read(s, backup_off[n], backup,
                                        backup_len[n]);
            if (rc)
                return overlay_diag_fail(s, 3, rc, -1);
            hrc = overlay_parse_header(s, backup, 1);
        }
        if (!hrc) {
            memcpy(s->scratch, backup, 64);
            if (o->version == HFS_OVL_VERSION_3 &&
                o->state == HFS_OVL_STATE_INIT)
                return overlay_init_fresh(s, o, &vr);
        } else {
            int zrc = overlay_container_all_zero(s, 0,
                                                 2u * HFS_OVL_BLOCK);
            if (zrc < 0)
                return overlay_diag_fail(s, 3, zrc, -1);
            if (!zrc)
                return overlay_diag_fail(s, 3, HFS_RO_FORMAT, -1);
            zrc = overlay_container_all_zero(s,
                        (uint64_t)2u * HFS_OVL_BLOCK,
                        (uint64_t)o->bitmap_blocks * HFS_OVL_BLOCK);
            if (zrc < 0)
                return overlay_diag_fail(s, 3, zrc, -1);
            if (!zrc)
                return overlay_diag_fail(s, 3, HFS_RO_FORMAT, -1);
            zrc = overlay_container_all_zero(s,
                        (uint64_t)(2u + o->bitmap_blocks) * HFS_OVL_BLOCK,
                        (uint64_t)(o->entry_blocks + o->cache_blocks) *
                        HFS_OVL_BLOCK);
            if (zrc < 0)
                return overlay_diag_fail(s, 3, zrc, -1);
            if (!zrc)
                return overlay_diag_fail(s, 3, HFS_RO_FORMAT, -1);
            return overlay_init_fresh(s, o, &vr);
        }
    }

    if (o->version == HFS_OVL_VERSION_3 &&
        o->state == HFS_OVL_STATE_INIT)
        return overlay_init_fresh(s, o, &vr);

    s->diag.ovl_stage = 4; /* validate overlay header */
    if (o->next_id < HFS_OVL_ID_BASE || o->next_id > 0x7fffffffu)
        return overlay_diag_fail(s, 4, HFS_RO_FORMAT, -1);

    s->diag.ovl_stage = 5; /* read allocation bitmap */
    rc = overlay_container_read(s,
                                (uint64_t)o->bitmap_start * HFS_OVL_BLOCK,
                                o->bitmap, o->bitmap_bytes);
    if (rc)
        return overlay_diag_fail(s, 5, rc, -1);

    s->diag.ovl_stage = 6; /* validate reserved bitmap blocks */
    for (uint32_t b = 0; b < o->data_start; ++b)
        if (!overlay_bit(o, b))
            return overlay_diag_fail(s, 6, HFS_RO_FORMAT, (int)b);

    /* Read the entry table in chunks.  Reading 320 bytes per slot made
     * startup wait for 2,048 small ATA requests even when only a handful of
     * entries were used. */
    unsigned int entry_limit = o->version == HFS_OVL_VERSION_3 ?
                               o->entry_high_water : HFS_OVL_MAX_ENTRIES;
    if (entry_limit > HFS_OVL_MAX_ENTRIES)
        entry_limit = HFS_OVL_MAX_ENTRIES;
    for (unsigned int base = 0; base < entry_limit;
         base += HFS_OVL_READ_CHUNK) {
        unsigned int count = entry_limit - base;
        if (count > HFS_OVL_READ_CHUNK)
            count = HFS_OVL_READ_CHUNK;
        s->diag.ovl_stage = 7; /* read a table chunk */
        rc = overlay_container_read(s, overlay_entry_offset(o, base),
                                    s->scratch,
                                    count * HFS_OVL_ENTRY_SIZE);
        if (rc)
            return overlay_diag_fail(s, 7, rc, (int)base);
        s->diag.ovl_stage = 8; /* decode/validate entries */
        for (unsigned int k = 0; k < count; ++k) {
            unsigned int i = base + k;
            rc = overlay_decode_entry(&o->entries[i],
                                      s->scratch + k * HFS_OVL_ENTRY_SIZE);
            if (rc) {
                /* Preserve exactly what the iPod decoded before the validator
                 * rejected this slot. This distinguishes bad on-device reads
                 * from a validator/format assumption mismatch. */
                s->diag.ovl_id = o->entries[i].id;
                s->diag.ovl_parent = o->entries[i].parent;
                s->diag.ovl_flags = o->entries[i].flags;
                s->diag.ovl_data_start = o->entries[i].data_start;
                s->diag.ovl_data_blocks = o->entries[i].data_blocks;
                s->diag.ovl_kind = o->entries[i].kind;
                s->diag.ovl_name_len = o->entries[i].name_len;
                return overlay_diag_fail(s, 8, rc, (int)i);
            }
        }
    }

    rc = overlay_validate(o, &vr);
    if (rc)
        return overlay_diag_validation(s, &vr);
    overlay_rebuild_indexes(o);
    o->ready = 1;
    if (o->header_recovered) {
        rc = overlay_sync_header(s);
        if (rc) {
            o->ready = 0;
            return overlay_diag_fail(s, 10, rc, -1);
        }
        o->header_recovered = 0;
    }
    s->diag.ovl_error = 0;
    s->diag.ovl_stage = 9; /* ready */
    s->diag.ovl_slot = -1;
    return HFS_RO_OK;
}

#endif
