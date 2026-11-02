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

/* Small read-only path cache for the adapter's name lookup hook.  Entries are
 * invalidated by generation, which is bumped whenever an overlay entry is
 * persisted; native HFS+ metadata never changes. */
#define HFS_NAME_CACHE_SLOTS 8

struct hfs_name_cache_entry {
    uint32_t generation;
    uint32_t parent;
    uint32_t backing_parent;
    uint8_t found;
    char name[FAT_DIRENTRY_NAME_MAX + 1];
    struct fat_direntry entry;
};

struct hfs_diag {
    int last_error;
    int index_cache;
    uint32_t flush_ops;
    /* Append-only counters indexed by enum hfsplus_op. */
    uint32_t op_count[HFSPLUS_OP_COUNT];
    /* Flushes attributed to the adapter operation currently holding the
     * mutex; -1 means no attribution (mount/unmount/background). */
    uint32_t flush_op_count[HFSPLUS_OP_COUNT];
    int op_active;
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
    /* BOOTLOADER keeps this member because the shared adapter modules still
     * reference it, but nothing ever loads the overlay there: overlay.ready
     * stays zero, and the 708 KiB of entries/bitmap remains untouched BSS.
     * hfsplus_init()/hfsplus_mount_overlay() no longer memset it on boot. */
    struct hfs_overlay overlay;
    struct mutex mutex;
    /* Set when a container is present but invalid, or when the container is
     * absent.  Read access remains available, but native in-place writes are
     * not a safe fallback for a writable-overlay firmware. */
    int force_readonly;
    struct hfs_diag diag;
    /* Read-only path lookup cache, keyed by the logical/backing parent and
     * the exact name.  generation invalidates all entries after a mutation. */
    uint32_t name_cache_gen;
    uint8_t name_cache_next;
    struct hfs_name_cache_entry name_cache[HFS_NAME_CACHE_SLOTS];
    uint8_t scratch[HFS_RO_MAX_NODE_SIZE];
#ifndef BOOTLOADER
    /* Firmware-only: catalog node cache and the persisted v3 index cache.
     * The bootloader never builds indexes, so omitting these saves 288 KiB
     * of bootloader BSS. */
    uint8_t catalog_cache[HFS_RO_MAX_NODE_SIZE] __attribute__((aligned(32)));
    uint8_t index_cache[HFS_OVL_CACHE_BYTES];
    /* Firmware-only second aligned buffer for 16 KiB realloc/materialize
     * copies.  s->scratch stays reserved for metadata staging. */
    uint8_t copy_buffer[HFS_IO_BOUNCE_SIZE] __attribute__((aligned(32)));
#endif
};

extern struct hfs_state hfs_states[HFS_NUM_VOLUMES];

/* Invalid volumes map to volume 0 so diagnostics stay queryable. */
struct hfs_state *hfs_state_get(int volume);

#endif
