/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Overlay entry store: IDs, hash indexes, slot allocation and entry lifecycle.
 * This module does not decide namespace visibility or FAT return codes.
 */
#include "hfsplus_overlay_store.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_time.h"
#include <string.h>

static unsigned int overlay_id_bucket(uint32_t id)
{
    return (id * 2654435761u) & (HFS_OVL_ID_HASH_SIZE - 1u);
}

static unsigned int overlay_parent_bucket(uint32_t parent)
{
    parent ^= parent >> 16;
    return (parent * 2654435761u) & (HFS_OVL_PARENT_HASH_SIZE - 1u);
}

static void overlay_id_insert(struct hfs_overlay *o, unsigned int index)
{
    uint32_t id = o->entries[index].id;
    unsigned int b = overlay_id_bucket(id);
    unsigned int tomb = HFS_OVL_ID_HASH_SIZE;
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE) {
            if (tomb != HFS_OVL_ID_HASH_SIZE)
                b = tomb;
            o->id_hash[b] = (uint16_t)index;
            return;
        }
        if (v == HFS_OVL_NONE - 1u && tomb == HFS_OVL_ID_HASH_SIZE)
            tomb = b;
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
}

static void overlay_id_rebuild(struct hfs_overlay *o)
{
    const unsigned int limit = overlay_entry_limit(o);
    memset(o->id_hash, 0xff, sizeof(o->id_hash));
    for (unsigned int i = 0; i < limit; ++i)
        if (o->entries[i].flags & HFS_OVL_USED)
            overlay_id_insert(o, i);
}

static void overlay_id_remove(struct hfs_overlay *o, uint32_t id)
{
    unsigned int b = overlay_id_bucket(id);
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE)
            return;
        if (v < HFS_OVL_MAX_ENTRIES && o->entries[v].id == id) {
            o->id_hash[b] = HFS_OVL_NONE - 1u; /* probe-chain tombstone */
            if (++o->id_tombstones > HFS_OVL_ID_HASH_SIZE / 2u) {
                overlay_id_rebuild(o);
                o->id_tombstones = 0;
            }
            return;
        }
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
}

static void overlay_parent_insert(struct hfs_overlay *o, unsigned int index)
{
    unsigned int b = overlay_parent_bucket(o->entries[index].parent);
    o->parent_next[index] = o->parent_head[b];
    o->parent_head[b] = (uint16_t)index;
}

static void overlay_parent_remove(struct hfs_overlay *o, unsigned int index,
                                  uint32_t parent)
{
    unsigned int b = overlay_parent_bucket(parent);
    uint16_t *link = &o->parent_head[b];
    for (unsigned int n = 0; n < HFS_OVL_MAX_ENTRIES; ++n) {
        if (*link == HFS_OVL_NONE)
            return;
        if (*link == index) {
            *link = o->parent_next[index];
            o->parent_next[index] = HFS_OVL_NONE;
            return;
        }
        if (*link >= HFS_OVL_MAX_ENTRIES)
            return;
        link = &o->parent_next[*link];
    }
    /* A cycle in the RAM chain must not hang a filesystem operation. */
    o->parent_head[b] = HFS_OVL_NONE;
    o->parent_index_ok = 0;
}

void overlay_rebuild_indexes(struct hfs_overlay *o)
{
    const unsigned int limit = overlay_entry_limit(o);
    memset(o->id_hash, 0xff, sizeof(o->id_hash));
    o->id_tombstones = 0;
    memset(o->parent_head, 0xff, sizeof(o->parent_head));
    memset(o->parent_next, 0xff, sizeof(o->parent_next));
    o->parent_index_ok = 0;
    o->free_slot_hint = limit < HFS_OVL_MAX_ENTRIES ?
                        (uint16_t)limit : HFS_OVL_NONE;
    o->free_blocks = 0;
    o->alloc_hint = o->data_start;
    bool have_hint = false;
    uint32_t max_id = HFS_OVL_ID_BASE - 1u;
    for (uint32_t b = o->data_start; b < o->total_blocks; ++b) {
        if (!overlay_bit(o, b)) {
            ++o->free_blocks;
            if (!have_hint) {
                o->alloc_hint = b;
                have_hint = true;
            }
        }
    }
    for (unsigned int i = 0; i < limit; ++i) {
        if (!(o->entries[i].flags & HFS_OVL_USED)) {
            if (o->free_slot_hint == HFS_OVL_NONE ||
                i < o->free_slot_hint)
                o->free_slot_hint = (uint16_t)i;
            continue;
        }
        if (o->entries[i].id > max_id)
            max_id = o->entries[i].id;
        if (!overlay_entry_visible(o, i))
            continue;
        overlay_id_insert(o, i);
        overlay_parent_insert(o, i);
    }
    if (o->next_id <= max_id)
        o->next_id = max_id == 0x7fffffffu ? HFS_OVL_ID_BASE : max_id + 1u;
    /* Only v3 persists a high-water bound, so only v3 is known to have a
     * complete parent chain for every slot that can exist.  v1/v2 keep the
     * linear readdir fallback. */
    o->parent_index_ok = o->version == HFS_OVL_VERSION_3;
}

