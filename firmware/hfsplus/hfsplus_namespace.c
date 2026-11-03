/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Namespace merge: native HFS+ entries plus overlay entries, tombstones and
 * visibility.  This module owns parent/name/identity policy; it does not own
 * the on-disk format or allocator.
 */
#include "hfsplus_rb.h"
#include "hfsplus_state.h"
#include "hfsplus_overlay_store.h"
#include "hfsplus_overlay_alloc.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_time.h"
#include "hfsplus_fat.h"
#include "hfsplus_ro.h"
#include "fs_attr.h"
#include "string-extra.h"
#include <string.h>

static int overlay_hides_match(const struct hfs_overlay *o, unsigned int idx,
                               uint32_t parent, uint32_t base_id,
                               const char *name, int cnid_reused)
{
    const struct hfs_overlay_entry *e = &o->entries[idx];
    if (!(e->flags & HFS_OVL_USED) || !overlay_entry_visible(o, idx) ||
        e->parent != parent || e->base_id != base_id)
        return 0;
    if (e->flags & HFS_OVL_TOMBSTONE) {
        /* CNID reuse makes a tombstone's base_id ambiguous.  Ignoring it is
         * safer than hiding a newly created object. */
        if (cnid_reused || !overlay_name_eq(e->name, name))
            return 0;
        return 1;
    }
    return !(e->flags & HFS_OVL_DELETED) &&
           overlay_name_eq(e->name, name);
}

static int overlay_hides_base(const struct hfs_overlay *o, uint32_t parent,
                              uint32_t backing_parent, uint32_t base_id,
                              const char *name, int cnid_reused)
{
    /* A shadowed base directory has both a stable native CNID (backing) and a
     * new overlay ID.  Entries created before a rename retain the old parent,
     * so both bucket chains must be considered.  The parent hash is rebuilt
     * and validated from the persistent entries at mount; legacy v1/v2 images
     * keep the bounded linear fallback. */
    const unsigned int limit = overlay_entry_limit(o);
    for (unsigned int pass = 0; pass < 2; ++pass) {
        uint32_t p = pass ? backing_parent : parent;
        if (!p || (pass && p == parent))
            continue;
        if (!o->parent_index_ok) {
            for (unsigned int i = 0; i < limit; ++i)
                if (overlay_hides_match(o, i, p, base_id, name, cnid_reused))
                    return 1;
            continue;
        }
        for (unsigned int n = 0, idx = overlay_parent_first(o, p);
             idx != HFS_OVL_NONE && n < HFS_OVL_MAX_ENTRIES;
             ++n, idx = overlay_parent_next(o, idx)) {
            if (idx >= limit)
                break;
            if (overlay_hides_match(o, idx, p, base_id, name, cnid_reused))
                return 1;
        }
    }
    return 0;
}

int overlay_shadow_base(struct hfs_state *s, struct fat_file *file,
                        uint32_t new_parent, const char *new_name)
{
    if (!s->overlay.ready || file->hfs_overlay)
        return HFS_RO_ARGUMENT;
    struct hfs_ro_entry base;
    int rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
    if (rc)
        return rc;
    /* Another handle may already have shadowed this exact base object.  Reuse
     * that entry instead of creating a second live name for one file.  A
     * different live name for the same native object is a conflicting shadow;
     * refuse it rather than duplicating the object. */
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        struct hfs_overlay_entry *e = &s->overlay.entries[i];
        if ((e->flags & (HFS_OVL_USED | HFS_OVL_DELETED |
                         HFS_OVL_TOMBSTONE)) != HFS_OVL_USED ||
            e->base_id != base.id)
            continue;
        if (e->parent != new_parent ||
            !overlay_name_eq(e->name, new_name))
            return HFS_RO_FORMAT;
        rc = overlay_tombstone(s, (uint32_t)file->dircluster, base.id,
                               base.name);
        if (rc)
            return rc;
        file->firstcluster = (long)e->id;
        file->dircluster = (long)new_parent;
        file->e.entry = e->id;
        file->hfs_overlay = 1;
        file->hfs_overlay_index = (uint16_t)i;
        file->hfs_backing_id = base.id;
        file->hfs_kind = base.kind;
        file->hfs_data = base.data;
        file->hfs_data.size = e->size;
        return HFS_RO_OK;
    }
    int vis;
    rc = overlay_create_entry(s, new_parent, new_name, base.kind,
                              HFS_OVL_BASE, base.id, base.data.size, &vis);
    if (rc)
        return rc;
    rc = overlay_tombstone(s, (uint32_t)file->dircluster, base.id, base.name);
    if (rc) {
        overlay_destroy_entry(&s->overlay, (unsigned int)vis);
        overlay_sync_entry(s, (unsigned int)vis);
        return rc;
    }
    file->firstcluster = (long)s->overlay.entries[vis].id;
    file->dircluster = (long)new_parent;
    file->e.entry = s->overlay.entries[vis].id;
    file->hfs_overlay = 1;
    file->hfs_overlay_index = (uint16_t)vis;
    file->hfs_backing_id = base.id;
    file->hfs_kind = base.kind;
    file->hfs_data = base.data;
    return HFS_RO_OK;
}

