/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Whole-overlay validation.  This is deliberately separate from the loader,
 * allocator and FAT adapter: it must not publish READY (or allow writes) when
 * any persistent entry disagrees with the bitmap, range ownership or parent
 * graph.
 */
#include "hfsplus_overlay.h"
#include <string.h>

static const struct hfs_overlay_entry *
overlay_find_id_linear(const struct hfs_overlay *o, uint32_t id)
{
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i)
        if ((o->entries[i].flags & HFS_OVL_USED) &&
            o->entries[i].id == id)
            return &o->entries[i];
    return NULL;
}

static int overlay_validate_fail(struct hfs_overlay_validation *result,
                                 int slot, enum hfs_overlay_reason reason)
{
    result->slot = slot;
    result->reason = reason;
    return HFS_RO_FORMAT;
}

int overlay_validate(struct hfs_overlay *o,
                     struct hfs_overlay_validation *result)
{
    uint32_t max_id = HFS_OVL_ID_BASE - 1u;
    result->slot = -1;
    result->reason = HFS_OVL_REASON_NONE;
    memset(o->shadowed, 0, sizeof(o->shadowed));

    /* ID uniqueness is independent of legacy name/BASE shadowing.  Use the
     * runtime ID hash as scratch; it is rebuilt from the persistent entries
     * after validation succeeds. */
    memset(o->id_hash, 0xff, sizeof(o->id_hash));
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        const struct hfs_overlay_entry *e = &o->entries[i];
        if (!(e->flags & HFS_OVL_USED))
            continue;
        if (e->id < HFS_OVL_ID_BASE || e->id > 0x7fffffffu)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_ENTRY_ID);
        unsigned int b = (e->id * 2654435761u) & (HFS_OVL_ID_HASH_SIZE - 1u);
        unsigned int n;
        for (n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
            uint16_t v = o->id_hash[b];
            if (v == HFS_OVL_NONE) {
                o->id_hash[b] = (uint16_t)i;
                break;
            }
            if (o->entries[v].id == e->id)
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_DUPLICATE_ID);
            b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
        }
        if (n == HFS_OVL_ID_HASH_SIZE)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_DUPLICATE_ID);
    }

    if (!o->total_blocks || !o->bitmap_bytes ||
        (o->version != HFS_OVL_VERSION_V1 &&
         o->version != HFS_OVL_VERSION &&
         o->version != HFS_OVL_VERSION_3) ||
        (o->version == HFS_OVL_VERSION_3 &&
         (o->cache_blocks != HFS_OVL_CACHE_BLOCKS ||
          o->cache_start != o->bitmap_start + o->bitmap_blocks +
                            o->entry_blocks ||
          o->entry_high_water > HFS_OVL_MAX_ENTRIES ||
          o->state > HFS_OVL_STATE_INIT)) ||
        o->bitmap_start != (o->version == HFS_OVL_VERSION_V1 ? 1u : 2u) ||
        o->data_start != o->bitmap_start + o->bitmap_blocks +
                          o->entry_blocks + o->cache_blocks ||
        o->data_start >= o->total_blocks ||
        o->bitmap_blocks > o->total_blocks ||
        o->entry_blocks > o->total_blocks)
        return overlay_validate_fail(result, -1, HFS_OVL_REASON_LAYOUT);

    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        const struct hfs_overlay_entry *e = &o->entries[i];
        if (!(e->flags & HFS_OVL_USED))
            continue;
        if (e->parent < HFS_RO_ROOT_ID || e->parent == e->id)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_PARENT);
        if (e->name_len == 0 || e->name_len > FAT_DIRENTRY_NAME_MAX)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_ENTRY_NAME);
        if (e->flags & ~(HFS_OVL_USED | HFS_OVL_DELETED |
                         HFS_OVL_BASE | HFS_OVL_TOMBSTONE))
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_ENTRY_FLAGS);
        if (e->id > max_id)
            max_id = e->id;
        if (e->name[e->name_len] != '\0' ||
            strlen(e->name) != e->name_len)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_ENTRY_NAME);
        if (overlay_entry_visible(o, i)) {
            for (unsigned int j = 0; j < i; ++j) {
                const struct hfs_overlay_entry *p = &o->entries[j];
                if (!(p->flags & HFS_OVL_USED) || !overlay_entry_visible(o, j))
                    continue;
                /* Old firmware allowed duplicate live names and duplicate
                 * BASE shadows.  Keep the newest ID and hide the older one in
                 * RAM; never reject an otherwise readable legacy overlay. */
                if ((e->flags & (HFS_OVL_BASE | HFS_OVL_TOMBSTONE)) &&
                    (p->flags & (HFS_OVL_BASE | HFS_OVL_TOMBSTONE)) &&
                    p->parent == e->parent && p->base_id == e->base_id &&
                    p->flags == e->flags &&
                    overlay_name_eq(p->name, e->name)) {
                    if (p->id < e->id)
                        o->shadowed[j] = 1;
                    else
                        o->shadowed[i] = 1;
                }
                if (!(e->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) &&
                    !(p->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) &&
                    e->parent == p->parent &&
                    overlay_name_eq(e->name, p->name)) {
                    if (p->id < e->id)
                        o->shadowed[j] = 1;
                    else
                        o->shadowed[i] = 1;
                }
                if ((e->flags & HFS_OVL_BASE) && (p->flags & HFS_OVL_BASE) &&
                    e->base_id == p->base_id) {
                    if (p->id < e->id)
                        o->shadowed[j] = 1;
                    else
                        o->shadowed[i] = 1;
                }
                if (!overlay_entry_visible(o, i))
                    break;
            }
        }
        if (e->flags & HFS_OVL_TOMBSTONE) {
            if (e->data_blocks || e->data_start || e->size ||
                !e->base_id || e->base_id >= HFS_OVL_ID_BASE ||
                (e->kind != 1 && e->kind != 2))
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_TOMBSTONE);
            continue;
        }
        if (e->kind != 1 && e->kind != 2)
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_ENTRY_KIND);
        if (e->flags & HFS_OVL_BASE) {
            if (!e->base_id || e->base_id >= HFS_OVL_ID_BASE)
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_BASE_ID);
        } else if (e->base_id >= HFS_OVL_ID_BASE) {
            /* Legacy overlays stored a native backing CNID without setting
             * HFS_OVL_BASE; keep it as a hint for hiding the native name, but
             * do not treat the entry as an unmaterialized BASE shadow. */
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_BASE_ID);
        }
        if (e->data_blocks) {
            if (e->data_start < o->data_start ||
                e->data_start > o->total_blocks ||
                e->data_blocks > o->total_blocks - e->data_start)
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_DATA_RANGE);
            if (e->size > (uint64_t)e->data_blocks * HFS_OVL_BLOCK)
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_DATA_SIZE);
            for (uint32_t b = e->data_start;
                 b < e->data_start + e->data_blocks; ++b)
                if (!overlay_bit(o, b))
                    return overlay_validate_fail(
                        result, (int)i, HFS_OVL_REASON_DATA_UNALLOCATED);
            for (unsigned int j = 0; j < i; ++j) {
                const struct hfs_overlay_entry *p = &o->entries[j];
                if ((p->flags & HFS_OVL_USED) && p->data_blocks &&
                    e->data_start < p->data_start + p->data_blocks &&
                    p->data_start < e->data_start + e->data_blocks)
                    return overlay_validate_fail(result, (int)i,
                                                 HFS_OVL_REASON_DATA_OVERLAP);
            }
        } else if (e->size && !(e->flags & HFS_OVL_BASE)) {
            return overlay_validate_fail(result, (int)i,
                                         HFS_OVL_REASON_DATA_SIZE);
        }
        if (e->parent >= HFS_OVL_ID_BASE) {
            uint32_t parent = e->parent;
            unsigned int depth = 0;
            while (parent >= HFS_OVL_ID_BASE) {
                if (parent == e->id || ++depth > HFS_OVL_MAX_ENTRIES)
                    return overlay_validate_fail(result, (int)i,
                                                 HFS_OVL_REASON_PARENT);
                const struct hfs_overlay_entry *p =
                    overlay_find_id_linear(o, parent);
                if (!p || (p->flags & (HFS_OVL_DELETED |
                                       HFS_OVL_TOMBSTONE)) ||
                    p->kind != 1)
                    return overlay_validate_fail(result, (int)i,
                                                 HFS_OVL_REASON_PARENT);
                parent = p->parent;
            }
            if (parent < HFS_RO_ROOT_ID)
                return overlay_validate_fail(result, (int)i,
                                             HFS_OVL_REASON_PARENT);
        }
    }
    if (o->next_id < HFS_OVL_ID_BASE || o->next_id > 0x7fffffffu)
        return overlay_validate_fail(result, -1, HFS_OVL_REASON_NEXT_ID);
    if (o->next_id <= max_id) {
        if (max_id == 0x7fffffffu)
            return overlay_validate_fail(result, -1, HFS_OVL_REASON_NEXT_ID);
        o->next_id = max_id + 1u;
    }
    return HFS_RO_OK;
}
