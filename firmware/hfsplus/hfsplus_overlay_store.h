/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_OVERLAY_STORE_H
#define ROCKPOD_HFSPLUS_OVERLAY_STORE_H

#include <stdint.h>
#include "hfsplus_state.h"

void overlay_rebuild_indexes(struct hfs_overlay *o);
int overlay_find_id(const struct hfs_overlay *o, uint32_t id);
void overlay_destroy_entry(struct hfs_overlay *o, unsigned int index);
int overlay_create_entry(struct hfs_state *s, uint32_t parent, const char *name,
                         uint16_t kind, uint32_t flags, uint32_t base_id,
                         uint64_t size, int *index);
int overlay_tombstone(struct hfs_state *s, uint32_t parent, uint32_t base_id,
                      const char *name);
struct hfs_overlay_entry *overlay_file_entry(struct hfs_state *s,
                                             const struct fat_file *file);
unsigned int overlay_parent_first(const struct hfs_overlay *o, uint32_t parent);
unsigned int overlay_parent_next(const struct hfs_overlay *o,
                                 unsigned int index);
void overlay_unindex_entry(struct hfs_overlay *o, unsigned int index);
void overlay_reindex_entry(struct hfs_overlay *o, unsigned int index);
void overlay_parent_move(struct hfs_overlay *o, unsigned int index,
                         uint32_t new_parent);

#endif