int hfsplus_open_root(int volume, struct fat_file *file)
{
    if (!file || !hfs_valid_volume(volume))
        return HFS_RO_ARGUMENT;
    memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
    file->volume = volume;
#else
    (void)volume;
#endif
    file->firstcluster = HFS_RO_ROOT_ID;
    file->hfs_kind = 1;
    file->hfs_backing_id = HFS_RO_ROOT_ID;
    file->hfs_overlay_index = HFS_OVL_NONE;
    file->e.entry = HFS_RO_ROOT_ID;
    return 0;
}

int hfsplus_open(const struct fat_file *parent, long id, struct fat_file *file)
{
    if (!hfs_valid_file(parent) || !file)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(parent->volume)];
    int rc = HFS_RO_NOT_FOUND;
    mutex_lock(&s->mutex);
    /* The bootloader never publishes an overlay; skip the RAM index walk
     * entirely instead of touching an unpopulated id_hash. */
    int oi = s->overlay.ready ? overlay_find_id(&s->overlay, (uint32_t)id) : -1;
    if (oi >= 0) {
        struct hfs_overlay_entry *oe = &s->overlay.entries[oi];
        uint32_t logical_parent = (uint32_t)parent->firstcluster;
        uint32_t backing_parent = parent->hfs_overlay && parent->hfs_backing_id ?
                                  parent->hfs_backing_id : logical_parent;
        if (oe->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE) ||
            (oe->parent != logical_parent && oe->parent != backing_parent))
            goto out;
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = (long)oe->id;
        file->dircluster = parent->firstcluster;
        file->e.entry = oe->id;
        file->hfs_kind = oe->kind;
        file->hfs_overlay = 1;
        file->hfs_overlay_index = (uint16_t)oi;
        file->hfs_backing_id = oe->base_id;
        file->hfs_data.size = oe->size;
        if ((oe->flags & HFS_OVL_BASE) && oe->base_id) {
            struct hfs_ro_entry base;
            rc = hfs_ro_find_id(&s->volume, oe->base_id, &base);
            if (rc)
                goto out;
            file->hfs_data = base.data;
            file->hfs_data.size = oe->size;
            if (oe->kind == 1)
                file->hfs_backing_id = base.id;
        }
        rc = 0;
        goto out;
    }

    uint32_t backing_parent = parent->hfs_overlay && parent->hfs_backing_id ?
                              parent->hfs_backing_id : (uint32_t)parent->firstcluster;
    struct hfs_ro_iterator it;
    struct hfs_ro_entry e;
    rc = hfs_ro_iter_init(&s->volume, backing_parent, &it);
    if (rc)
        goto out;
    while ((rc = hfs_ro_iter_next(&s->volume, &it, &e)) > 0) {
        if (e.id != (uint32_t)id)
            continue;
        if (e.unsupported || e.data.size > FAT_MAX_FILE_SIZE ||
            strlen(e.name) > FAT_DIRENTRY_NAME_MAX) {
            rc = HFS_RO_UNSUPPORTED;
            goto out;
        }
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = id;
        file->dircluster = parent->firstcluster;
        file->e.entry = e.id;
        file->hfs_data = e.data;
        file->hfs_kind = e.kind;
        file->hfs_backing_id = e.kind == 1 ? e.id : 0;
        file->hfs_overlay_index = HFS_OVL_NONE;
        rc = 0;
        goto out;
    }
    if (!rc)
        rc = HFS_RO_NOT_FOUND;
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_create_file(struct fat_file *parent, const char *name,
                        uint8_t attr, struct fat_file *file,
                        struct fat_direntry *fatent)
{
    if (!parent || !name || !file || !hfs_valid_file(parent))
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(parent->volume)];
    mutex_lock(&s->mutex);
    hfs_diag_op(s, HFSPLUS_OP_CREATE);
    bool settings = hfs_settings_name(name);
    if (settings)
        hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
    hfs_diag_begin(s, settings ? HFSPLUS_OP_SETTINGS : HFSPLUS_OP_CREATE);
    if (!s->overlay.ready) {
        hfs_diag_end(s);
        mutex_unlock(&s->mutex);
        return HFS_RO_UNSUPPORTED;
    }
    uint16_t kind = (attr & ATTR_DIRECTORY) ? 1 : 2;
    int idx;
    int rc = overlay_create_entry(s, (uint32_t)parent->firstcluster, name,
                                  kind, 0, 0, 0, &idx);
    if (!rc) {
        struct hfs_overlay_entry *e = &s->overlay.entries[idx];
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = (long)e->id;
        file->dircluster = parent->firstcluster;
        file->e.entry = e->id;
        file->hfs_kind = kind;
        file->hfs_overlay = 1;
        file->hfs_overlay_index = (uint16_t)idx;
        if (fatent)
            fill_overlay_fatent(e, fatent);
    }
    hfs_diag_end(s);
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

