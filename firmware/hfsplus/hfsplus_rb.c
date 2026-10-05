/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "hfsplus_rb.h"
#include "hfsplus_partition.h"
#include "storage.h"
#include "mutex.h"
#include "fs_attr.h"
#include "string-extra.h"
#include <limits.h>
#include <string.h>

#define HFS_IO_BOUNCE_SIZE 16384u

struct hfs_io {
    int drive;
    uint32_t sector_size;
    uint64_t size;
    /* Large aligned bounce buffer lets the 512-byte iFlash ATA layer service
     * sequential HFS+ reads/writes in batches instead of one sector/call. */
    uint8_t buffer[HFS_IO_BOUNCE_SIZE] __attribute__((aligned(32)));
};

/* v11 writable overlay.
 *
 * Native HFS+ metadata stays untouched.  Rockpod stores files that it creates,
 * grows, truncates, renames or removes inside a preallocated HFS+ container
 * named /.rockpod-rw.  The base HFS+ namespace and this overlay are merged by
 * the adapter.  This keeps Apple's catalog/allocation/journal structures valid
 * while giving Rockbox normal writable filesystem semantics. */
#define HFS_OVL_NAME              ".rockpod-rw"
#define HFS_OVL_MAGIC             "RPOVL11"
#define HFS_OVL_VERSION           1u
#define HFS_OVL_BLOCK             4096u
#define HFS_OVL_ENTRY_SIZE        320u
#define HFS_OVL_MAX_ENTRIES       2048u
#define HFS_OVL_MAX_BLOCKS        524288u /* 2 GiB at 4 KiB/block */
#define HFS_OVL_BITMAP_MAX        (HFS_OVL_MAX_BLOCKS / 8u)
#define HFS_OVL_ID_BASE           0x40000000u
#define HFS_OVL_NONE              0xffffu
#define HFS_OVL_ID_HASH_SIZE      4096u
#define HFS_OVL_PARENT_HASH_SIZE  2048u
#define HFS_OVL_GROW_MIN_BLOCKS   8u   /* 32 KiB minimum allocation quantum */
#define HFS_OVL_SHRINK_HYSTERESIS 64u  /* keep <=256 KiB slack to avoid churn */

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
    uint32_t next_id;
    uint32_t generation;
    uint32_t total_blocks;
    uint32_t bitmap_bytes;
    uint32_t bitmap_blocks;
    uint32_t entry_blocks;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t alloc_hint;
    uint16_t free_slot_hint;
    struct hfs_ro_entry container;
    uint8_t bitmap[HFS_OVL_BITMAP_MAX];
    struct hfs_overlay_entry entries[HFS_OVL_MAX_ENTRIES];
    /* RAM-only acceleration indexes. They are rebuilt from persistent entries
     * at mount, so they cannot corrupt the on-disk overlay. */
    uint16_t id_hash[HFS_OVL_ID_HASH_SIZE];
    uint16_t parent_head[HFS_OVL_PARENT_HASH_SIZE];
    uint16_t parent_next[HFS_OVL_MAX_ENTRIES];
};

static struct hfs_state {
    struct hfs_ro_volume volume;
    struct hfs_io io;
    struct hfs_overlay overlay;
    struct mutex mutex;
    uint8_t scratch[HFS_RO_MAX_NODE_SIZE];
    uint8_t catalog_cache[HFS_RO_MAX_NODE_SIZE] __attribute__((aligned(32)));
} states[NUM_VOLUMES];

static struct hfs_io scan_io;
static int last_error;
static int last_stage;
static int overlay_diag_error;
static int overlay_diag_stage;
static int overlay_diag_slot = -1;
static uint32_t overlay_diag_id;
static uint32_t overlay_diag_parent;
static uint32_t overlay_diag_flags;
static uint32_t overlay_diag_data_start;
static uint32_t overlay_diag_data_blocks;
static uint16_t overlay_diag_kind;
static uint16_t overlay_diag_name_len;

static int overlay_diag_fail(int stage, int error, int slot)
{
    overlay_diag_stage = stage;
    overlay_diag_error = error;
    overlay_diag_slot = slot;
    return error;
}

static void fill_overlay_fatent(const struct hfs_overlay_entry *e,
                                struct fat_direntry *fatent);

static int setup_io(struct hfs_io *io, int drive)
{
    struct storage_info info;
    storage_get_info(drive, &info);
    if (info.sector_size < 512 || info.sector_size > sizeof(io->buffer) ||
        (info.sector_size & (info.sector_size - 1)) ||
        info.num_sectors > UINT64_MAX / info.sector_size)
        return HFS_RO_UNSUPPORTED;
    io->drive = drive;
    io->sector_size = info.sector_size;
    io->size = (uint64_t)info.num_sectors * info.sector_size;
    return 0;
}

static int device_read(void *ctx, uint64_t offset, void *buffer, size_t length)
{
    struct hfs_io *io = ctx;
    uint8_t *out = buffer;
    if (offset > io->size || length > io->size - offset)
        return -1;

    /* Unaligned prefix. */
    if (length && offset % io->sector_size) {
        uint32_t within = offset % io->sector_size;
        size_t chunk = io->sector_size - within;
        if (chunk > length)
            chunk = length;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(out, io->buffer + within, chunk);
        out += chunk;
        offset += chunk;
        length -= chunk;
    }

    /* Bulk aligned middle. Keep DMA on the aligned bounce buffer. */
    unsigned int max_sectors = sizeof(io->buffer) / io->sector_size;
    while (length >= io->sector_size) {
        unsigned int sectors = (unsigned int)(length / io->sector_size);
        if (sectors > max_sectors)
            sectors = max_sectors;
        size_t bytes = (size_t)sectors * io->sector_size;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 sectors, io->buffer))
            return -1;
        memcpy(out, io->buffer, bytes);
        out += bytes;
        offset += bytes;
        length -= bytes;
    }

    /* Unaligned tail. */
    if (length) {
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(out, io->buffer, length);
    }
    return 0;
}

static int device_write(void *ctx, uint64_t offset, const void *buffer,
                        size_t length)
{
    struct hfs_io *io = ctx;
    const uint8_t *in = buffer;
    if (offset > io->size || length > io->size - offset)
        return -1;

    if (length && offset % io->sector_size) {
        uint32_t within = offset % io->sector_size;
        size_t chunk = io->sector_size - within;
        if (chunk > length)
            chunk = length;
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(io->buffer + within, in, chunk);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  1, io->buffer))
            return -1;
        in += chunk;
        offset += chunk;
        length -= chunk;
    }

    unsigned int max_sectors = sizeof(io->buffer) / io->sector_size;
    while (length >= io->sector_size) {
        unsigned int sectors = (unsigned int)(length / io->sector_size);
        if (sectors > max_sectors)
            sectors = max_sectors;
        size_t bytes = (size_t)sectors * io->sector_size;
        memcpy(io->buffer, in, bytes);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  sectors, io->buffer))
            return -1;
        in += bytes;
        offset += bytes;
        length -= bytes;
    }

    if (length) {
        if (storage_read_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                 1, io->buffer))
            return -1;
        memcpy(io->buffer, in, length);
        if (storage_write_sectors(IF_MD(io->drive,) offset / io->sector_size,
                                  1, io->buffer))
            return -1;
    }
    return 0;
}

static int write_existing_inline(struct hfs_state *s,
                                 const struct hfs_ro_entry *entry,
                                 uint64_t offset, const void *buffer,
                                 size_t length)
{
    if (!entry || entry->kind != 2 || entry->unsupported ||
        offset > entry->data.size || length > entry->data.size - offset)
        return HFS_RO_ARGUMENT;

    uint64_t inline_blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        inline_blocks += entry->data.extents[i].count;
    if (inline_blocks != entry->data.blocks)
        return HFS_RO_UNSUPPORTED;

    const uint8_t *in = buffer;
    for (unsigned int i = 0; i < 8 && length; ++i) {
        const struct hfs_ro_extent *e = &entry->data.extents[i];
        uint64_t bytes = (uint64_t)e->count * s->volume.block_size;
        if (offset >= bytes) {
            offset -= bytes;
            continue;
        }
        size_t chunk = length;
        if (bytes - offset < chunk)
            chunk = (size_t)(bytes - offset);
        uint64_t physical = s->volume.base +
            (uint64_t)e->start * s->volume.block_size + offset;
        if (device_write(&s->io, physical, in, chunk))
            return HFS_RO_IO;
        in += chunk;
        length -= chunk;
        offset = 0;
    }
    return length ? HFS_RO_FORMAT : HFS_RO_OK;
}


static uint16_t ovl_get16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t ovl_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t ovl_get64(const uint8_t *p)
{
    return (uint64_t)ovl_get32(p) | (uint64_t)ovl_get32(p + 4) << 32;
}

