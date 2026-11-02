/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_OVERLAY_ALLOC_H
#define ROCKPOD_HFSPLUS_OVERLAY_ALLOC_H

#include <stdint.h>
#include "hfsplus_state.h"

void overlay_set_bit(struct hfs_overlay *o, uint32_t block, int used);
uint32_t overlay_free_blocks(const struct hfs_overlay *o);
int overlay_free_run(struct hfs_state *s, struct hfs_overlay_entry *owner,
                     uint32_t start, uint32_t blocks, int require_owner);
int overlay_materialize_base(struct hfs_state *s, struct hfs_overlay_entry *e,
                             const struct hfs_ro_fork *fork);
int overlay_ensure_capacity(struct hfs_state *s, struct hfs_overlay_entry *e,
                            uint64_t bytes, int *entry_synced);
int overlay_shrink(struct hfs_state *s, struct hfs_overlay_entry *e,
                   uint64_t bytes);

#endif
