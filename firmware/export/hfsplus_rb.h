/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HFSPLUS_RB_H
#define HFSPLUS_RB_H
/* Angle-bracket includes are deliberate: the host adapter harness puts stub
 * fat.h/disk.h earlier on the include path, while firmware resolves them from
 * firmware/export.  This keeps one API header instead of a host copy. */
#include <fat.h>
#include <disk.h>

#define HFSPLUS_NOT_HFS (-100)

/* Why overlay_validate() rejected a persistent overlay (stage 11).  The
 * numeric values are part of the debug/API surface; append new reasons at the
 * end and keep tools/rockpod/overlay_check.py messages in sync. */
enum hfs_overlay_reason {
    HFS_OVL_REASON_NONE = 0,
    HFS_OVL_REASON_LAYOUT,
    HFS_OVL_REASON_ENTRY_ID,
    HFS_OVL_REASON_DUPLICATE_ID,
    HFS_OVL_REASON_ENTRY_NAME,
    HFS_OVL_REASON_ENTRY_FLAGS,
    HFS_OVL_REASON_ENTRY_KIND,
    HFS_OVL_REASON_TOMBSTONE,
    HFS_OVL_REASON_BASE_ID,
    HFS_OVL_REASON_DATA_RANGE,
    HFS_OVL_REASON_DATA_SIZE,
    HFS_OVL_REASON_DATA_UNALLOCATED,
    HFS_OVL_REASON_DATA_OVERLAP,
    HFS_OVL_REASON_PARENT,
    HFS_OVL_REASON_NEXT_ID,
};

void hfsplus_init(void);
bool hfsplus_mounted(int volume);
int hfsplus_mount(int volume, int drive, sector_t start, sector_t count);
void hfsplus_usb_fix_mbr(unsigned char *mbr, unsigned int unit);
void hfsplus_unmount(int volume);
int hfsplus_partitions(int drive, struct partinfo *parts, int capacity,
                       int *multiplier);
int hfsplus_last_error(void);
int hfsplus_last_stage(void); /* 1 = partition scan, 2 = HFS+ mount */
int hfsplus_last_error_vol(int volume);
int hfsplus_last_stage_vol(int volume);
bool hfsplus_had_hfs_candidate(void);
bool hfsplus_overlay_ready(int volume);
int hfsplus_overlay_last_error(void);
int hfsplus_overlay_last_stage(void);
int hfsplus_overlay_last_slot(void);
int hfsplus_overlay_last_reason(void);
int hfsplus_index_cache_used(int volume);
int hfsplus_overlay_format_version(int volume);
unsigned int hfsplus_io_reads(int volume);
unsigned int hfsplus_io_writes(int volume);
unsigned int hfsplus_io_flushes(int volume);
const char *hfsplus_overlay_reason_name(int reason);
int hfsplus_overlay_last_error_vol(int volume);
int hfsplus_overlay_last_stage_vol(int volume);
int hfsplus_overlay_last_slot_vol(int volume);
int hfsplus_overlay_last_reason_vol(int volume);
uint32_t hfsplus_overlay_diag_id(void);
uint32_t hfsplus_overlay_diag_parent(void);
uint32_t hfsplus_overlay_diag_flags(void);
uint32_t hfsplus_overlay_diag_data_start(void);
uint32_t hfsplus_overlay_diag_data_blocks(void);
unsigned int hfsplus_overlay_diag_kind(void);
unsigned int hfsplus_overlay_diag_name_len(void);
uint32_t hfsplus_overlay_diag_id_vol(int volume);
uint32_t hfsplus_overlay_diag_parent_vol(int volume);
uint32_t hfsplus_overlay_diag_flags_vol(int volume);
uint32_t hfsplus_overlay_diag_data_start_vol(int volume);
uint32_t hfsplus_overlay_diag_data_blocks_vol(int volume);
unsigned int hfsplus_overlay_diag_kind_vol(int volume);
unsigned int hfsplus_overlay_diag_name_len_vol(int volume);
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