static void ovl_put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void ovl_put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void ovl_put64(uint8_t *p, uint64_t v)
{
    ovl_put32(p, (uint32_t)v);
    ovl_put32(p + 4, (uint32_t)(v >> 32));
}

static int overlay_container_read(struct hfs_state *s, uint64_t off,
                                  void *buffer, size_t length)
{
    size_t done = 0;
    int rc = hfs_ro_pread(&s->volume, &s->overlay.container, off,
                          buffer, length, &done);
    return rc ? rc : (done == length ? HFS_RO_OK : HFS_RO_FORMAT);
}

static int overlay_container_write(struct hfs_state *s, uint64_t off,
                                   const void *buffer, size_t length)
{
    return write_existing_inline(s, &s->overlay.container, off, buffer, length);
}

static uint64_t overlay_entry_offset(const struct hfs_overlay *o,
                                     unsigned int index)
{
    return (uint64_t)(1u + o->bitmap_blocks) * HFS_OVL_BLOCK +
           (uint64_t)index * HFS_OVL_ENTRY_SIZE;
}

static int overlay_bit(const struct hfs_overlay *o, uint32_t block)
{
    return block < o->total_blocks &&
           (o->bitmap[block >> 3] & (uint8_t)(1u << (block & 7u)));
}

static void overlay_set_bit(struct hfs_overlay *o, uint32_t block, int used)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (used)
        o->bitmap[block >> 3] |= mask;
    else
        o->bitmap[block >> 3] &= (uint8_t)~mask;
}

static uint32_t overlay_free_blocks(const struct hfs_overlay *o)
{
    return o->ready ? o->free_blocks : 0;
}

static int overlay_sync_header(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    uint8_t *b = s->scratch;
    memset(b, 0, HFS_OVL_BLOCK);
    memcpy(b, HFS_OVL_MAGIC, 7);
    ovl_put32(b + 8, HFS_OVL_VERSION);
    ovl_put32(b + 12, HFS_OVL_BLOCK);
    ovl_put32(b + 16, HFS_OVL_MAX_ENTRIES);
    ovl_put32(b + 20, o->total_blocks);
    ovl_put32(b + 24, o->bitmap_bytes);
    ovl_put32(b + 28, o->data_start);
    ovl_put32(b + 32, o->next_id);
    ovl_put32(b + 36, ++o->generation);
    return overlay_container_write(s, 0, b, HFS_OVL_BLOCK);
}

static int overlay_sync_bitmap(struct hfs_state *s)
{
    return overlay_container_write(s, HFS_OVL_BLOCK, s->overlay.bitmap,
                                   s->overlay.bitmap_bytes);
}

/* Persist only the device sectors of the bitmap touched by an allocation.
 * v11 rewrote the entire 16-64 KiB bitmap even for a single 4 KiB block. */
static int overlay_sync_bitmap_range(struct hfs_state *s, uint32_t start,
                                     uint32_t blocks)
{
    struct hfs_overlay *o = &s->overlay;
    if (!blocks || start >= o->total_blocks || blocks > o->total_blocks - start)
        return blocks ? HFS_RO_ARGUMENT : HFS_RO_OK;
    uint32_t first = start >> 3;
    uint32_t last = (start + blocks - 1u) >> 3;
    uint32_t unit = s->io.sector_size;
    uint32_t begin = first - first % unit;
    uint32_t end = (last + 1u + unit - 1u) / unit * unit;
    if (end > o->bitmap_bytes)
        end = o->bitmap_bytes;
    return overlay_container_write(s, HFS_OVL_BLOCK + begin,
                                   o->bitmap + begin, end - begin);
}

static void overlay_encode_entry(const struct hfs_overlay_entry *e, uint8_t *b)
{
    memset(b, 0, HFS_OVL_ENTRY_SIZE);
    ovl_put32(b, e->id);
    ovl_put32(b + 4, e->parent);
    ovl_put32(b + 8, e->flags);
    ovl_put32(b + 12, e->base_id);
    ovl_put64(b + 16, e->size);
    ovl_put32(b + 24, e->data_start);
    ovl_put32(b + 28, e->data_blocks);
    ovl_put32(b + 32, e->mtime);
    ovl_put16(b + 36, e->kind);
    ovl_put16(b + 38, e->name_len);
    if (e->name_len)
        memcpy(b + 40, e->name, e->name_len);
}

static int overlay_decode_entry(struct hfs_overlay_entry *e, const uint8_t *b)
{
    memset(e, 0, sizeof(*e));
    e->id = ovl_get32(b);
    e->parent = ovl_get32(b + 4);
    e->flags = ovl_get32(b + 8);
    e->base_id = ovl_get32(b + 12);
    e->size = ovl_get64(b + 16);
    e->data_start = ovl_get32(b + 24);
    e->data_blocks = ovl_get32(b + 28);
    e->mtime = ovl_get32(b + 32);
    e->kind = ovl_get16(b + 36);
    e->name_len = ovl_get16(b + 38);
    if (!(e->flags & HFS_OVL_USED))
        return HFS_RO_OK;
    if ((e->kind != 1 && e->kind != 2) || !e->id ||
        e->name_len > 260 || e->parent < HFS_RO_ROOT_ID ||
        e->data_start > HFS_OVL_MAX_BLOCKS ||
        e->data_blocks > HFS_OVL_MAX_BLOCKS - e->data_start)
        return HFS_RO_FORMAT;
    memcpy(e->name, b + 40, e->name_len);
    e->name[e->name_len] = 0;
    return HFS_RO_OK;
}

static int overlay_sync_entry(struct hfs_state *s, unsigned int index)
{
    uint8_t raw[HFS_OVL_ENTRY_SIZE];
    if (index >= HFS_OVL_MAX_ENTRIES)
        return HFS_RO_ARGUMENT;
    overlay_encode_entry(&s->overlay.entries[index], raw);
    return overlay_container_write(s, overlay_entry_offset(&s->overlay, index),
                                   raw, sizeof(raw));
}

static unsigned int overlay_id_bucket(uint32_t id)
{
    return (id * 2654435761u) & (HFS_OVL_ID_HASH_SIZE - 1u);
}

static unsigned int overlay_parent_bucket(uint32_t parent)
{
    parent ^= parent >> 16;
    return (parent * 2654435761u) & (HFS_OVL_PARENT_HASH_SIZE - 1u);
}

static void overlay_id_insert(struct hfs_overlay *o, unsigned int index)
{
    uint32_t id = o->entries[index].id;
    unsigned int b = overlay_id_bucket(id);
    unsigned int tomb = HFS_OVL_ID_HASH_SIZE;
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE) {
            if (tomb != HFS_OVL_ID_HASH_SIZE)
                b = tomb;
            o->id_hash[b] = (uint16_t)index;
            return;
        }
        if (v == HFS_OVL_NONE - 1u && tomb == HFS_OVL_ID_HASH_SIZE)
            tomb = b;
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
}

static void overlay_id_remove(struct hfs_overlay *o, uint32_t id)
{
    unsigned int b = overlay_id_bucket(id);
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE)
            return;
        if (v < HFS_OVL_MAX_ENTRIES && o->entries[v].id == id) {
            o->id_hash[b] = HFS_OVL_NONE - 1u; /* probe-chain tombstone */
            return;
        }
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
}

static void overlay_parent_insert(struct hfs_overlay *o, unsigned int index)
{
    unsigned int b = overlay_parent_bucket(o->entries[index].parent);
    o->parent_next[index] = o->parent_head[b];
    o->parent_head[b] = (uint16_t)index;
}

static void overlay_parent_remove(struct hfs_overlay *o, unsigned int index,
                                  uint32_t parent)
{
    unsigned int b = overlay_parent_bucket(parent);
    uint16_t *link = &o->parent_head[b];
    while (*link != HFS_OVL_NONE) {
        if (*link == index) {
            *link = o->parent_next[index];
            o->parent_next[index] = HFS_OVL_NONE;
            return;
        }
        if (*link >= HFS_OVL_MAX_ENTRIES)
            return;
        link = &o->parent_next[*link];
    }
}

static void overlay_rebuild_indexes(struct hfs_overlay *o)
{
    memset(o->id_hash, 0xff, sizeof(o->id_hash));
    memset(o->parent_head, 0xff, sizeof(o->parent_head));
    memset(o->parent_next, 0xff, sizeof(o->parent_next));
    o->free_slot_hint = HFS_OVL_NONE;
    o->free_blocks = 0;
    o->alloc_hint = o->data_start;
    bool have_hint = false;
    uint32_t max_id = HFS_OVL_ID_BASE - 1u;
    for (uint32_t b = o->data_start; b < o->total_blocks; ++b) {
        if (!overlay_bit(o, b)) {
            ++o->free_blocks;
            if (!have_hint) {
                o->alloc_hint = b;
                have_hint = true;
            }
        }
    }
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        if (!(o->entries[i].flags & HFS_OVL_USED)) {
            if (o->free_slot_hint == HFS_OVL_NONE)
                o->free_slot_hint = (uint16_t)i;
            continue;
        }
        if (o->entries[i].id > max_id)
            max_id = o->entries[i].id;
        overlay_id_insert(o, i);
        overlay_parent_insert(o, i);
    }
    if (o->next_id <= max_id)
        o->next_id = max_id == 0x7fffffffu ? HFS_OVL_ID_BASE : max_id + 1u;
}

