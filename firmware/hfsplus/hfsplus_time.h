/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_TIME_H
#define ROCKPOD_HFSPLUS_TIME_H

#include <stdint.h>
#include <time.h>
#include <fat.h>

uint32_t hfs_now(void);
time_t hfs_time_to_unix(uint32_t hfs_time);
void set_fat_time(struct fat_direntry *fatent, time_t when);

#endif
