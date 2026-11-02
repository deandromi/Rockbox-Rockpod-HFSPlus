/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_OVERLAY_FORMAT_H
#define ROCKPOD_HFSPLUS_OVERLAY_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include "hfsplus_state.h"

int overlay_container_read(struct hfs_state *s, uint64_t off,
                           void *buffer, size_t length);
int overlay_container_write(struct hfs_state *s, uint64_t off,
                            const void *buffer, size_t length);
uint64_t overlay_entry_offset(const struct hfs_overlay *o,
                              unsigned int index);
int overlay_sync_header(struct hfs_state *s);
int overlay_sync_bitmap(struct hfs_state *s);
int overlay_sync_bitmap_range(struct hfs_state *s, uint32_t start,
                              uint32_t blocks);
int overlay_decode_entry(struct hfs_overlay_entry *e, const uint8_t *b);
int overlay_sync_entry(struct hfs_state *s, unsigned int index);
int overlay_data_read(struct hfs_state *s,
                      const struct hfs_overlay_entry *e,
                      uint64_t off, void *buffer, size_t length);
int overlay_data_write(struct hfs_state *s,
                       const struct hfs_overlay_entry *e,
                       uint64_t off, const void *buffer, size_t length);
int overlay_parse_header(struct hfs_state *s, const uint8_t *b, int backup);

#endif
