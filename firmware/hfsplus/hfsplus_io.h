/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_IO_H
#define ROCKPOD_HFSPLUS_IO_H

#include <stdint.h>
#include <stddef.h>
#include "hfsplus_state.h"

int setup_io(struct hfs_io *io, int drive);
int device_read(void *ctx, uint64_t offset, void *buffer, size_t length);
int device_write(void *ctx, uint64_t offset, const void *buffer, size_t length);
int hfsplus_signature_at(const struct hfs_io *io, uint64_t offset);
int write_existing_inline(struct hfs_state *s,
                          const struct hfs_ro_entry *entry,
                          uint64_t offset, const void *buffer,
                          size_t length);
int write_container_inline(struct hfs_state *s, uint64_t offset,
                           const void *buffer, size_t length);

#endif