static int overlay_find_free_slot(struct hfs_overlay *o)
{
    unsigned int start = o->free_slot_hint < HFS_OVL_MAX_ENTRIES ?
                         o->free_slot_hint : 0;
    for (unsigned int n = 0; n < HFS_OVL_MAX_ENTRIES; ++n) {
        unsigned int i = (start + n) % HFS_OVL_MAX_ENTRIES;
        if (!(o->entries[i].flags & HFS_OVL_USED)) {
            o->free_slot_hint = (uint16_t)((i + 1u) % HFS_OVL_MAX_ENTRIES);
            return (int)i;
        }
    }
    o->free_slot_hint = HFS_OVL_NONE;
    return -1;
}

int overlay_find_id(const struct hfs_overlay *o, uint32_t id)
{
    unsigned int b = overlay_id_bucket(id);
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE)
            return -1;
        if (v < HFS_OVL_MAX_ENTRIES &&
            (o->entries[v].flags & HFS_OVL_USED) && o->entries[v].id == id)
            return (int)v;
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
    return -1;
}

static int overlay_name_reserved(uint32_t parent, const char *name)
{
    return parent == HFS_RO_ROOT_ID && overlay_name_eq(name, HFS_OVL_NAME);
}

void overlay_destroy_entry(struct hfs_overlay *o, unsigned int index)
{
    if (index >= HFS_OVL_MAX_ENTRIES)
        return;
    overlay_id_remove(o, o->entries[index].id);
    overlay_parent_remove(o, index, o->entries[index].parent);
    memset(&o->entries[index], 0, sizeof(o->entries[index]));
    o->shadowed[index] = 0;
    o->header_dirty = 1;
    if (o->free_slot_hint == HFS_OVL_NONE ||
        index < o->free_slot_hint)
        o->free_slot_hint = (uint16_t)index;
}

static int overlay_new_id(struct hfs_state *s, uint32_t *id)
{
    struct hfs_overlay *o = &s->overlay;
    if (o->next_id < HFS_OVL_ID_BASE || o->next_id >= 0x7fffffffu)
        o->next_id = HFS_OVL_ID_BASE;
    /* next_id is reconstructed from committed entries on mount.  Skip any ID
     * that survived a wrap or a damaged header. */
    for (unsigned int n = 0; n <= HFS_OVL_MAX_ENTRIES; ++n) {
        uint32_t candidate = o->next_id++;
        if (o->next_id >= 0x7fffffffu)
            o->next_id = HFS_OVL_ID_BASE;
        if (overlay_find_id(o, candidate) < 0) {
            *id = candidate;
            return HFS_RO_OK;
        }
    }
    return HFS_RO_BUFFER;
}

int overlay_create_entry(struct hfs_state *s, uint32_t parent,
                         const char *name, uint16_t kind,
                         uint32_t flags, uint32_t base_id,
                         uint64_t size, int *index)
{
    size_t n = strlen(name);
    if (!s->overlay.ready || !n || n > FAT_DIRENTRY_NAME_MAX ||
        (kind != 1 && kind != 2) || overlay_name_reserved(parent, name))
        return HFS_RO_ARGUMENT;
    if (!(flags & HFS_OVL_TOMBSTONE)) {
        for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
            const struct hfs_overlay_entry *e = &s->overlay.entries[i];
            if (overlay_entry_visible(&s->overlay, i) &&
                (e->flags & (HFS_OVL_USED | HFS_OVL_DELETED |
                             HFS_OVL_TOMBSTONE)) == HFS_OVL_USED &&
                e->parent == parent && overlay_name_eq(e->name, name))
                return HFS_RO_ARGUMENT;
        }
    }
    if (flags & (HFS_OVL_BASE | HFS_OVL_TOMBSTONE)) {
        for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
            const struct hfs_overlay_entry *e = &s->overlay.entries[i];
            if (!(e->flags & HFS_OVL_USED) ||
                e->parent != parent || e->base_id != base_id ||
                !overlay_name_eq(e->name, name))
                continue;
            if ((flags & HFS_OVL_TOMBSTONE) &&
                (e->flags & HFS_OVL_TOMBSTONE))
                return HFS_RO_OK;
            if ((flags & HFS_OVL_BASE) &&
                (e->flags & HFS_OVL_BASE) &&
                !(e->flags & HFS_OVL_TOMBSTONE))
                return HFS_RO_OK;
        }
    }
    int slot = overlay_find_free_slot(&s->overlay);
    if (slot < 0)
        return HFS_RO_BUFFER;
    if (s->overlay.version == HFS_OVL_VERSION_3 &&
        (uint32_t)slot >= s->overlay.entry_high_water) {
        uint32_t old_high = s->overlay.entry_high_water;
        uint32_t new_high = old_high + HFS_OVL_HIGH_WATER_CHUNK;
        if (new_high < (uint32_t)slot + 1u)
            new_high = (uint32_t)slot + 1u;
        if (new_high > HFS_OVL_MAX_ENTRIES)
            new_high = HFS_OVL_MAX_ENTRIES;
        s->overlay.entry_high_water = new_high;
        /* The loader must see the new high-water bound before the entry it
         * bounds becomes visible.  Reserving a chunk amortizes this header
         * flush across the next HFS_OVL_HIGH_WATER_CHUNK creates. */
        int hrc = overlay_sync_header(s);
        if (hrc) {
            s->overlay.entry_high_water = old_high;
            return hrc;
        }
    }
    struct hfs_overlay_entry *e = &s->overlay.entries[slot];
    memset(e, 0, sizeof(*e));
    int rc = overlay_new_id(s, &e->id);
    if (rc)
        return rc;
    e->parent = parent;
    e->flags = HFS_OVL_USED | flags;
    e->base_id = base_id;
    e->size = size;
    e->kind = kind;
    e->mtime = hfs_now();
    e->name_len = (uint16_t)n;
    memcpy(e->name, name, n + 1);
    rc = overlay_sync_entry(s, (unsigned int)slot);
    if (rc) {
        memset(e, 0, sizeof(*e));
    } else {
        overlay_id_insert(&s->overlay, (unsigned int)slot);
        overlay_parent_insert(&s->overlay, (unsigned int)slot);
        s->overlay.header_dirty = 1;
        if (index)
            *index = slot;
    }
    return rc;
}

