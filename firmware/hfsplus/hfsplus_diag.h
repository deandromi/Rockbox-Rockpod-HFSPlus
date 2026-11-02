/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_DIAG_H
#define ROCKPOD_HFSPLUS_DIAG_H

#include "hfsplus_state.h"

int overlay_diag_fail(struct hfs_state *s, int stage, int error, int slot);
int overlay_diag_validation(struct hfs_state *s,
                            const struct hfs_overlay_validation *vr);

#endif