int hfsplus_remove(struct fat_file *file, enum fat_remove_op what)
{
    if (!hfs_valid_file(file) || file->firstcluster == HFS_RO_ROOT_ID)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    hfs_diag_op(s, HFSPLUS_OP_REMOVE);
    hfs_diag_begin(s, HFSPLUS_OP_REMOVE);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        if (hfs_settings_name(e->name)) {
            hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
            hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
        }
        unsigned int idx = file->hfs_overlay_index;
        if (what & FAT_RM_DIRENTRIES) {
            e->flags |= HFS_OVL_DELETED;
            rc = overlay_sync_entry(s, idx);
            if (rc)
                goto out;
            file->dircluster = 0;
            file->hfs_removed = 1;
        }
        if (what & FAT_RM_DATA) {
            /* Persist the cleared entry before returning its blocks.  A power
             * loss or failed entry write may leak blocks, but must never leave
             * a live entry pointing at freed blocks. */
            struct hfs_overlay_entry old = *e;
            uint32_t data_start = e->data_start;
            uint32_t data_blocks = e->data_blocks;
            int free_data = !(e->flags & HFS_OVL_BASE) && data_blocks != 0;
            overlay_unindex_entry(&s->overlay, idx);
            memset(e, 0, sizeof(*e));
            rc = overlay_sync_entry(s, idx);
            if (rc) {
                *e = old;
                overlay_reindex_entry(&s->overlay, idx);
                goto out;
            }
            if ((uint16_t)idx < s->overlay.free_slot_hint ||
                s->overlay.free_slot_hint == HFS_OVL_NONE)
                s->overlay.free_slot_hint = (uint16_t)idx;
            file->hfs_overlay = 0;
            file->hfs_overlay_index = HFS_OVL_NONE;
            if (free_data) {
                /* The entry is already cleared on disk.  Restore it only in
                 * RAM long enough to prove ownership of the run being freed. */
                *e = old;
                rc = overlay_free_run(s, e, data_start, data_blocks, 1);
                memset(e, 0, sizeof(*e));
                if (rc)
                    goto out;
            }
        }
    } else {
        if (what & FAT_RM_DIRENTRIES) {
            struct hfs_ro_entry base;
            rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
            if (rc)
                goto out;
            if (hfs_settings_name(base.name)) {
                hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
                hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
            }
            rc = overlay_tombstone(s, (uint32_t)file->dircluster,
                                   base.id, base.name);
            if (rc)
                goto out;
            file->dircluster = 0;
            file->hfs_removed = 1;
        }
        /* Base HFS+ data remains allocated and becomes unreachable only from
         * HFSPlus Rockboxed iPod merged namespace; native metadata is never modified. */
    }