static int overlay_find_free_slot(struct hfs_overlay *o)
{
    unsigned int start = o->free_slot_hint < HFS_OVL_MAX_ENTRIES ?
                         o->free_slot_hint : 0;
    for (unsigned int n = 0; n < HFS_OVL_MAX_ENTRIES; ++n) {
        unsigned int i = (start + n) % HFS_OVL_MAX_ENTRIES;
        if (!(o->entries[i].flags & HFS_OVL_USED)) {
            o->free_slot_hint = (uint16_t)((i + 1u) % HFS_OVL_MAX_ENTRIES);
            return (int)i;
        }
    }
    o->free_slot_hint = HFS_OVL_NONE;
    return -1;
}

static int overlay_find_id(const struct hfs_overlay *o, uint32_t id)
{
    unsigned int b = overlay_id_bucket(id);
    for (unsigned int n = 0; n < HFS_OVL_ID_HASH_SIZE; ++n) {
        uint16_t v = o->id_hash[b];
        if (v == HFS_OVL_NONE)
            return -1;
        if (v < HFS_OVL_MAX_ENTRIES &&
            (o->entries[v].flags & HFS_OVL_USED) && o->entries[v].id == id)
            return (int)v;
        b = (b + 1u) & (HFS_OVL_ID_HASH_SIZE - 1u);
    }
    return -1;
}

static int overlay_hides_base(const struct hfs_overlay *o, uint32_t parent,
                              uint32_t base_id, const char *name)
{
    /* Correctness first: overlay namespace lookups are rare compared with
     * native HFS+ catalog reads, and there are at most 2048 overlay entries.
     * A linear walk is deterministic even if the optional RAM parent hash is
     * stale or damaged, which is especially important for database rebuilds
     * that repeatedly delete and recreate files in /.rockbox. */
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        const struct hfs_overlay_entry *e = &o->entries[i];
        if (!(e->flags & HFS_OVL_USED) || e->parent != parent ||
            e->base_id != base_id)
            continue;
        if ((e->flags & HFS_OVL_TOMBSTONE) ||
            (!(e->flags & HFS_OVL_DELETED) && !strcasecmp(e->name, name)))
            return 1;
    }
    return 0;
}

static int overlay_alloc_run(struct hfs_state *s, uint32_t blocks,
                             uint32_t *start)
{
    struct hfs_overlay *o = &s->overlay;
    if (!blocks || blocks > o->free_blocks ||
        blocks > o->total_blocks - o->data_start)
        return HFS_RO_BUFFER;

    uint32_t begin = o->alloc_hint;
    if (begin < o->data_start || begin >= o->total_blocks)
        begin = o->data_start;

    for (unsigned int pass = 0; pass < 2; ++pass) {
        uint32_t from = pass ? o->data_start : begin;
        uint32_t to = pass ? begin : o->total_blocks;
        uint32_t run = 0;
        for (uint32_t b = from; b < to; ++b) {
            if (!overlay_bit(o, b)) {
                if (++run == blocks) {
                    uint32_t first = b + 1u - run;
                    for (uint32_t j = 0; j < blocks; ++j)
                        overlay_set_bit(o, first + j, 1);
                    int rc = overlay_sync_bitmap_range(s, first, blocks);
                    if (rc) {
                        for (uint32_t j = 0; j < blocks; ++j)
                            overlay_set_bit(o, first + j, 0);
                        return rc;
                    }
                    o->free_blocks -= blocks;
                    o->alloc_hint = first + blocks;
                    if (o->alloc_hint >= o->total_blocks)
                        o->alloc_hint = o->data_start;
                    *start = first;
                    return HFS_RO_OK;
                }
            } else {
                run = 0;
            }
        }
    }
    return HFS_RO_BUFFER;
}

static int overlay_free_run(struct hfs_state *s, uint32_t start, uint32_t blocks)
{
    struct hfs_overlay *o = &s->overlay;
    if (!blocks)
        return HFS_RO_OK;
    if (start < o->data_start || start >= o->total_blocks ||
        blocks > o->total_blocks - start)
        return HFS_RO_FORMAT;
    for (uint32_t i = 0; i < blocks; ++i)
        overlay_set_bit(o, start + i, 0);
    int rc = overlay_sync_bitmap_range(s, start, blocks);
    if (rc) {
        for (uint32_t i = 0; i < blocks; ++i)
            overlay_set_bit(o, start + i, 1);
        return rc;
    }
    o->free_blocks += blocks;
    if (start < o->alloc_hint)
        o->alloc_hint = start;
    return HFS_RO_OK;
}

static int overlay_data_read(struct hfs_state *s,
                             const struct hfs_overlay_entry *e,
                             uint64_t off, void *buffer, size_t length)
{
    if (off > e->size || length > e->size - off)
        return HFS_RO_FORMAT;
    if (!length)
        return HFS_RO_OK;
    if (!e->data_blocks || e->data_start < s->overlay.data_start)
        return HFS_RO_FORMAT;
    return overlay_container_read(s,
        (uint64_t)e->data_start * HFS_OVL_BLOCK + off, buffer, length);
}

static int overlay_data_write(struct hfs_state *s,
                              const struct hfs_overlay_entry *e,
                              uint64_t off, const void *buffer, size_t length)
{
    uint64_t capacity = (uint64_t)e->data_blocks * HFS_OVL_BLOCK;
    if (off > capacity || length > capacity - off)
        return HFS_RO_FORMAT;
    if (!length)
        return HFS_RO_OK;
    if (!e->data_blocks || e->data_start < s->overlay.data_start ||
        e->data_start >= s->overlay.total_blocks ||
        e->data_blocks > s->overlay.total_blocks - e->data_start)
        return HFS_RO_FORMAT;
    return overlay_container_write(s,
        (uint64_t)e->data_start * HFS_OVL_BLOCK + off, buffer, length);
}

static int overlay_try_extend(struct hfs_state *s, struct hfs_overlay_entry *e,
                              uint32_t needed)
{
    struct hfs_overlay *o = &s->overlay;
    if (needed <= e->data_blocks)
        return HFS_RO_OK;
    if (!e->data_blocks)
        return HFS_RO_BUFFER;
    uint32_t extra = needed - e->data_blocks;
    uint32_t end = e->data_start + e->data_blocks;
    if (extra > o->free_blocks || end > o->total_blocks ||
        extra > o->total_blocks - end)
        return HFS_RO_BUFFER;
    for (uint32_t i = 0; i < extra; ++i)
        if (overlay_bit(o, end + i))
            return HFS_RO_BUFFER;
    for (uint32_t i = 0; i < extra; ++i)
        overlay_set_bit(o, end + i, 1);
    int rc = overlay_sync_bitmap_range(s, end, extra);
    if (rc) {
        for (uint32_t i = 0; i < extra; ++i)
            overlay_set_bit(o, end + i, 0);
        return rc;
    }
    o->free_blocks -= extra;
    e->data_blocks = needed;
    o->alloc_hint = end + extra;
    if (o->alloc_hint >= o->total_blocks)
        o->alloc_hint = o->data_start;
    return HFS_RO_OK;
}

static int overlay_realloc(struct hfs_state *s, struct hfs_overlay_entry *e,
                           uint32_t needed)
{
    if (needed <= e->data_blocks)
        return HFS_RO_OK;
    if (e->data_blocks) {
        int erc = overlay_try_extend(s, e, needed);
        if (!erc)
            return HFS_RO_OK;
        if (erc != HFS_RO_BUFFER)
            return erc;
    }

    uint32_t newstart;
    int rc = overlay_alloc_run(s, needed, &newstart);
    if (rc)
        return rc;
    uint32_t oldstart = e->data_start, oldblocks = e->data_blocks;
    uint64_t copied = 0;
    while (copied < e->size && oldblocks) {
        size_t chunk = HFS_OVL_BLOCK;
        if (e->size - copied < chunk)
            chunk = (size_t)(e->size - copied);
        rc = overlay_container_read(s,
            (uint64_t)oldstart * HFS_OVL_BLOCK + copied, s->scratch, chunk);
        if (rc)
            break;
        rc = overlay_container_write(s,
            (uint64_t)newstart * HFS_OVL_BLOCK + copied, s->scratch, chunk);
        if (rc)
            break;
        copied += chunk;
    }
    if (rc) {
        overlay_free_run(s, newstart, needed);
        return rc;
    }
    e->data_start = newstart;
    e->data_blocks = needed;
    if (oldblocks)
        return overlay_free_run(s, oldstart, oldblocks);
    return HFS_RO_OK;
}

