/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HFSPLUS_PARTITION_H
#define HFSPLUS_PARTITION_H
#include "hfsplus_ro.h"

struct hfs_partition {
    uint64_t offset;
    uint64_t length;
    uint32_t unit;
    uint8_t type;
};

int hfs_partition_scan(hfs_ro_read_fn read, void *context, uint64_t disk_size,
                       uint32_t sector_size, struct hfs_partition *parts,
                       unsigned int capacity);
#endif
