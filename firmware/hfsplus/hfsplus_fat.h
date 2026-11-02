/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_FAT_H
#define ROCKPOD_HFSPLUS_FAT_H

#include "hfsplus_rb.h"
#include "hfsplus_state.h"
#include "hfsplus_time.h"
#include "fs_attr.h"
#include "string-extra.h"
#include <string.h>

/* Adapter-boundary helpers shared by namespace and FAT entry points.  They are
 * header-inline so neither module depends on the other's implementation. */
/* The VFS maps only FAT_RC_ENOSPC to ENOSPC.  Convert the generic buffer
 * error at the adapter boundary so a full overlay/entry table is reported as
 * a full filesystem instead of EIO. */
static inline int hfs_file_rc(int rc)
{
    return rc == HFS_RO_BUFFER ? FAT_RC_ENOSPC : rc;
}

/* Per-operation diagnostics.  Counters are append-only and indexed by
 * enum hfsplus_op; an out-of-range op is ignored rather than corrupting the
 * neighbouring diagnostic fields.  The caller holds s->mutex. */
#ifdef BOOTLOADER
static inline void hfs_diag_op(struct hfs_state *s, int op)
{
    (void)s;
    (void)op;
}

static inline void hfs_diag_begin(struct hfs_state *s, int op)
{
    (void)s;
    (void)op;
}

static inline void hfs_diag_end(struct hfs_state *s)
{
    (void)s;
}
#else
static inline void hfs_diag_op(struct hfs_state *s, int op)
{
    if (op >= 0 && (unsigned int)op < HFSPLUS_OP_COUNT)
        ++s->diag.op_count[op];
}

/* Attribute flushes performed while this operation holds s->mutex.  The
 * caller must pair begin/end on every exit path. */
static inline void hfs_diag_begin(struct hfs_state *s, int op)
{
    s->diag.op_active = op;
}

static inline void hfs_diag_end(struct hfs_state *s)
{
    s->diag.op_active = -1;
}
#endif

/* Settings are written through config.cfg (+ .new/.old promotion) and the
 * legacy rockbox.cfg name.  This is a basename match at the adapter layer;
 * fixed.cfg is read-only and is not counted. */
#ifdef BOOTLOADER
static inline bool hfs_settings_name(const char *name)
{
    (void)name;
    return false;
}
#else
static inline bool hfs_settings_name(const char *name)
{
    return name && (!strcmp(name, "config.cfg") ||
                    !strcmp(name, "config.cfg.new") ||
                    !strcmp(name, "config.cfg.old") ||
                    !strcmp(name, "rockbox.cfg"));
}
#endif

static inline bool hfs_valid_volume(int volume)
{
    return (unsigned int)volume < HFS_NUM_VOLUMES;
}

static inline bool hfs_valid_file(const struct fat_file *file)
{
    return file && hfs_valid_volume(IF_MV_VOL(file->volume));
}

static inline bool hfs_valid_stream(const struct fat_filestr *stream)
{
    return stream && hfs_valid_file(stream->fatfilep);
}

static inline void fill_overlay_fatent(const struct hfs_overlay_entry *e,
                                       struct fat_direntry *fatent)
{
    fat_empty_fat_direntry(fatent);
    strlcpy((char *)fatent->name, e->name, sizeof(fatent->name));
    fatent->attr = e->kind == 1 ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    fatent->filesize = e->kind == 2 ? (uint32_t)e->size : 0;
    fatent->firstcluster = (int32_t)e->id;
    /* Overlay mtime is stored as Unix seconds; native HFS+ dates use the
     * 1904 epoch and are converted by their callers. */
    set_fat_time(fatent, (time_t)e->mtime);
}

#endif