static uint32_t overlay_growth_target(const struct hfs_overlay *o,
                                      uint32_t current, uint32_t needed);

static int overlay_materialize_base(struct hfs_state *s,
                                    struct hfs_overlay_entry *e,
                                    const struct hfs_ro_fork *fork)
{
    if (!(e->flags & HFS_OVL_BASE))
        return HFS_RO_OK;
    uint32_t needed = (uint32_t)((e->size + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    uint32_t capacity = overlay_growth_target(&s->overlay, 0, needed);
    if (needed) {
        uint32_t start;
        int rc = overlay_alloc_run(s, capacity, &start);
        if (rc)
            return rc;
        struct hfs_ro_entry src;
        memset(&src, 0, sizeof(src));
        src.kind = 2;
        src.data = *fork;
        uint64_t copied = 0;
        while (copied < e->size) {
            size_t chunk = HFS_OVL_BLOCK, done = 0;
            if (e->size - copied < chunk)
                chunk = (size_t)(e->size - copied);
            rc = hfs_ro_pread(&s->volume, &src, copied, s->scratch, chunk, &done);
            if (rc || done != chunk) {
                overlay_free_run(s, start, capacity);
                return rc ? rc : HFS_RO_IO;
            }
            rc = overlay_container_write(s,
                (uint64_t)start * HFS_OVL_BLOCK + copied, s->scratch, chunk);
            if (rc) {
                overlay_free_run(s, start, capacity);
                return rc;
            }
            copied += chunk;
        }
        e->data_start = start;
        e->data_blocks = capacity;
    }
    e->flags &= ~HFS_OVL_BASE;
    return HFS_RO_OK;
}

static uint32_t overlay_growth_target(const struct hfs_overlay *o,
                                      uint32_t current, uint32_t needed)
{
    if (needed <= current)
        return current;
    uint32_t target = current;
    if (!target)
        target = HFS_OVL_GROW_MIN_BLOCKS;
    while (target < needed) {
        uint32_t grow = target / 2u;
        if (grow < HFS_OVL_GROW_MIN_BLOCKS)
            grow = HFS_OVL_GROW_MIN_BLOCKS;
        if (target > o->total_blocks - grow) {
            target = needed;
            break;
        }
        target += grow;
    }
    if (target < needed)
        target = needed;
    return target;
}

static int overlay_ensure_capacity(struct hfs_state *s,
                                   struct hfs_overlay_entry *e,
                                   uint64_t bytes)
{
    if (bytes > (uint64_t)(s->overlay.total_blocks - s->overlay.data_start) *
                HFS_OVL_BLOCK)
        return HFS_RO_BUFFER;
    uint32_t needed = (uint32_t)((bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    if (!needed || needed <= e->data_blocks)
        return HFS_RO_OK;
    uint32_t target = overlay_growth_target(&s->overlay, e->data_blocks, needed);
    int rc = overlay_realloc(s, e, target);
    if (rc == HFS_RO_BUFFER && target != needed)
        rc = overlay_realloc(s, e, needed);
    return rc;
}

static int overlay_shrink(struct hfs_state *s, struct hfs_overlay_entry *e,
                          uint64_t bytes)
{
    uint32_t needed = (uint32_t)((bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK);
    if ((e->flags & HFS_OVL_BASE) || needed >= e->data_blocks)
        return HFS_RO_OK;
    uint32_t old = e->data_blocks;
    if (!needed) {
        int rc = overlay_free_run(s, e->data_start, old);
        if (rc)
            return rc;
        e->data_start = 0;
        e->data_blocks = 0;
        return HFS_RO_OK;
    }

    /* Keep moderate slack. Database and PictureFlow files are commonly opened,
     * grown and closed repeatedly; trimming them to the exact last block made
     * v11 allocate again on the very next append. */
    if (old - needed <= HFS_OVL_SHRINK_HYSTERESIS || needed * 2u >= old)
        return HFS_RO_OK;
    uint32_t target = overlay_growth_target(&s->overlay, 0, needed);
    if (target > old)
        target = needed;
    int rc = overlay_free_run(s, e->data_start + target, old - target);
    if (!rc)
        e->data_blocks = target;
    return rc;
}

static int overlay_new_id(struct hfs_state *s, uint32_t *id)
{
    struct hfs_overlay *o = &s->overlay;
    if (o->next_id < HFS_OVL_ID_BASE || o->next_id == 0x7fffffffu)
        o->next_id = HFS_OVL_ID_BASE;
    *id = o->next_id++;
    /* next_id is reconstructed from committed entries on mount. Avoid a 4 KiB
     * header rewrite for every tiny cfg/database file creation. */
    return HFS_RO_OK;
}

static int overlay_create_entry(struct hfs_state *s, uint32_t parent,
                                const char *name, uint16_t kind,
                                uint32_t flags, uint32_t base_id,
                                uint64_t size, int *index)
{
    size_t n = strlen(name);
    if (!s->overlay.ready || !n || n > 260 || (kind != 1 && kind != 2))
        return HFS_RO_ARGUMENT;
    int slot = overlay_find_free_slot(&s->overlay);
    if (slot < 0)
        return HFS_RO_BUFFER;
    struct hfs_overlay_entry *e = &s->overlay.entries[slot];
    memset(e, 0, sizeof(*e));
    int rc = overlay_new_id(s, &e->id);
    if (rc)
        return rc;
    e->parent = parent;
    e->flags = HFS_OVL_USED | flags;
    e->base_id = base_id;
    e->size = size;
    e->kind = kind;
    e->name_len = (uint16_t)n;
    memcpy(e->name, name, n + 1);
    rc = overlay_sync_entry(s, (unsigned int)slot);
    if (rc) {
        memset(e, 0, sizeof(*e));
    } else {
        overlay_id_insert(&s->overlay, (unsigned int)slot);
        overlay_parent_insert(&s->overlay, (unsigned int)slot);
        if (index)
            *index = slot;
    }
    return rc;
}

static int overlay_tombstone(struct hfs_state *s, uint32_t parent,
                             uint32_t base_id, const char *name)
{
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        struct hfs_overlay_entry *e = &s->overlay.entries[i];
        if ((e->flags & (HFS_OVL_USED | HFS_OVL_TOMBSTONE)) ==
                (HFS_OVL_USED | HFS_OVL_TOMBSTONE) &&
            e->parent == parent && e->base_id == base_id &&
            !strcasecmp(e->name, name))
            return HFS_RO_OK;
    }
    int idx;
    return overlay_create_entry(s, parent, name, 2, HFS_OVL_TOMBSTONE,
                                base_id, 0, &idx);
}

static int overlay_load(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    overlay_diag_error = 0;
    overlay_diag_stage = 1; /* lookup /.rockpod-rw */
    overlay_diag_slot = -1;
    memset(o, 0, sizeof(*o));

    struct hfs_ro_entry container;
    int rc = hfs_ro_lookup(&s->volume, "/" HFS_OVL_NAME, &container);
    if (rc)
        return overlay_diag_fail(1, rc, -1);

    overlay_diag_stage = 2; /* validate container/extents */
    uint64_t inline_blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        inline_blocks += container.data.extents[i].count;
    if (container.kind != 2 || container.unsupported ||
        inline_blocks != container.data.blocks ||
        container.data.size < 16u * 1024u * 1024u)
        return overlay_diag_fail(2, HFS_RO_UNSUPPORTED, -1);

    o->container = container;
    o->total_blocks = (uint32_t)(container.data.size / HFS_OVL_BLOCK);
    if (o->total_blocks > HFS_OVL_MAX_BLOCKS)
        o->total_blocks = HFS_OVL_MAX_BLOCKS;
    o->bitmap_bytes = (o->total_blocks + 7u) / 8u;
    o->bitmap_blocks = (o->bitmap_bytes + HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK;
    o->entry_blocks = (HFS_OVL_MAX_ENTRIES * HFS_OVL_ENTRY_SIZE +
                       HFS_OVL_BLOCK - 1) / HFS_OVL_BLOCK;
    o->data_start = 1u + o->bitmap_blocks + o->entry_blocks;
    if (o->data_start + 16u >= o->total_blocks)
        return overlay_diag_fail(2, HFS_RO_BUFFER, -1);

    overlay_diag_stage = 3; /* read overlay header */
    rc = overlay_container_read(s, 0, s->scratch, HFS_OVL_BLOCK);
    if (rc)
        return overlay_diag_fail(3, rc, -1);

    if (memcmp(s->scratch, HFS_OVL_MAGIC, 7)) {
        overlay_diag_stage = 10; /* initialise fresh overlay */
        memset(o->bitmap, 0, sizeof(o->bitmap));
        memset(o->entries, 0, sizeof(o->entries));
        for (uint32_t b = 0; b < o->data_start; ++b)
            overlay_set_bit(o, b, 1);
        memset(s->scratch, 0, sizeof(s->scratch));
        uint64_t left = (uint64_t)o->entry_blocks * HFS_OVL_BLOCK;
        uint64_t off = (uint64_t)(1u + o->bitmap_blocks) * HFS_OVL_BLOCK;
        while (left) {
            size_t chunk = left > sizeof(s->scratch) ? sizeof(s->scratch) : (size_t)left;
            rc = overlay_container_write(s, off, s->scratch, chunk);
            if (rc)
                return overlay_diag_fail(10, rc, -1);
            off += chunk;
            left -= chunk;
        }
        o->next_id = HFS_OVL_ID_BASE;
        o->generation = 0;
        rc = overlay_sync_bitmap(s);
        if (rc)
            return overlay_diag_fail(10, rc, -1);
        overlay_rebuild_indexes(o);
        o->ready = 1;
        rc = overlay_sync_header(s);
        if (rc) {
            o->ready = 0;
            return overlay_diag_fail(10, rc, -1);
        }
        overlay_diag_stage = 9;
        return HFS_RO_OK;
    }

    overlay_diag_stage = 4; /* validate overlay header */
    if (ovl_get32(s->scratch + 8) != HFS_OVL_VERSION ||
        ovl_get32(s->scratch + 12) != HFS_OVL_BLOCK ||
        ovl_get32(s->scratch + 16) != HFS_OVL_MAX_ENTRIES ||
        ovl_get32(s->scratch + 20) != o->total_blocks ||
        ovl_get32(s->scratch + 24) != o->bitmap_bytes ||
        ovl_get32(s->scratch + 28) != o->data_start)
        return overlay_diag_fail(4, HFS_RO_FORMAT, -1);
    o->next_id = ovl_get32(s->scratch + 32);
    o->generation = ovl_get32(s->scratch + 36);
    if (o->next_id < HFS_OVL_ID_BASE)
        return overlay_diag_fail(4, HFS_RO_FORMAT, -1);

    overlay_diag_stage = 5; /* read allocation bitmap */
    rc = overlay_container_read(s, HFS_OVL_BLOCK, o->bitmap, o->bitmap_bytes);
    if (rc)
        return overlay_diag_fail(5, rc, -1);

    overlay_diag_stage = 6; /* validate reserved bitmap blocks */
    for (uint32_t b = 0; b < o->data_start; ++b)
        if (!overlay_bit(o, b))
            return overlay_diag_fail(6, HFS_RO_FORMAT, (int)b);

    /* Read and validate each overlay table entry separately. */
    for (unsigned int i = 0; i < HFS_OVL_MAX_ENTRIES; ++i) {
        overlay_diag_stage = 7; /* read one entry */
        rc = overlay_container_read(s, overlay_entry_offset(o, i),
                                    s->scratch, HFS_OVL_ENTRY_SIZE);
        if (rc)
            return overlay_diag_fail(7, rc, (int)i);

        overlay_diag_stage = 8; /* decode/validate one entry */
        rc = overlay_decode_entry(&o->entries[i], s->scratch);
        if (rc) {
            /* Preserve exactly what the iPod decoded before the validator
             * rejected this slot. This distinguishes bad on-device reads
             * from a validator/format assumption mismatch. */
            overlay_diag_id = o->entries[i].id;
            overlay_diag_parent = o->entries[i].parent;
            overlay_diag_flags = o->entries[i].flags;
            overlay_diag_data_start = o->entries[i].data_start;
            overlay_diag_data_blocks = o->entries[i].data_blocks;
            overlay_diag_kind = o->entries[i].kind;
            overlay_diag_name_len = o->entries[i].name_len;
            return overlay_diag_fail(8, rc, (int)i);
        }
    }

    overlay_rebuild_indexes(o);
    o->ready = 1;
    overlay_diag_error = 0;
    overlay_diag_stage = 9; /* ready */
    overlay_diag_slot = -1;
    return HFS_RO_OK;
}

static int overlay_shadow_base(struct hfs_state *s, struct fat_file *file,
                               uint32_t new_parent, const char *new_name)
{
    if (!s->overlay.ready || file->hfs_overlay)
        return HFS_RO_ARGUMENT;
    struct hfs_ro_entry base;
    int rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
    if (rc)
        return rc;
    int vis;
    rc = overlay_create_entry(s, new_parent, new_name, base.kind,
                              HFS_OVL_BASE, base.id, base.data.size, &vis);
    if (rc)
        return rc;
    rc = overlay_tombstone(s, (uint32_t)file->dircluster, base.id, base.name);
    if (rc) {
        memset(&s->overlay.entries[vis], 0, sizeof(s->overlay.entries[vis]));
        overlay_sync_entry(s, (unsigned int)vis);
        return rc;
    }
    file->firstcluster = (long)s->overlay.entries[vis].id;
    file->dircluster = (long)new_parent;
    file->e.entry = s->overlay.entries[vis].id;
    file->hfs_overlay = 1;
    file->hfs_overlay_index = (uint16_t)vis;
    file->hfs_backing_id = base.id;
    file->hfs_kind = base.kind;
    file->hfs_data = base.data;
    return HFS_RO_OK;
}


void hfsplus_init(void)
{
    last_error = 0;
    last_stage = 0;
    overlay_diag_error = 0;
    overlay_diag_stage = 0;
    overlay_diag_slot = -1;
    overlay_diag_id = 0;
    overlay_diag_parent = 0;
    overlay_diag_flags = 0;
    overlay_diag_data_start = 0;
    overlay_diag_data_blocks = 0;
    overlay_diag_kind = 0;
    overlay_diag_name_len = 0;
    for (unsigned int i = 0; i < NUM_VOLUMES; ++i) {
        hfs_ro_unmount(&states[i].volume);
        memset(&states[i].overlay, 0, sizeof(states[i].overlay));
        mutex_init(&states[i].mutex);
    }
}

bool hfsplus_mounted(int volume)
{
    return (unsigned int)volume < NUM_VOLUMES && states[volume].volume.mounted;
}

bool fat_is_readonly(const struct fat_file *file)
{
    /* Historical name retained because the FAT compatibility layer uses it to
     * identify HFS+ objects.  v11 may still be writable through the overlay. */
    return file && hfsplus_mounted(IF_MV_VOL(file->volume));
}

bool hfsplus_writable(const struct fat_file *file)
{
    return file && hfsplus_mounted(IF_MV_VOL(file->volume)) &&
           states[IF_MV_VOL(file->volume)].overlay.ready;
}

bool hfsplus_can_write_existing(const struct fat_file *file)
{
    if (!file || !hfsplus_mounted(IF_MV_VOL(file->volume)) ||
        file->hfs_kind != 2)
        return false;
    if (states[IF_MV_VOL(file->volume)].overlay.ready)
        return true;
    uint64_t blocks = 0;
    for (unsigned int i = 0; i < 8; ++i)
        blocks += file->hfs_data.extents[i].count;
    return blocks == file->hfs_data.blocks;
}

int hfsplus_closewrite(struct fat_filestr *filestr, uint32_t size,
                       struct fat_direntry *fatentp)
{
    if (!filestr || !filestr->fatfilep || filestr->fatfilep->hfs_kind != 2)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[IF_MV_VOL(filestr->fatfilep->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    struct fat_file *file = filestr->fatfilep;
    if (file->hfs_overlay) {
        unsigned int idx = file->hfs_overlay_index;
        if (idx >= HFS_OVL_MAX_ENTRIES ||
            !(s->overlay.entries[idx].flags & HFS_OVL_USED) ||
            s->overlay.entries[idx].id != (uint32_t)file->firstcluster) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        struct hfs_overlay_entry *e = &s->overlay.entries[idx];
        if ((e->flags & HFS_OVL_BASE) && size > e->size) {
            rc = overlay_materialize_base(s, e, &file->hfs_data);
            if (rc)
                goto out;
        }
        rc = overlay_ensure_capacity(s, e, size);
        if (rc)
            goto out;
        rc = overlay_shrink(s, e, size);
        if (rc)
            goto out;
        e->size = size;
        rc = overlay_sync_entry(s, idx);
        if (!rc) {
            file->hfs_data.size = size;
            if (fatentp)
                fill_overlay_fatent(e, fatentp);
        }
    } else if ((uint64_t)size != file->hfs_data.size) {
        /* Without the overlay only v10-style in-place, same-size writes are
         * safe. */
        rc = HFS_RO_UNSUPPORTED;
    } else if (fatentp) {
        struct hfs_ro_entry base;
        int frc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (!frc) {
            fat_empty_fat_direntry(fatentp);
            strcpy((char *)fatentp->name, base.name);
            fatentp->attr = ATTR_ARCHIVE;
            fatentp->filesize = (uint32_t)base.data.size;
            fatentp->firstcluster = (int32_t)base.id;
        }
    }
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_last_error(void)
{
    return last_error;
}

int hfsplus_last_stage(void)
{
    return last_stage;
}


bool hfsplus_overlay_ready(int volume)
{
    return (unsigned int)volume < NUM_VOLUMES && states[volume].overlay.ready;
}

int hfsplus_overlay_last_error(void)
{
    return overlay_diag_error;
}

int hfsplus_overlay_last_stage(void)
{
    return overlay_diag_stage;
}

int hfsplus_overlay_last_slot(void)
{
    return overlay_diag_slot;
}

uint32_t hfsplus_overlay_diag_id(void) { return overlay_diag_id; }
uint32_t hfsplus_overlay_diag_parent(void) { return overlay_diag_parent; }
uint32_t hfsplus_overlay_diag_flags(void) { return overlay_diag_flags; }
uint32_t hfsplus_overlay_diag_data_start(void) { return overlay_diag_data_start; }
uint32_t hfsplus_overlay_diag_data_blocks(void) { return overlay_diag_data_blocks; }
unsigned int hfsplus_overlay_diag_kind(void) { return overlay_diag_kind; }
unsigned int hfsplus_overlay_diag_name_len(void) { return overlay_diag_name_len; }

int hfsplus_partitions(int drive, struct partinfo *parts, int capacity,
                       int *multiplier)
{
    struct hfs_partition found[MAX_PARTITIONS_PER_DRIVE];
    memset(parts, 0, sizeof(*parts) * capacity);
    int rc = setup_io(&scan_io, drive);
    if (rc) {
        last_error = rc;
        last_stage = 1;
        return rc;
    }
    /* iPod Classic / iFlash partition geometry:
     *
     * The on-disk MBR stores partition start/size values in the iPod's
     * 4096-byte virtual-sector units even when the ATA layer reports 512-byte
     * logical sectors.  Parse the Apple_HFS entry directly from the MBR and
     * convert its LBA values using 4096 bytes.  hfsplus_mount() still performs
     * all HFS+ signature and metadata validation. */
    uint8_t mbr[512];
    rc = HFS_RO_UNSUPPORTED;
    if (!device_read(&scan_io, 0, mbr, sizeof(mbr)) &&
        mbr[510] == 0x55 && mbr[511] == 0xaa) {
        for (unsigned int i = 0; i < 4; ++i) {
            const uint8_t *p = mbr + 446 + i * 16;
            if (p[4] != 0xaf)
                continue;
            uint64_t begin = (uint32_t)p[8] | (uint32_t)p[9] << 8 |
                             (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
            uint64_t blocks = (uint32_t)p[12] | (uint32_t)p[13] << 8 |
                              (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
            uint64_t offset = begin * 4096u;
            uint64_t length = blocks * 4096u;
            if (!begin || !blocks || offset >= scan_io.size)
                continue;
            if (length > scan_io.size - offset)
                length = scan_io.size - offset;
            found[0].offset = offset;
            found[0].length = length;
            found[0].unit = 4096;
            found[0].type = 0xaf;
            rc = 1;
            break;
        }
    }

    /* Fallback for a standard Mac-formatted iPod Classic layout if the ATA
     * view of sector zero does not expose the USB-written MBR entry.  Disk
     * Mode places the HFS+ data partition at virtual LBA 16510.  Validate the
     * H+/HX signature before accepting this fallback. */
    if (rc != 1) {
        const uint64_t base = 16510u * 4096u;
        uint8_t sig[2];
        if (scan_io.size > base + 1536 &&
            !device_read(&scan_io, base + 1024, sig, sizeof(sig)) &&
            sig[0] == 'H' && (sig[1] == '+' || sig[1] == 'X')) {
            found[0].offset = base;
            found[0].length = scan_io.size - base;
            found[0].unit = 4096;
            found[0].type = 0xaf;
            rc = 1;
        } else {
            rc = hfs_partition_scan(device_read, &scan_io, scan_io.size,
                                    scan_io.sector_size, found,
                                    MAX_PARTITIONS_PER_DRIVE);
        }
    }
    if (rc < 0) {
        last_error = rc;
        last_stage = 1;
        return rc;
    }
    if (rc > capacity)
        return HFS_RO_BUFFER;
    *multiplier = 1;
    for (int i = 0; i < rc; ++i) {
        parts[i].start = found[i].offset / scan_io.sector_size;
        parts[i].size = found[i].length / scan_io.sector_size;
        parts[i].type = found[i].type;
        int mult = found[i].unit / scan_io.sector_size;
        if (mult > *multiplier)
            *multiplier = mult;
    }
    return rc;
}

int hfsplus_mount(int volume, int drive, sector_t start, sector_t count)
{
    if ((unsigned int)volume >= NUM_VOLUMES || hfsplus_mounted(volume))
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[volume];
    int rc = setup_io(&s->io, drive);
    if (rc) {
        last_error = rc;
        last_stage = 2;
        return rc;
    }
    if (!count || start > s->io.size / s->io.sector_size ||
        count > s->io.size / s->io.sector_size - start) {
        last_error = HFS_RO_FORMAT;
        last_stage = 2;
        return HFS_RO_FORMAT;
    }
    uint64_t base = (uint64_t)start * s->io.sector_size;
    uint64_t length = (uint64_t)count * s->io.sector_size;
    uint8_t sig[2];
    if (length < 1536 || device_read(&s->io, base + 1024, sig, sizeof(sig))) {
        last_error = HFS_RO_IO;
        last_stage = 2;
        return HFSPLUS_NOT_HFS;
    }
    if (sig[0] != 'H' || (sig[1] != '+' && sig[1] != 'X')) {
        last_error = HFS_RO_FORMAT;
        last_stage = 2;
        return HFSPLUS_NOT_HFS;
    }
    mutex_lock(&s->mutex);
    rc = hfs_ro_mount(&s->volume, device_read, &s->io, base, length,
                      s->scratch, sizeof(s->scratch));
    if (!rc)
        rc = hfs_ro_enable_fast_index(&s->volume, s->catalog_cache,
                                      sizeof(s->catalog_cache));
    if (!rc) {
        int orc = overlay_load(s);
        /* The overlay is optional.  Missing/unsupported container leaves the
         * volume readable (and v10-style same-size writes still work). */
        if (orc)
            memset(&s->overlay, 0, sizeof(s->overlay));
    }
    mutex_unlock(&s->mutex);
    if (rc) {
        last_error = rc;
        last_stage = 2;
    }
    return rc;
}

void hfsplus_unmount(int volume)
{
    if ((unsigned int)volume < NUM_VOLUMES) {
        memset(&states[volume].overlay, 0, sizeof(states[volume].overlay));
        hfs_ro_unmount(&states[volume].volume);
    }
}

int hfsplus_sector_size(int volume)
{
    return states[volume].io.sector_size;
}

unsigned int hfsplus_cluster_size(int volume)
{
    return states[volume].volume.block_size;
}

bool hfsplus_size(int volume, sector_t *size, sector_t *free)
{
    if (!hfsplus_mounted(volume))
        return false;
    struct hfs_state *s = &states[volume];
    const struct hfs_ro_volume *v = &s->volume;
    if (size)
        *size = (uint64_t)v->blocks * v->block_size / 1024;
    if (free) {
        uint64_t native_free = (uint64_t)v->free_blocks * v->block_size / 1024;
        if (s->overlay.ready) {
            mutex_lock(&s->mutex);
            uint64_t overlay_free = (uint64_t)overlay_free_blocks(&s->overlay) *
                                    HFS_OVL_BLOCK / 1024;
            mutex_unlock(&s->mutex);
            *free = native_free < overlay_free ? native_free : overlay_free;
        } else {
            *free = native_free;
        }
    }
    return true;
}

static struct hfs_overlay_entry *overlay_file_entry(struct hfs_state *s,
                                                    const struct fat_file *file)
{
    if (!file->hfs_overlay || file->hfs_overlay_index >= HFS_OVL_MAX_ENTRIES)
        return NULL;
    struct hfs_overlay_entry *e = &s->overlay.entries[file->hfs_overlay_index];
    return (e->flags & HFS_OVL_USED) && e->id == (uint32_t)file->firstcluster ? e : NULL;
}

static void fill_overlay_fatent(const struct hfs_overlay_entry *e,
                                struct fat_direntry *fatent)
{
    fat_empty_fat_direntry(fatent);
    strcpy((char *)fatent->name, e->name);
    fatent->attr = e->kind == 1 ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    fatent->filesize = e->kind == 2 ? (uint32_t)e->size : 0;
    fatent->firstcluster = (int32_t)e->id;
}

int hfsplus_open_root(int volume, struct fat_file *file)
{
    memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
    file->volume = volume;
#else
    (void)volume;
#endif
    file->firstcluster = HFS_RO_ROOT_ID;
    file->hfs_kind = 1;
    file->hfs_backing_id = HFS_RO_ROOT_ID;
    file->hfs_overlay_index = HFS_OVL_NONE;
    file->e.entry = HFS_RO_ROOT_ID;
    return 0;
}

int hfsplus_open(const struct fat_file *parent, long id, struct fat_file *file)
{
    struct hfs_state *s = &states[IF_MV_VOL(parent->volume)];
    int rc = HFS_RO_NOT_FOUND;
    mutex_lock(&s->mutex);
    int oi = overlay_find_id(&s->overlay, (uint32_t)id);
    if (oi >= 0) {
        struct hfs_overlay_entry *oe = &s->overlay.entries[oi];
        if (oe->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE))
            goto out;
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = (long)oe->id;
        file->dircluster = parent->firstcluster;
        file->e.entry = oe->id;
        file->hfs_kind = oe->kind;
        file->hfs_overlay = 1;
        file->hfs_overlay_index = (uint16_t)oi;
        file->hfs_backing_id = oe->base_id;
        file->hfs_data.size = oe->size;
        if ((oe->flags & HFS_OVL_BASE) && oe->base_id) {
            struct hfs_ro_entry base;
            rc = hfs_ro_find_id(&s->volume, oe->base_id, &base);
            if (rc)
                goto out;
            file->hfs_data = base.data;
            file->hfs_data.size = oe->size;
            if (oe->kind == 1)
                file->hfs_backing_id = base.id;
        }
        rc = 0;
        goto out;
    }

    uint32_t backing_parent = parent->hfs_overlay && parent->hfs_backing_id ?
                              parent->hfs_backing_id : (uint32_t)parent->firstcluster;
    struct hfs_ro_iterator it;
    struct hfs_ro_entry e;
    rc = hfs_ro_iter_init(&s->volume, backing_parent, &it);
    if (rc)
        goto out;
    while ((rc = hfs_ro_iter_next(&s->volume, &it, &e)) > 0) {
        if (e.id != (uint32_t)id)
            continue;
        if (e.unsupported || e.data.size > FAT_MAX_FILE_SIZE) {
            rc = HFS_RO_UNSUPPORTED;
            goto out;
        }
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = id;
        file->dircluster = parent->firstcluster;
        file->e.entry = e.id;
        file->hfs_data = e.data;
        file->hfs_kind = e.kind;
        file->hfs_backing_id = e.kind == 1 ? e.id : 0;
        file->hfs_overlay_index = HFS_OVL_NONE;
        rc = 0;
        goto out;
    }
    if (!rc)
        rc = HFS_RO_NOT_FOUND;
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_create_file(struct fat_file *parent, const char *name,
                        uint8_t attr, struct fat_file *file,
                        struct fat_direntry *fatent)
{
    if (!parent || !name || !file)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[IF_MV_VOL(parent->volume)];
    mutex_lock(&s->mutex);
    if (!s->overlay.ready) {
        mutex_unlock(&s->mutex);
        return HFS_RO_UNSUPPORTED;
    }
    uint16_t kind = (attr & ATTR_DIRECTORY) ? 1 : 2;
    int idx;
    int rc = overlay_create_entry(s, (uint32_t)parent->firstcluster, name,
                                  kind, 0, 0, 0, &idx);
    if (!rc) {
        struct hfs_overlay_entry *e = &s->overlay.entries[idx];
        memset(file, 0, sizeof(*file));
#ifdef HAVE_MULTIVOLUME
        file->volume = parent->volume;
#endif
        file->firstcluster = (long)e->id;
        file->dircluster = parent->firstcluster;
        file->e.entry = e->id;
        file->hfs_kind = kind;
        file->hfs_overlay = 1;
        file->hfs_overlay_index = (uint16_t)idx;
        if (fatent)
            fill_overlay_fatent(e, fatent);
    }
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_remove(struct fat_file *file, enum fat_remove_op what)
{
    if (!file || file->firstcluster == HFS_RO_ROOT_ID)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        unsigned int idx = file->hfs_overlay_index;
        if (what & FAT_RM_DIRENTRIES) {
            e->flags |= HFS_OVL_DELETED;
            rc = overlay_sync_entry(s, idx);
            if (rc)
                goto out;
            file->dircluster = 0;
            file->hfs_removed = 1;
        }
        if (what & FAT_RM_DATA) {
            if (!(e->flags & HFS_OVL_BASE) && e->data_blocks) {
                rc = overlay_free_run(s, e->data_start, e->data_blocks);
                if (rc)
                    goto out;
            }
            uint32_t old_id = e->id, old_parent = e->parent;
            overlay_id_remove(&s->overlay, old_id);
            overlay_parent_remove(&s->overlay, idx, old_parent);
            memset(e, 0, sizeof(*e));
            rc = overlay_sync_entry(s, idx);
            if ((uint16_t)idx < s->overlay.free_slot_hint ||
                s->overlay.free_slot_hint == HFS_OVL_NONE)
                s->overlay.free_slot_hint = (uint16_t)idx;
            file->hfs_overlay = 0;
            file->hfs_overlay_index = HFS_OVL_NONE;
        }
    } else {
        if (what & FAT_RM_DIRENTRIES) {
            struct hfs_ro_entry base;
            rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
            if (rc)
                goto out;
            rc = overlay_tombstone(s, (uint32_t)file->dircluster,
                                   base.id, base.name);
            if (rc)
                goto out;
            file->dircluster = 0;
            file->hfs_removed = 1;
        }
        /* Base HFS+ data remains allocated and becomes unreachable only from
         * Rockpod's merged namespace; native metadata is never modified. */
    }
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_rename(struct fat_file *parent, struct fat_file *file,
                   const unsigned char *newname)
{
    if (!parent || !file || !newname || file->firstcluster == HFS_RO_ROOT_ID)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        size_t len = strlen((const char *)newname);
        if (!len || len > 260) {
            rc = HFS_RO_ARGUMENT;
            goto out;
        }
        uint32_t old_parent = e->parent;
        char old_name[261];
        uint16_t old_name_len = e->name_len;
        memcpy(old_name, e->name, old_name_len + 1);
        if (old_parent != (uint32_t)parent->firstcluster)
            overlay_parent_remove(&s->overlay, file->hfs_overlay_index, old_parent);
        e->parent = (uint32_t)parent->firstcluster;
        e->name_len = (uint16_t)len;
        memcpy(e->name, newname, len + 1);
        if (old_parent != e->parent)
            overlay_parent_insert(&s->overlay, file->hfs_overlay_index);
        rc = overlay_sync_entry(s, file->hfs_overlay_index);
        if (!rc) {
            file->dircluster = parent->firstcluster;
        } else {
            if (old_parent != e->parent)
                overlay_parent_remove(&s->overlay, file->hfs_overlay_index, e->parent);
            e->parent = old_parent;
            e->name_len = old_name_len;
            memcpy(e->name, old_name, old_name_len + 1);
            if (old_parent != (uint32_t)parent->firstcluster)
                overlay_parent_insert(&s->overlay, file->hfs_overlay_index);
        }
    } else {
        rc = overlay_shadow_base(s, file, (uint32_t)parent->firstcluster,
                                 (const char *)newname);
    }
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_modtime(struct fat_file *parent, struct fat_file *file,
                    time_t modtime)
{
    (void)parent;
    if (!file)
        return HFS_RO_ARGUMENT;
    struct hfs_state *s = &states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    if (file->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, file);
        if (!e)
            rc = HFS_RO_FORMAT;
        else {
            e->mtime = (uint32_t)modtime;
            rc = overlay_sync_entry(s, file->hfs_overlay_index);
        }
    }
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_truncate(const struct fat_filestr *filestr)
{
    if (!filestr || !filestr->fatfilep)
        return HFS_RO_ARGUMENT;
    struct fat_file *file = filestr->fatfilep;
    struct hfs_state *s = &states[IF_MV_VOL(file->volume)];
    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    if (!s->overlay.ready) {
        rc = HFS_RO_UNSUPPORTED;
        goto out;
    }
    if (!file->hfs_overlay) {
        struct hfs_ro_entry base;
        rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (rc)
            goto out;
        rc = overlay_shadow_base(s, file, (uint32_t)file->dircluster, base.name);
        if (rc)
            goto out;
    }
    /* Exact byte length is committed by hfsplus_closewrite().  Deferring the
     * physical shrink also avoids throwing away the partial final sector. */
    rc = 1;
out:
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_readdir(struct fat_filestr *stream, struct fat_dirscan_info *scan,
                    struct fat_direntry *entry)
{
    struct hfs_state *s = &states[IF_MV_VOL(stream->fatfilep->volume)];
    struct hfs_ro_entry e;
    int rc = 0;
    mutex_lock(&s->mutex);
    uint32_t logical_parent = (uint32_t)stream->fatfilep->firstcluster;
    uint32_t backing_parent = stream->fatfilep->hfs_overlay &&
                              stream->fatfilep->hfs_backing_id ?
                              stream->fatfilep->hfs_backing_id : logical_parent;
    if (scan->entry == FAT_DIRSCAN_RW_VAL) {
        /* Overlay-only directories have no native catalog CNID.  A renamed or
         * shadowed base directory does, through hfs_backing_id. */
        bool has_base_directory = !stream->fatfilep->hfs_overlay ||
                                  stream->fatfilep->hfs_backing_id != 0;
        scan->hfs_phase = has_base_directory ? 0 : 1;
        scan->hfs_overlay_index = 0;
        if (!scan->hfs_phase) {
            rc = hfs_ro_iter_init(&s->volume, backing_parent,
                                  &scan->hfs_iterator);
            if (rc)
                goto out;
        }
    }

    if (scan->hfs_phase == 0) {
        while ((rc = hfs_ro_iter_next(&s->volume, &scan->hfs_iterator, &e)) > 0) {
            if (e.unsupported || strlen(e.name) > FAT_DIRENTRY_NAME_MAX ||
                e.data.size > FAT_MAX_FILE_SIZE)
                continue;
            if (logical_parent == HFS_RO_ROOT_ID && !strcmp(e.name, HFS_OVL_NAME))
                continue;
            if (s->overlay.ready && overlay_hides_base(&s->overlay,
                    logical_parent, e.id, e.name))
                continue;
            fat_empty_fat_direntry(entry);
            strcpy((char *)entry->name, e.name);
            entry->attr = e.kind == 1 ? ATTR_DIRECTORY : ATTR_ARCHIVE;
            entry->filesize = (uint32_t)e.data.size;
            entry->firstcluster = (int32_t)e.id;
            scan->entry = e.id;
            scan->entries = 1;
            goto out;
        }
        if (rc < 0)
            goto out;
        scan->hfs_phase = 1;
        scan->hfs_overlay_index = 0;
    }

    if (s->overlay.ready) {
        /* Use the v11 linear overlay namespace walk.  The HFS+ catalog itself
         * remains indexed, so this has negligible impact for a normal overlay
         * containing tens or hundreds of writable files, while avoiding any
         * dependency on the v12 parent hash for path discovery. */
        while (scan->hfs_overlay_index < HFS_OVL_MAX_ENTRIES) {
            unsigned int idx = scan->hfs_overlay_index++;
            struct hfs_overlay_entry *oe = &s->overlay.entries[idx];
            if (!(oe->flags & HFS_OVL_USED) ||
                (oe->flags & (HFS_OVL_DELETED | HFS_OVL_TOMBSTONE)) ||
                oe->parent != logical_parent)
                continue;
            fill_overlay_fatent(oe, entry);
            scan->entry = oe->id;
            scan->entries = 1;
            rc = 1;
            goto out;
        }
    }
    rc = 0;
out:
    if (rc <= 0) {
        fat_empty_fat_direntry(entry);
        scan->entries = 0;
    }
    mutex_unlock(&s->mutex);
    return rc;
}

int hfsplus_seek(struct fat_filestr *stream, unsigned long sector)
{
    struct hfs_state *s = &states[IF_MV_VOL(stream->fatfilep->volume)];
    uint64_t bytes = stream->fatfilep->hfs_data.size;
    mutex_lock(&s->mutex);
    if (stream->fatfilep->hfs_overlay) {
        struct hfs_overlay_entry *e = overlay_file_entry(s, stream->fatfilep);
        if (e)
            bytes = e->size;
    }
    mutex_unlock(&s->mutex);
    uint64_t sectors = (bytes + s->io.sector_size - 1) / s->io.sector_size;
    if (sector > sectors)
        return FAT_SEEK_EOF;
    stream->lastsector = sector;
    stream->eof = false;
    return 0;
}

long hfsplus_readwrite(struct fat_filestr *stream, unsigned long sectors,
                       void *buffer, bool write)
{
    if (!sectors || (!write && stream->eof))
        return 0;
    struct fat_file *file = stream->fatfilep;
    struct hfs_state *s = &states[IF_MV_VOL(file->volume)];
    if (sectors > LONG_MAX / s->io.sector_size)
        return HFS_RO_ARGUMENT;
    size_t requested = sectors * s->io.sector_size;
    uint64_t offset = (uint64_t)stream->lastsector * s->io.sector_size;

    mutex_lock(&s->mutex);
    int rc = HFS_RO_OK;
    size_t got = 0;

    if (write && s->overlay.ready && !file->hfs_overlay) {
        struct hfs_ro_entry base;
        rc = hfs_ro_find_id(&s->volume, (uint32_t)file->firstcluster, &base);
        if (rc)
            goto out;
        rc = overlay_shadow_base(s, file, (uint32_t)file->dircluster, base.name);
        if (rc)
            goto out;
    }

    if (file->hfs_overlay) {
        struct hfs_overlay_entry *oe = overlay_file_entry(s, file);
        if (!oe) {
            rc = HFS_RO_FORMAT;
            goto out;
        }
        if (write) {
            if (oe->flags & HFS_OVL_BASE) {
                rc = overlay_materialize_base(s, oe, &file->hfs_data);
                if (rc)
                    goto out;
                /* Persist the new backing run before data writes. Size remains
                 * the last committed value until close/fsync. */
                rc = overlay_sync_entry(s, file->hfs_overlay_index);
                if (rc)
                    goto out;
            }
            uint64_t end = offset + requested;
            if (end < offset) {
                rc = HFS_RO_ARGUMENT;
                goto out;
            }
            uint32_t old_start = oe->data_start, old_blocks = oe->data_blocks;
            rc = overlay_ensure_capacity(s, oe, end);
            if (rc)
                goto out;
            if (old_start != oe->data_start || old_blocks != oe->data_blocks) {
                rc = overlay_sync_entry(s, file->hfs_overlay_index);
                if (rc)
                    goto out;
            }
            rc = overlay_data_write(s, oe, offset, buffer, requested);
            if (rc)
                goto out;
            if (end > oe->size)
                oe->size = end;
            /* Do not rewrite the 320-byte entry for every sector. close/fsync
             * commits the logical size; allocation changes above are already
             * persisted before data is exposed. */
            file->hfs_data.size = oe->size;
            got = requested;
        } else {
            if (offset >= oe->size) {
                got = 0;
                goto success;
            }
            got = requested;
            if (oe->size - offset < got)
                got = (size_t)(oe->size - offset);
            if (oe->flags & HFS_OVL_BASE) {
                struct hfs_ro_entry base;
                memset(&base, 0, sizeof(base));
                base.kind = 2;
                base.data = file->hfs_data;
                size_t done = 0;
                rc = hfs_ro_pread(&s->volume, &base, offset, buffer, got, &done);
                if (rc || done != got) {
                    rc = rc ? rc : HFS_RO_IO;
                    goto out;
                }
            } else if (got) {
                rc = overlay_data_read(s, oe, offset, buffer, got);
                if (rc)
                    goto out;
            }
        }
    } else {
        struct hfs_ro_entry e;
        memset(&e, 0, sizeof(e));
        e.kind = file->hfs_kind;
        e.data = file->hfs_data;
        if (write) {
            if (!hfsplus_can_write_existing(file) || offset >= e.data.size) {
                rc = HFS_RO_UNSUPPORTED;
                goto out;
            }
            size_t writable = requested;
            if (e.data.size - offset < writable)
                writable = (size_t)(e.data.size - offset);
            rc = write_existing_inline(s, &e, offset, buffer, writable);
            if (rc)
                goto out;
            got = requested;
        } else {
            rc = hfs_ro_pread(&s->volume, &e, offset, buffer, requested, &got);
            if (rc)
                goto out;
        }
    }

success:
    rc = HFS_RO_OK;
out:
    mutex_unlock(&s->mutex);
    if (rc)
        return rc;

    size_t transferred = write ? sectors :
        (got + s->io.sector_size - 1) / s->io.sector_size;
    size_t padded = transferred * s->io.sector_size;
    if (!write && padded > got)
        memset((uint8_t *)buffer + got, 0, padded - got);
    stream->lastsector += transferred;
    uint64_t logical_size = file->hfs_data.size;
    if (file->hfs_overlay) {
        mutex_lock(&s->mutex);
        struct hfs_overlay_entry *oe = overlay_file_entry(s, file);
        if (oe)
            logical_size = oe->size;
        mutex_unlock(&s->mutex);
    }
    stream->eof = offset + got >= logical_size;
    return (long)transferred;
}

