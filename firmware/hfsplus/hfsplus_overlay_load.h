/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_OVERLAY_LOAD_H
#define ROCKPOD_HFSPLUS_OVERLAY_LOAD_H

#include "hfsplus_state.h"

#ifndef BOOTLOADER
int overlay_load(struct hfs_state *s);
#endif

#endif