out:
    hfs_diag_end(s);
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

int hfsplus_rename(struct fat_file *parent, struct fat_file *file,
                   const unsigned char *newname)
{
    if (!hfs_valid_file(parent) || !hfs_valid_file(file) || !newname ||
        file->firstcluster == HFS_RO_ROOT_ID)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    hfs_diag_op(s, HFSPLUS_OP_RENAME);
    hfs_diag_begin(s, HFSPLUS_OP_RENAME);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        size_t len = strlen((const char *)newname);
        if (!len || len > FAT_DIRENTRY_NAME_MAX) {
            rc = HFS_RO_ARGUMENT;
            goto out;
        }
        uint32_t old_parent = e->parent;
        char old_name[261];
        uint16_t old_name_len = e->name_len;
        memcpy(old_name, e->name, old_name_len + 1);
        if (hfs_settings_name(old_name) ||
            hfs_settings_name((const char *)newname)) {
            hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
            hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
        }
        uint32_t new_parent = (uint32_t)parent->firstcluster;
        overlay_parent_move(&s->overlay, file->hfs_overlay_index, new_parent);
        e->name_len = (uint16_t)len;
        memcpy(e->name, newname, len + 1);
        rc = overlay_sync_entry(s, file->hfs_overlay_index);
        if (!rc) {
            file->dircluster = parent->firstcluster;
        } else {
            overlay_parent_move(&s->overlay, file->hfs_overlay_index, old_parent);
            e->name_len = old_name_len;
            memcpy(e->name, old_name, old_name_len + 1);
        }
    } else {
        if (hfs_settings_name((const char *)newname)) {
            hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
            hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
        }
        rc = overlay_shadow_base(s, file, (uint32_t)parent->firstcluster,
                                 (const char *)newname);
    }
out:
    hfs_diag_end(s);
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

int hfsplus_modtime(struct fat_file *parent, struct fat_file *file,
                    time_t modtime)
{
    (void)parent;
    if (!hfs_valid_file(file))
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    hfs_diag_begin(s, -1);
    bool settings = false;
    int rc = HFS_RO_OK;
    if (!file->hfs_overlay) {
        struct hfs_ro_entry base;
        rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (!rc) {
            settings = hfs_settings_name(base.name);
            if (settings) {
                hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
                hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
            }
            rc = overlay_shadow_base(s, file, (uint32_t)file->dircluster,
                                     base.name);
        }
    }
    if (!rc && file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e)
            rc = HFS_RO_FORMAT;
        else {
            if (!settings && hfs_settings_name(e->name)) {
                hfs_diag_op(s, HFSPLUS_OP_SETTINGS);
                hfs_diag_begin(s, HFSPLUS_OP_SETTINGS);
            }
            e->mtime = (uint32_t)modtime;
            rc = overlay_sync_entry(s, file->hfs_overlay_index);
        }
    }
    hfs_diag_end(s);
    mutex_unlock(&s->mutex);
    return hfs_file_rc(rc);
}

static int overlay_readdir_next(struct hfs_overlay *o, uint32_t logical_parent,
                                uint32_t backing_parent,
                                struct fat_dirscan_info *scan,
                                struct fat_direntry *entry)
{
    const unsigned int limit = overlay_entry_limit(o);
    if (!o->parent_index_ok) {
        /* Legacy v1/v2 images, or a parent chain repaired after corruption,
         * keep the bounded linear namespace walk. */
        if (scan->hfs_overlay_index == HFS_OVL_NONE)
            scan->hfs_overlay_index = 0;
        while (scan->hfs_overlay_index < limit) {
            unsigned int idx = scan->hfs_overlay_index++;
            struct hfs_overlay_entry *oe = &o->entries[idx];
            if (!(oe->flags & HFS_OVL_USED) ||
                !overlay_entry_visible(o, idx) ||
                (oe->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) ||
                (oe->parent != logical_parent &&
                 (!backing_parent || oe->parent != backing_parent)))
                continue;
            fill_overlay_fatent(oe, entry);
            scan->entry = oe->id;
            scan->entries = 1;
            return 1;
        }
        return 0;
    }

