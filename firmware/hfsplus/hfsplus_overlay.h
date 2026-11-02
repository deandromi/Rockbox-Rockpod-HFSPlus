/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal overlay layout shared by the adapter and the validator.  This is
 * not a public firmware header; keep it out of apps/.
 */
#ifndef ROCKPOD_HFSPLUS_OVERLAY_H
#define ROCKPOD_HFSPLUS_OVERLAY_H

#include <stdint.h>
#include <string.h>
#include "hfsplus_ro.h"
#include "hfsplus_rb.h"

/* v11 writable overlay.
 *
 * Native HFS+ metadata stays untouched.  Rockpod stores files that it creates,
 * grows, truncates, renames or removes inside a preallocated HFS+ container
 * named /.rockpod-rw.  The base HFS+ namespace and this overlay are merged by
 * the adapter.  This keeps Apple's catalog/allocation/journal structures valid
 * while giving Rockbox normal writable filesystem semantics. */
#define HFS_OVL_NAME              ".rockpod-rw"
#define HFS_OVL_MAGIC             "RPOVL12"
#define HFS_OVL_MAGIC_V1          "RPOVL11"
#define HFS_OVL_MAGIC_V3          "RPOVL13"
#define HFS_OVL_VERSION           2u
#define HFS_OVL_VERSION_V1        1u
#define HFS_OVL_VERSION_3         3u
#define HFS_OVL_BLOCK             4096u
#define HFS_OVL_ENTRY_SIZE        320u
#define HFS_OVL_MAX_ENTRIES       2048u
#define HFS_OVL_READ_CHUNK        96u
#define HFS_OVL_MAX_BLOCKS        524288u /* 2 GiB at 4 KiB/block */
#define HFS_OVL_BITMAP_MAX        (HFS_OVL_MAX_BLOCKS / 8u)
#define HFS_OVL_ID_BASE           0x40000000u
#define HFS_OVL_NONE              0xffffu
#define HFS_OVL_ID_HASH_SIZE      4096u
#define HFS_OVL_PARENT_HASH_SIZE  2048u
#define HFS_OVL_GROW_MIN_BLOCKS   8u   /* 32 KiB minimum allocation quantum */
#define HFS_OVL_SHRINK_HYSTERESIS 64u  /* keep <=256 KiB slack to avoid churn */
#define HFS_OVL_CACHE_BLOCKS      64u  /* v3 directory/CNID index cache */
#define HFS_OVL_CACHE_BYTES       (HFS_OVL_CACHE_BLOCKS * HFS_OVL_BLOCK)
#define HFS_OVL_HIGH_WATER_CHUNK  64u  /* reserve slots per v3 header flush */
#define HFS_OVL_STATE_READY       0u
#define HFS_OVL_STATE_INIT        1u

#define HFS_OVL_USED              0x00000001u
#define HFS_OVL_DELETED           0x00000002u
#define HFS_OVL_BASE              0x00000004u
#define HFS_OVL_TOMBSTONE         0x00000008u

struct hfs_overlay_entry {
    uint32_t id;
    uint32_t parent;
    uint32_t flags;
    uint32_t base_id;
    uint64_t size;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t mtime;
    uint16_t kind;
    uint16_t name_len;
    char name[261];
};

struct hfs_overlay {
    int ready;
    int header_recovered;
    uint32_t next_id;
    uint32_t generation;
    uint32_t version;
    uint32_t bitmap_start;
    uint32_t total_blocks;
    uint32_t bitmap_bytes;
    uint32_t bitmap_blocks;
    uint32_t entry_blocks;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t alloc_hint;
    uint32_t entry_high_water;
    uint32_t cache_start;
    uint32_t cache_blocks;
    uint32_t state;
    uint16_t free_slot_hint;
    struct hfs_ro_entry container;
    uint8_t bitmap[HFS_OVL_BITMAP_MAX];
    struct hfs_overlay_entry entries[HFS_OVL_MAX_ENTRIES];
    /* RAM-only: a legacy duplicate live name is hidden, but its data range is
     * still validated and left allocated. */
    uint8_t shadowed[HFS_OVL_MAX_ENTRIES];
    /* RAM-only acceleration indexes. They are rebuilt from persistent entries
     * at mount, so they cannot corrupt the on-disk overlay. */
    uint16_t id_hash[HFS_OVL_ID_HASH_SIZE];
    uint16_t id_tombstones;
    uint16_t parent_head[HFS_OVL_PARENT_HASH_SIZE];
    uint8_t header_dirty;
    /* RAM-only: the parent chain is complete and safe for readdir.  Legacy
     * v1/v2 images and any repaired/corrupt chain fall back to a bounded
     * linear scan. */
    uint8_t parent_index_ok;
    uint16_t parent_next[HFS_OVL_MAX_ENTRIES];
};

struct hfs_overlay_validation {
    int slot;
    enum hfs_overlay_reason reason;
};

/* A legacy duplicate is hidden in RAM without deleting its entry or data.
 * All visibility checks go through this predicate. */
static inline int overlay_entry_visible(const struct hfs_overlay *o,
                                        unsigned int index)
{
    return index < HFS_OVL_MAX_ENTRIES && !o->shadowed[index];
}

/* Number of entry slots that can exist in this overlay.  v3 persists a
 * high-water bound; v1/v2 predate it and keep the full-table scan. */
static inline unsigned int overlay_entry_limit(const struct hfs_overlay *o)
{
    if (o->version == HFS_OVL_VERSION_3 &&
        o->entry_high_water <= HFS_OVL_MAX_ENTRIES)
        return o->entry_high_water;
    return HFS_OVL_MAX_ENTRIES;
}

static inline int overlay_bit(const struct hfs_overlay *o, uint32_t block)
{
    return block < o->total_blocks &&
           (o->bitmap[block >> 3] & (uint8_t)(1u << (block & 7u)));
}

/* The native HFS+ reader performs exact byte-name lookup.  Using exact
 * matching here keeps the overlay namespace consistent with it; HFS+
 * Unicode case folding is documented as unsupported. */
static inline int overlay_name_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* Validate the persistent overlay as a whole before publishing it as READY.
 * The caller owns diagnostics: on failure, apply the returned reason/slot. */
int overlay_validate(struct hfs_overlay *o,
                     struct hfs_overlay_validation *result);

#endif
