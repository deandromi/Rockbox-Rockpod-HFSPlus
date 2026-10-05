/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCKPOD_HFSPLUS_RO_H
#define ROCKPOD_HFSPLUS_RO_H

#include <stddef.h>
#include <stdint.h>

#define HFS_RO_NAME_BYTES 1021
#define HFS_RO_MAX_NODE_SIZE 32768
#define HFS_RO_ROOT_ID 2
#define HFS_RO_DIR_INDEX_MAX 4096

enum hfs_ro_error {
    HFS_RO_OK = 0,
    HFS_RO_IO = -1,
    HFS_RO_FORMAT = -2,
    HFS_RO_UNSUPPORTED = -3,
    HFS_RO_DIRTY = -4,
    HFS_RO_NOT_FOUND = -5,
    HFS_RO_NOT_DIR = -6,
    HFS_RO_IS_DIR = -7,
    HFS_RO_ARGUMENT = -8,
    HFS_RO_BUFFER = -9
};

/* The callback must read exactly length bytes and return zero on success. */
typedef int (*hfs_ro_read_fn)(void *context, uint64_t offset,
                              void *buffer, size_t length);

struct hfs_ro_extent {
    uint32_t start;
    uint32_t count;
};

struct hfs_ro_fork {
    uint64_t size;
    uint32_t blocks;
    struct hfs_ro_extent extents[8];
};

struct hfs_ro_entry {
    uint32_t id;
    uint32_t parent;
    uint16_t kind;
    uint16_t mode;
    uint8_t unsupported;
    char name[HFS_RO_NAME_BYTES];
    struct hfs_ro_fork data;
};

struct hfs_ro_dirloc {
    uint32_t parent;
    uint32_t node;
    uint16_t record;
    uint8_t indexed;
};

struct hfs_ro_volume {
    hfs_ro_read_fn read;
    void *context;
    uint64_t base;
    uint64_t length;
    uint32_t block_size;
    uint32_t blocks;
    uint32_t free_blocks;
    uint32_t attributes;
    struct hfs_ro_fork catalog;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t total_nodes;
    uint32_t leaf_records;
    uint16_t node_size;
    uint8_t *scratch;
    size_t scratch_size;
    /* Optional dedicated catalog node cache. Rockpod supplies this so a
     * directory walk does not reread the same B-tree node for every entry. */
    uint8_t *catalog_cache;
    size_t catalog_cache_size;
    uint32_t catalog_cache_node;
    struct hfs_ro_dirloc dir_index[HFS_RO_DIR_INDEX_MAX];
    uint16_t dir_index_count;
    uint8_t dir_index_complete;
    int mounted;
};

struct hfs_ro_iterator {
    uint32_t parent;
    uint32_t node;
    uint32_t previous;
    uint32_t visited;
    uint32_t records_seen;
    uint16_t record;
    uint8_t indexed;
};

/* A volume and its scratch buffer require external serialization. */
int hfs_ro_mount(struct hfs_ro_volume *volume, hfs_ro_read_fn read,
                 void *context, uint64_t base, uint64_t length,
                 void *scratch, size_t scratch_size);
void hfs_ro_unmount(struct hfs_ro_volume *volume);
int hfs_ro_enable_fast_index(struct hfs_ro_volume *volume, void *cache, size_t cache_size);
int hfs_ro_iter_init(const struct hfs_ro_volume *volume, uint32_t parent,
                     struct hfs_ro_iterator *iterator);
/* Returns one entry, zero at EOF, or a negative hfs_ro_error. */
int hfs_ro_iter_next(struct hfs_ro_volume *volume,
                     struct hfs_ro_iterator *iterator,
                     struct hfs_ro_entry *entry);
/* Paths use exact UTF-8 spelling of the on-disk UTF-16 name. */
int hfs_ro_lookup(struct hfs_ro_volume *volume, const char *path,
                  struct hfs_ro_entry *entry);
int hfs_ro_find_id(struct hfs_ro_volume *volume, uint32_t id,
                   struct hfs_ro_entry *entry);
int hfs_ro_pread(struct hfs_ro_volume *volume,
                 const struct hfs_ro_entry *entry, uint64_t offset,
                 void *buffer, size_t length, size_t *done);
const char *hfs_ro_strerror(int error);

#endif