    /* v3: walk only the two parent buckets.  logical_parent covers writable
     * directories; backing_parent covers a renamed/shadowed native directory.
     * When they are equal, one pass is enough.  Phases 1/3 mean "bucket not
     * started"; 2/4 mean "in progress" so an exhausted chain (cursor NONE)
     * is not mistaken for an unstarted bucket. */
    while (scan->hfs_phase >= 1 && scan->hfs_phase <= 4) {
        bool logical = scan->hfs_phase <= 2;
        uint32_t p = logical ? logical_parent : backing_parent;
        if (!logical &&
            (!backing_parent || backing_parent == logical_parent)) {
            scan->hfs_phase = 5;
            break;
        }
        if (scan->hfs_phase == 1 || scan->hfs_phase == 3) {
            scan->hfs_overlay_index = (uint16_t)overlay_parent_first(o, p);
            scan->hfs_phase = logical ? 2 : 4;
        }
        while (scan->hfs_overlay_index != HFS_OVL_NONE) {
            unsigned int idx = scan->hfs_overlay_index;
            struct hfs_overlay_entry *oe;
            scan->hfs_overlay_index =
                (uint16_t)overlay_parent_next(o, idx);
            if (idx >= limit)
                continue;
            oe = &o->entries[idx];
            if (!(oe->flags & HFS_OVL_USED) ||
                !overlay_entry_visible(o, idx) ||
                (oe->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) ||
                oe->parent != p)
                continue;
            fill_overlay_fatent(oe, entry);
            scan->entry = oe->id;
            scan->entries = 1;
            return 1;
        }
        scan->hfs_phase = logical ? 3 : 5;
        scan->hfs_overlay_index = HFS_OVL_NONE;
    }
    return 0;
}

/* A shadowed native directory has both a logical overlay parent and a native
 * backing CNID; an overlay-only directory has no backing parent. */
static void namespace_parents(const struct fat_file *dir,
                              uint32_t *logical_parent,
                              uint32_t *backing_parent)
{
    *logical_parent = (uint32_t)dir->firstcluster;
    *backing_parent = dir->hfs_overlay && dir->hfs_backing_id ?
                      dir->hfs_backing_id : *logical_parent;
}

static int namespace_readdir_locked(struct hfs_state *s,
                                    struct fat_filestr *stream,
                                    struct fat_dirscan_info *scan,
                                    struct fat_direntry *entry)
{
    struct hfs_ro_entry e;
    int rc = 0;
    uint32_t logical_parent, backing_parent;
    namespace_parents(stream->fatfilep, &logical_parent, &backing_parent);
    if (scan->entry == FAT_DIRSCAN_RW_VAL) {
        /* Overlay-only directories have no native catalog CNID.  A renamed or
         * shadowed base directory does, through hfs_backing_id. */
        bool has_base_directory = !stream->fatfilep->hfs_overlay ||
                                  stream->fatfilep->hfs_backing_id != 0;
        scan->hfs_phase = has_base_directory ? 0 : 1;
        scan->hfs_overlay_index = HFS_OVL_NONE;
        if (!scan->hfs_phase) {
            rc = hfs_ro_iter_init(&s->volume, backing_parent,
                                  &scan->hfs_iterator);
            if (rc)
                goto out;
        }
    }

    if (scan->hfs_phase == 0) {
        while ((rc = hfs_ro_iter_next(&s->volume, &scan->hfs_iterator, &e)) > 0) {
            if (e.unsupported || strlen(e.name) > FAT_DIRENTRY_NAME_MAX ||
                e.data.size > FAT_MAX_FILE_SIZE)
                continue;
            if (logical_parent == HFS_RO_ROOT_ID && !strcmp(e.name, HFS_OVL_NAME))
                continue;
            if (s->overlay.ready && overlay_hides_base(&s->overlay,
                    logical_parent, backing_parent, e.id, e.name,
                    !!(s->volume.attributes & (1u << 12))))
                continue;
            fat_empty_fat_direntry(entry);
            strlcpy((char *)entry->name, e.name, sizeof(entry->name));
            entry->attr = e.kind == 1 ? ATTR_DIRECTORY : ATTR_ARCHIVE;
            entry->filesize = (uint32_t)e.data.size;
            entry->firstcluster = (int32_t)e.id;
            set_fat_time(entry, hfs_time_to_unix(e.modified ? e.modified :
                                                 e.created));
            scan->entry = e.id;
            scan->entries = 1;
            goto out;
        }
        if (rc < 0)
            goto out;
        scan->hfs_phase = 1;
        scan->hfs_overlay_index = HFS_OVL_NONE;
    }

