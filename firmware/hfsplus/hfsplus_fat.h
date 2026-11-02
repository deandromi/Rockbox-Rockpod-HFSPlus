/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_FAT_H
#define ROCKPOD_HFSPLUS_FAT_H

#include "hfsplus_state.h"
#include "hfsplus_time.h"
#include "fs_attr.h"
#include "string-extra.h"

/* Adapter-boundary helpers shared by namespace and FAT entry points.  They are
 * header-inline so neither module depends on the other's implementation. */
/* The VFS maps only FAT_RC_ENOSPC to ENOSPC.  Convert the generic buffer
 * error at the adapter boundary so a full overlay/entry table is reported as
 * a full filesystem instead of EIO. */
static inline int hfs_file_rc(int rc)
{
    return rc == HFS_RO_BUFFER ? FAT_RC_ENOSPC : rc;
}

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
    set_fat_time(fatent, hfs_time_to_unix(e->mtime));
}

#endif
