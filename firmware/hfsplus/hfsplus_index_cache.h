/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_INDEX_CACHE_H
#define ROCKPOD_HFSPLUS_INDEX_CACHE_H

#include "hfsplus_state.h"

int hfs_index_cache_load(struct hfs_state *s);
int hfs_index_cache_save(struct hfs_state *s);

#endif