    if (s->overlay.ready)
        rc = overlay_readdir_next(&s->overlay, logical_parent, backing_parent,
                                  scan, entry);
    else
        rc = 0;
    if (rc > 0)
        goto out;
    rc = 0;
out:
    if (rc <= 0) {
        fat_empty_fat_direntry(entry);
        scan->entries = 0;
    }
    return rc;
}

int hfsplus_readdir(struct fat_filestr *stream, struct fat_dirscan_info *scan,
                    struct fat_direntry *entry)
{
    if (!hfs_valid_stream(stream) || !scan || !entry)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &hfs_states[IF_MV_VOL(stream->fatfilep->volume)];
    mutex_lock(&s->mutex);
    int rc = namespace_readdir_locked(s, stream, scan, entry);
    mutex_unlock(&s->mutex);
    return rc;
}

/* Look up one name in a directory.  The readdir walk remains the canonical
 * source of truth; this cache only avoids repeating it for the same parent,
 * name and backing directory.  A generation bump after any persisted overlay
 * entry makes every cached result stale. */
int hfsplus_find(struct fat_filestr *stream, const char *name,
                 struct fat_direntry *entry)
{
    if (!hfs_valid_stream(stream) || !name || !entry)
        return HFS_RO_ARGUMENT;

    struct hfs_state *s = &hfs_states[IF_MV_VOL(stream->fatfilep->volume)];
    uint32_t logical_parent, backing_parent;
    namespace_parents(stream->fatfilep, &logical_parent, &backing_parent);

    mutex_lock(&s->mutex);
    for (unsigned int i = 0; i < HFS_NAME_CACHE_SLOTS; ++i) {
        struct hfs_name_cache_entry *ce = &s->name_cache[i];
        if (ce->generation == s->name_cache_gen &&
            ce->parent == logical_parent &&
            ce->backing_parent == backing_parent &&
            !strcmp(ce->name, name)) {
            if (ce->found)
                *entry = ce->entry;
            mutex_unlock(&s->mutex);
            return ce->found;
        }
    }

    struct fat_dirscan_info scan;
    memset(&scan, 0, sizeof(scan));
    scan.entry = FAT_DIRSCAN_RW_VAL;
    int rc;
    while ((rc = namespace_readdir_locked(s, stream, &scan, entry)) > 0) {
        if (strcasecmp((const char *)entry->name, name))
            continue;

        struct hfs_name_cache_entry *ce =
            &s->name_cache[s->name_cache_next++ % HFS_NAME_CACHE_SLOTS];
        size_t len = strlen((const char *)entry->name);
        if (len <= FAT_DIRENTRY_NAME_MAX) {
            ce->generation = s->name_cache_gen;
            ce->parent = logical_parent;
            ce->backing_parent = backing_parent;
            ce->found = 1;
            memcpy(ce->name, entry->name, len + 1);
            ce->entry = *entry;
        }
        mutex_unlock(&s->mutex);
        return 1;
    }

    if (rc == 0 && strlen(name) <= FAT_DIRENTRY_NAME_MAX) {
        /* Negative caching matters for the many file_exists() probes that
         * precede external art and optional config files. */
        struct hfs_name_cache_entry *ce =
            &s->name_cache[s->name_cache_next++ % HFS_NAME_CACHE_SLOTS];
        ce->generation = s->name_cache_gen;
        ce->parent = logical_parent;
        ce->backing_parent = backing_parent;
        ce->found = 0;
        strcpy(ce->name, name);
    }

    mutex_unlock(&s->mutex);
    return rc < 0 ? rc : 0;
}