int overlay_tombstone(struct hfs_state *s, uint32_t parent,
                             uint32_t base_id, const char *name)
{
    unsigned int b = overlay_parent_bucket(parent);
    for (unsigned int n = 0, i = s->overlay.parent_head[b];
         i != HFS_OVL_NONE && n < HFS_OVL_MAX_ENTRIES;
         ++n, i = s->overlay.parent_next[i]) {
        if (i >= HFS_OVL_MAX_ENTRIES)
            break;
        struct hfs_overlay_entry *e = &s->overlay.entries[i];
        if (overlay_entry_visible(&s->overlay, i) &&
            (e->flags & (HFS_OVL_USED | HFS_OVL_TOMBSTONE)) ==
                (HFS_OVL_USED | HFS_OVL_TOMBSTONE) &&
            e->parent == parent && e->base_id == base_id &&
            overlay_name_eq(e->name, name))
            return HFS_RO_OK;
    }
    int idx;
    return overlay_create_entry(s, parent, name, 2, HFS_OVL_TOMBSTONE,
                                base_id, 0, &idx);
}

struct hfs_overlay_entry *overlay_file_entry(struct hfs_state *s,
                                             const struct fat_file *file)
{
    if (!file->hfs_overlay || file->hfs_overlay_index >= HFS_OVL_MAX_ENTRIES ||
        !overlay_entry_visible(&s->overlay, file->hfs_overlay_index))
        return NULL;
    struct hfs_overlay_entry *e = &s->overlay.entries[file->hfs_overlay_index];
    if ((e->flags & (HFS_OVL_USED | HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) !=
            HFS_OVL_USED ||
        e->id != (uint32_t)file->firstcluster)
        return NULL;
    return e;
}

unsigned int overlay_parent_first(const struct hfs_overlay *o, uint32_t parent)
{
    return o->parent_head[overlay_parent_bucket(parent)];
}

unsigned int overlay_parent_next(const struct hfs_overlay *o,
                                 unsigned int index)
{
    return index < HFS_OVL_MAX_ENTRIES ? o->parent_next[index] : HFS_OVL_NONE;
}

/* RAM-only index updates.  The caller owns the persistent sync and rollback. */
void overlay_unindex_entry(struct hfs_overlay *o, unsigned int index)
{
    if (index >= HFS_OVL_MAX_ENTRIES)
        return;
    overlay_id_remove(o, o->entries[index].id);
    overlay_parent_remove(o, index, o->entries[index].parent);
}

void overlay_reindex_entry(struct hfs_overlay *o, unsigned int index)
{
    if (index >= HFS_OVL_MAX_ENTRIES || !overlay_entry_visible(o, index) ||
        !(o->entries[index].flags & HFS_OVL_USED))
        return;
    overlay_id_insert(o, index);
    overlay_parent_insert(o, index);
}

void overlay_parent_move(struct hfs_overlay *o, unsigned int index,
                         uint32_t new_parent)
{
    if (index >= HFS_OVL_MAX_ENTRIES ||
        o->entries[index].parent == new_parent)
        return;
    overlay_parent_remove(o, index, o->entries[index].parent);
    o->entries[index].parent = new_parent;
    overlay_parent_insert(o, index);
}
