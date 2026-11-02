/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Persistent overlay format: v1/v2 headers, checksums, backups, bitmap and
 * entry serialization, and container-relative data access.  This module owns
 * bytes on disk, not allocation or namespace policy.
 */
#include "hfsplus_overlay_format.h"
#include "hfsplus_io.h"
#include "storage.h"
#include <string.h>

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

int overlay_container_read(struct hfs_state *s, uint64_t off,
                           void *buffer, size_t length)
{
    size_t done = 0;
    int rc = hfs_ro_pread(&s->volume, &s->overlay.container, off,
                          buffer, length, &done);
    return rc ? rc : (done == length ? HFS_RO_OK : HFS_RO_FORMAT);
}

int overlay_container_write(struct hfs_state *s, uint64_t off,
                            const void *buffer, size_t length)
{
    return write_container_inline(s, off, buffer, length);
}

uint64_t overlay_entry_offset(const struct hfs_overlay *o,
                              unsigned int index)
{
    return (uint64_t)(o->bitmap_start + o->bitmap_blocks) * HFS_OVL_BLOCK +
           (uint64_t)index * HFS_OVL_ENTRY_SIZE;
}

static int overlay_flush(void)
{
#ifdef HAVE_STORAGE_FLUSH
    if (storage_flush())
        return HFS_RO_IO;
#endif
    return HFS_RO_OK;
}

static uint32_t overlay_header_sum(const uint8_t *b)
{
    uint32_t sum = 0;
    for (unsigned int i = 0; i < 40; ++i)
        sum = (sum << 8) ^ (sum + b[i]);
    return ~sum;
}

int overlay_sync_header(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    uint8_t *b = s->scratch;
    memset(b, 0, HFS_OVL_BLOCK);
    memcpy(b, o->version == HFS_OVL_VERSION_V1 ? HFS_OVL_MAGIC_V1 :
              HFS_OVL_MAGIC, 7);
    ovl_put32(b + 8, o->version);
    ovl_put32(b + 12, HFS_OVL_BLOCK);
    ovl_put32(b + 16, HFS_OVL_MAX_ENTRIES);
    ovl_put32(b + 20, o->total_blocks);
    ovl_put32(b + 24, o->bitmap_bytes);
    ovl_put32(b + 28, o->data_start);
    ovl_put32(b + 32, o->next_id);
    ovl_put32(b + 36, ++o->generation);
    ovl_put32(b + 40, overlay_header_sum(b));
    /* The v2 backup lives in block 1.  Legacy v1 keeps its bitmap in block 1,
     * so it stores a 64-byte backup at offset 2048 inside the header block.
     * Write the backup first so a torn primary can still be recovered. */
    int rc = HFS_RO_OK;
    if (o->version == HFS_OVL_VERSION)
        rc = overlay_container_write(s, HFS_OVL_BLOCK, b, HFS_OVL_BLOCK);
    else
        memcpy(b + 2048, b, 64); /* v1 backup inside the header block */
    if (!rc)
        rc = overlay_container_write(s, 0, b, HFS_OVL_BLOCK);
    return rc ? rc : overlay_flush();
}

int overlay_sync_bitmap(struct hfs_state *s)
{
    int rc = overlay_container_write(s,
                                     (uint64_t)s->overlay.bitmap_start *
                                     HFS_OVL_BLOCK,
                                     s->overlay.bitmap,
                                     s->overlay.bitmap_bytes);
    return rc ? rc : overlay_flush();
}

/* Persist only the device sectors of the bitmap touched by an allocation.
 * v11 rewrote the entire 16-64 KiB bitmap even for a single 4 KiB block. */
