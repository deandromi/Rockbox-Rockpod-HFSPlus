/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Persistent directory/CNID index cache for v3 overlays.  The cache lives in
 * reserved container blocks, so it needs no namespace bootstrap and can be
 * read immediately after the overlay header.
 */
#include "hfsplus_index_cache.h"
#include "hfsplus_overlay_format.h"
#include "hfsplus_ro.h"
#include <string.h>

static uint32_t cache_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int hfs_index_cache_load(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    if (!s || o->version != HFS_OVL_VERSION_3 || !o->ready ||
        o->cache_blocks != HFS_OVL_CACHE_BLOCKS || !o->cache_start)
        return HFS_RO_NOT_FOUND;
    uint64_t off = (uint64_t)o->cache_start * HFS_OVL_BLOCK;
    int rc = overlay_container_read(s, off, s->index_cache,
                                    HFS_RO_INDEX_HEADER);
    if (rc)
        return rc;
    if (memcmp(s->index_cache, HFS_RO_INDEX_MAGIC, 6) ||
        cache_le32(s->index_cache + 8) != HFS_RO_INDEX_VERSION)
        return HFS_RO_NOT_FOUND;
    uint32_t used = cache_le32(s->index_cache + 48);
    if (used < HFS_RO_INDEX_HEADER || used > HFS_OVL_CACHE_BYTES)
        return HFS_RO_FORMAT;
    rc = overlay_container_read(s, off, s->index_cache, used);
    if (rc)
        return rc;
    return hfs_ro_fast_index_import(&s->volume, s->catalog_cache,
                                    sizeof(s->catalog_cache),
                                    s->index_cache, used);
}

int hfs_index_cache_save(struct hfs_state *s)
{
    struct hfs_overlay *o = &s->overlay;
    if (!s || o->version != HFS_OVL_VERSION_3 || !o->ready ||
        o->cache_blocks != HFS_OVL_CACHE_BLOCKS || !o->cache_start)
        return HFS_RO_NOT_FOUND;
    size_t used = 0;
    int rc = hfs_ro_fast_index_export(&s->volume, s->index_cache,
                                      sizeof(s->index_cache), &used);
    if (rc)
        return rc;
    /* The cache is advisory.  A torn write invalidates its checksum and the
     * next mount rebuilds; no flush is needed here. */
    return overlay_container_write(s, (uint64_t)o->cache_start * HFS_OVL_BLOCK,
                                   s->index_cache, used);
}
