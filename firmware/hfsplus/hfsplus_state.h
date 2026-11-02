/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-volume adapter state shared by the decomposed HFS+ modules.  This is an
 * internal header; the public adapter API stays in hfsplus_rb.h.
 */
#ifndef ROCKPOD_HFSPLUS_STATE_H
#define ROCKPOD_HFSPLUS_STATE_H

#include "config.h"
#include "hfsplus_ro.h"
#include "hfsplus_overlay.h"
#include "mutex.h"

#ifndef HFS_NUM_VOLUMES
#define HFS_NUM_VOLUMES NUM_VOLUMES
#endif

#define HFS_IO_BOUNCE_SIZE 16384u

struct hfs_io {
    int drive;
    uint32_t sector_size;
    uint64_t size;
    uint32_t reads;
    uint32_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    /* Large aligned bounce buffer lets the 512-byte iFlash ATA layer service
     * sequential HFS+ reads/writes in batches instead of one sector/call. */
    uint8_t buffer[HFS_IO_BOUNCE_SIZE] __attribute__((aligned(32)));
};

struct hfs_diag {
    int last_error;
    int index_cache;
    uint32_t flush_ops;
    int last_stage;
    int ovl_error;
    int ovl_stage;
    int ovl_slot;
    int ovl_reason;
    uint32_t ovl_id;
    uint32_t ovl_parent;
    uint32_t ovl_flags;
    uint32_t ovl_data_start;
    uint32_t ovl_data_blocks;
    uint16_t ovl_kind;
    uint16_t ovl_name_len;
};

struct hfs_state {
    struct hfs_ro_volume volume;
    struct hfs_io io;
    struct hfs_overlay overlay;
    struct mutex mutex;
    /* Set when a container is present but invalid, or when the container is
     * absent.  Read access remains available, but native in-place writes are
     * not a safe fallback for a writable-overlay firmware. */
    int force_readonly;
    struct hfs_diag diag;
    uint8_t scratch[HFS_RO_MAX_NODE_SIZE];
    uint8_t catalog_cache[HFS_RO_MAX_NODE_SIZE] __attribute__((aligned(32)));
    uint8_t index_cache[HFS_OVL_CACHE_BYTES];
};

extern struct hfs_state hfs_states[HFS_NUM_VOLUMES];

/* Invalid volumes map to volume 0 so diagnostics stay queryable. */
struct hfs_state *hfs_state_get(int volume);

#endif
