/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HFSPLUS_RB_H
#define HFSPLUS_RB_H
#include "fat.h"
#include "disk.h"

#define HFSPLUS_NOT_HFS (-100)
void hfsplus_init(void);
bool hfsplus_mounted(int volume);
int hfsplus_mount(int volume, int drive, sector_t start, sector_t count);
void hfsplus_unmount(int volume);
int hfsplus_partitions(int drive, struct partinfo *parts, int capacity,
                       int *multiplier);
int hfsplus_last_error(void);
int hfsplus_last_stage(void); /* 1 = partition scan, 2 = HFS+ mount */
bool hfsplus_overlay_ready(int volume);
int hfsplus_overlay_last_error(void);
int hfsplus_overlay_last_stage(void);
int hfsplus_overlay_last_slot(void);
uint32_t hfsplus_overlay_diag_id(void);
uint32_t hfsplus_overlay_diag_parent(void);
uint32_t hfsplus_overlay_diag_flags(void);
uint32_t hfsplus_overlay_diag_data_start(void);
uint32_t hfsplus_overlay_diag_data_blocks(void);
unsigned int hfsplus_overlay_diag_kind(void);
unsigned int hfsplus_overlay_diag_name_len(void);
int hfsplus_sector_size(int volume);
unsigned int hfsplus_cluster_size(int volume);
bool hfsplus_size(int volume, sector_t *size, sector_t *free);
bool fat_is_readonly(const struct fat_file *file);
bool hfsplus_writable(const struct fat_file *file);
int hfsplus_create_file(struct fat_file *parent, const char *name,
                        uint8_t attr, struct fat_file *file,
                        struct fat_direntry *fatent);
int hfsplus_remove(struct fat_file *file, enum fat_remove_op what);
int hfsplus_rename(struct fat_file *parent, struct fat_file *file,
                   const unsigned char *newname);
int hfsplus_modtime(struct fat_file *parent, struct fat_file *file,
                    time_t modtime);
int hfsplus_truncate(const struct fat_filestr *filestr);
bool hfsplus_can_write_existing(const struct fat_file *file);
int hfsplus_closewrite(struct fat_filestr *filestr, uint32_t size,
                       struct fat_direntry *fatentp);
int hfsplus_open_root(int volume, struct fat_file *file);
int hfsplus_open(const struct fat_file *parent, long id, struct fat_file *file);
int hfsplus_readdir(struct fat_filestr *stream, struct fat_dirscan_info *scan,
                    struct fat_direntry *entry);
long hfsplus_readwrite(struct fat_filestr *stream, unsigned long sectors,
                       void *buffer, bool write);
int hfsplus_seek(struct fat_filestr *stream, unsigned long sector);
#endif