int overlay_sync_bitmap_range(struct hfs_state *s, uint32_t start,
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
    int rc = overlay_container_write(s,
                                     (uint64_t)o->bitmap_start * HFS_OVL_BLOCK +
                                     begin, o->bitmap + begin, end - begin);
    return rc ? rc : overlay_flush();
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

int overlay_decode_entry(struct hfs_overlay_entry *e, const uint8_t *b)
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
        e->name_len == 0 || e->name_len > FAT_DIRENTRY_NAME_MAX ||
        e->id < HFS_OVL_ID_BASE || e->id > 0x7fffffffu ||
        (e->flags & ~(HFS_OVL_USED | HFS_OVL_DELETED |
                      HFS_OVL_BASE | HFS_OVL_TOMBSTONE)) ||
        e->parent < HFS_RO_ROOT_ID ||
        e->data_start > HFS_OVL_MAX_BLOCKS ||
        e->data_blocks > HFS_OVL_MAX_BLOCKS - e->data_start)
        return HFS_RO_FORMAT;
    memcpy(e->name, b + 40, e->name_len);
    e->name[e->name_len] = 0;
    return HFS_RO_OK;
}

int overlay_sync_entry(struct hfs_state *s, unsigned int index)
{
    uint8_t raw[HFS_OVL_ENTRY_SIZE];
    if (index >= HFS_OVL_MAX_ENTRIES)
        return HFS_RO_ARGUMENT;
    overlay_encode_entry(&s->overlay.entries[index], raw);
    int rc = overlay_container_write(s, overlay_entry_offset(&s->overlay, index),
                                     raw, sizeof(raw));
    return rc ? rc : overlay_flush();
}

int overlay_data_read(struct hfs_state *s,
                      const struct hfs_overlay_entry *e,
                      uint64_t off, void *buffer, size_t length)
{
    if (off > e->size || length > e->size - off)
        return HFS_RO_FORMAT;
    if (!length)
        return HFS_RO_OK;
    if (!e->data_blocks || e->data_start < s->overlay.data_start ||
        e->data_start >= s->overlay.total_blocks ||
        e->data_blocks > s->overlay.total_blocks - e->data_start ||
        off + length > (uint64_t)e->data_blocks * HFS_OVL_BLOCK)
        return HFS_RO_FORMAT;
    return overlay_container_read(s,
        (uint64_t)e->data_start * HFS_OVL_BLOCK + off, buffer, length);
}

int overlay_data_write(struct hfs_state *s,
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

int overlay_parse_header(struct hfs_state *s, const uint8_t *b,
                                int backup)
{
    struct hfs_overlay *o = &s->overlay;
    uint32_t version;
    if (!memcmp(b, HFS_OVL_MAGIC, 7))
        version = HFS_OVL_VERSION;
    else if (!memcmp(b, HFS_OVL_MAGIC_V1, 7))
        version = HFS_OVL_VERSION_V1;
    else
        return HFS_RO_FORMAT;
    if (ovl_get32(b + 8) != version)
        return HFS_RO_FORMAT;
    uint32_t sum = ovl_get32(b + 40);
    if ((version == HFS_OVL_VERSION && !sum) ||
        (sum && sum != overlay_header_sum(b)))
        return HFS_RO_FORMAT;
    uint32_t bitmap_start = version == HFS_OVL_VERSION_V1 ? 1u : 2u;
    uint32_t expected = bitmap_start + o->bitmap_blocks + o->entry_blocks;
    uint32_t total = ovl_get32(b + 20);
    uint32_t next = ovl_get32(b + 32);
    if (ovl_get32(b + 12) != HFS_OVL_BLOCK ||
        ovl_get32(b + 16) != HFS_OVL_MAX_ENTRIES ||
        total != o->total_blocks ||
        ovl_get32(b + 24) != o->bitmap_bytes ||
        ovl_get32(b + 28) != expected ||
        expected >= total || next < HFS_OVL_ID_BASE ||
        next > 0x7fffffffu)
        return HFS_RO_FORMAT;
    o->version = version;
    o->bitmap_start = bitmap_start;
    o->data_start = expected;
    o->next_id = next;
    o->generation = ovl_get32(b + 36);
    o->header_recovered = backup;
    return HFS_RO_OK;
}
