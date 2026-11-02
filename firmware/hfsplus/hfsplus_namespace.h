/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_NAMESPACE_H
#define ROCKPOD_HFSPLUS_NAMESPACE_H

#include <stdint.h>
#include "hfsplus_state.h"

/* Create the BASE shadow/tombstone pair before a native file is modified. */
int overlay_shadow_base(struct hfs_state *s, struct fat_file *file,
                        uint32_t new_parent, const char *new_name);

#endif
