/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hfsplus_ro.h"
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static uint32_t j32(const uint8_t *p, int little)
{
    if (!little)
        return be32(p);
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
           (uint32_t)p[1] << 8 | p[0];
}

static uint64_t j64(const uint8_t *p, int little)
{
    if (!little)
        return be64(p);
    return (uint64_t)j32(p + 4, 1) << 32 | j32(p, 1);
}

static int power2(uint32_t n)
{
    return n && !(n & (n - 1));
}

static int read_volume(struct hfs_ro_volume *v, uint64_t off,
                        void *buf, size_t size)
{
    if (off > v->length || size > v->length - off)
        return HFS_RO_FORMAT;
    return v->read(v->context, v->base + off, buf, size) ?
           HFS_RO_IO : HFS_RO_OK;
}

static int parse_fork(struct hfs_ro_volume *v, const uint8_t *p,
                       struct hfs_ro_fork *fork)
{
    uint64_t sum = 0;
    int ended = 0;
    fork->size = be64(p);
    fork->blocks = be32(p + 12);
    if (fork->blocks > v->blocks ||
        fork->size > (uint64_t)fork->blocks * v->block_size)
        return HFS_RO_FORMAT;
    for (unsigned int i = 0; i < 8; ++i) {
        struct hfs_ro_extent *e = &fork->extents[i];
        e->start = be32(p + 16 + i * 8);
        e->count = be32(p + 20 + i * 8);
        if (!e->count) {
            if (e->start)
                return HFS_RO_FORMAT;
            ended = 1;
            continue;
        }
        if (ended || !e->start || e->start >= v->blocks ||
            e->count > v->blocks - e->start)
            return HFS_RO_FORMAT;
        for (unsigned int j = 0; j < i; ++j) {
            const struct hfs_ro_extent *old = &fork->extents[j];
            if (e->start < (uint64_t)old->start + old->count &&
                old->start < (uint64_t)e->start + e->count)
                return HFS_RO_FORMAT;
        }
        sum += e->count;
    }
    if (sum > fork->blocks || (ended && sum != fork->blocks))
        return HFS_RO_FORMAT;
    return HFS_RO_OK;
}

static int inline_fork(const struct hfs_ro_fork *fork)
{
    uint64_t sum = 0;
    for (unsigned int i = 0; i < 8; ++i)
        sum += fork->extents[i].count;
    return sum == fork->blocks;
}

static int read_fork(struct hfs_ro_volume *v, const struct hfs_ro_fork *fork,
                      uint64_t off, void *buffer, size_t length)
{
    uint8_t *out = buffer;
    if (off > fork->size || length > fork->size - off)
        return HFS_RO_FORMAT;
    if (!inline_fork(fork))
        return HFS_RO_UNSUPPORTED;
    for (unsigned int i = 0; i < 8 && length; ++i) {
        const struct hfs_ro_extent *e = &fork->extents[i];
        uint64_t bytes = (uint64_t)e->count * v->block_size;
        if (off >= bytes) {
            off -= bytes;
            continue;
        }
        size_t chunk = length;
        if (bytes - off < chunk)
            chunk = (size_t)(bytes - off);
        int rc = read_volume(v, (uint64_t)e->start * v->block_size + off,
                             out, chunk);
        if (rc)
            return rc;
        out += chunk;
        length -= chunk;
        off = 0;
    }
    return length ? HFS_RO_FORMAT : HFS_RO_OK;
}

static int check_journal(struct hfs_ro_volume *v, uint32_t info_block)
{
    uint8_t info[52], header[44];
    if (!info_block || info_block >= v->blocks)
        return HFS_RO_FORMAT;
    int rc = read_volume(v, (uint64_t)info_block * v->block_size,
                         info, sizeof(info));
    if (rc)
        return rc;
    if (be32(info) != 1)
        return HFS_RO_UNSUPPORTED;
    uint64_t offset = be64(info + 36), size = be64(info + 44);
    if (size < 1024 || offset < 1536 || offset % 512 || size % 512 ||
        offset > v->length || size > v->length - offset)
        return HFS_RO_FORMAT;
    rc = read_volume(v, offset, header, sizeof(header));
    if (rc)
        return rc;
    int little = be32(header + 4) == 0x78563412u;
    if (j32(header + 4, little) != 0x12345678u ||
        j32(header, little) != 0x4a4e4c78u)
        return HFS_RO_FORMAT;
    uint64_t start = j64(header + 8, little);
    uint64_t end = j64(header + 16, little);
    uint32_t header_size = j32(header + 40, little);
    uint32_t list_size = j32(header + 32, little);
    if (!power2(header_size) || header_size < 512 ||
        header_size > HFS_RO_MAX_NODE_SIZE || size <= header_size ||
        j64(header + 24, little) != size ||
        list_size < 32 || list_size > size - header_size ||
        start < header_size || start >= size ||
        end < header_size || end >= size ||
        start % header_size || end % header_size)
        return HFS_RO_FORMAT;
    uint32_t expected = j32(header + 36, little), sum = 0;
    memset(header + 36, 0, 4);
    for (size_t i = 0; i < sizeof(header); ++i)
        sum = (sum << 8) ^ (sum + header[i]);
    if (~sum != expected)
        return HFS_RO_FORMAT;
    return start == end ? HFS_RO_OK : HFS_RO_DIRTY;
}

static int node_record(const uint8_t *node, uint16_t size, uint16_t index,
                        const uint8_t **record, size_t *length)
{
    unsigned int count = be16(node + 10);
    if (count > (size - 14u) / 2u - 1u || index >= count)
        return HFS_RO_FORMAT;
    unsigned int table = size - 2u * (count + 1u);
    unsigned int prev = 14;
    for (unsigned int i = 0; i <= count; ++i) {
        unsigned int off = be16(node + size - 2u * (i + 1u));
        if (off < prev || off > table || off % 2 ||
            (i == 0 && off != 14) || (i && off == prev))
            return HFS_RO_FORMAT;
        prev = off;
    }
    unsigned int begin = be16(node + size - 2u * (index + 1u));
    unsigned int end = be16(node + size - 2u * (index + 2u));
    *record = node + begin;
    *length = end - begin;
    return HFS_RO_OK;
}

static int parse_record(struct hfs_ro_volume *v, const uint8_t *p,
                        size_t length, struct hfs_ro_entry *e);

static int read_catalog_node(struct hfs_ro_volume *v, uint32_t node,
                             const uint8_t **out)
{
    if (!v || node >= v->total_nodes || !out)
        return HFS_RO_ARGUMENT;
    if (v->catalog_cache && v->catalog_cache_size >= v->node_size) {
        if (v->catalog_cache_node != node) {
            int rc = read_fork(v, &v->catalog, (uint64_t)node * v->node_size,
                               v->catalog_cache, v->node_size);
            if (rc)
                return rc;
            v->catalog_cache_node = node;
        }
        *out = v->catalog_cache;
        return HFS_RO_OK;
    }
    int rc = read_fork(v, &v->catalog, (uint64_t)node * v->node_size,
                       v->scratch, v->node_size);
    if (!rc)
        *out = v->scratch;
    return rc;
}

static int record_parent_kind(const uint8_t *record, size_t length,
                              uint32_t *parent, uint16_t *kind)
{
    if (!record || length < 10 || !parent || !kind)
        return HFS_RO_FORMAT;
    uint16_t key_size = be16(record);
    if (key_size < 6 || (size_t)key_size + 4u > length)
        return HFS_RO_FORMAT;
    *parent = be32(record + 2);
    *kind = be16(record + key_size + 2);
    return HFS_RO_OK;
}

static int build_dir_index(struct hfs_ro_volume *v)
{
    v->dir_index_count = 0;
    v->dir_index_complete = 1;
    uint32_t node = v->first_leaf, previous = 0, visited = 0, seen = 0;
    uint32_t last_parent = 0;
    while (node) {
        if (node >= v->total_nodes || visited++ >= v->total_nodes)
            return HFS_RO_FORMAT;
        const uint8_t *n;
        int rc = read_catalog_node(v, node, &n);
        if (rc)
            return rc;
        uint16_t count = be16(n + 10);
        uint32_t next = be32(n);
        if (n[8] != 255 || n[9] != 1 || !count || be32(n + 4) != previous ||
            next >= v->total_nodes || next == node)
            return HFS_RO_FORMAT;
        for (uint16_t i = 0; i < count; ++i) {
            const uint8_t *record;
            size_t length;
            rc = node_record(n, v->node_size, i, &record, &length);
            if (rc)
                return rc;
            if (seen++ >= v->leaf_records)
                return HFS_RO_FORMAT;
            uint32_t parent;
            uint16_t kind;
            rc = record_parent_kind(record, length, &parent, &kind);
            if (rc)
                return rc;
            /* Validate the complete record once while building the index. */
            struct hfs_ro_entry validated;
            rc = parse_record(v, record, length, &validated);
            if (rc < 0)
                return rc;
            /* Only normal file/folder records describe children. Thread
             * records would otherwise add one pseudo-parent per object. */
            if ((kind == 1 || kind == 2) && parent != last_parent) {
                if (v->dir_index_count < HFS_RO_DIR_INDEX_MAX) {
                    struct hfs_ro_dirloc *d = &v->dir_index[v->dir_index_count++];
                    d->parent = parent;
                    d->node = node;
                    d->record = i;
                } else {
                    v->dir_index_complete = 0;
                }
                last_parent = parent;
            }
        }
        if (!next && (node != v->last_leaf || seen != v->leaf_records))
            return HFS_RO_FORMAT;
        previous = node;
        node = next;
    }
    return seen == v->leaf_records ? HFS_RO_OK : HFS_RO_FORMAT;
}

int hfs_ro_enable_fast_index(struct hfs_ro_volume *v, void *cache,
                             size_t cache_size)
{
    if (!v || !v->mounted || !cache || cache_size < v->node_size)
        return HFS_RO_ARGUMENT;
    v->catalog_cache = cache;
    v->catalog_cache_size = cache_size;
    v->catalog_cache_node = UINT32_MAX;
    return build_dir_index(v);
}

static int read_catalog_header(struct hfs_ro_volume *v)
{
    uint8_t prefix[120];
    int rc = read_fork(v, &v->catalog, 0, prefix, sizeof(prefix));
    if (rc)
        return rc;
    uint16_t size = be16(prefix + 32);
    if (!power2(size) || size < 512 || size > HFS_RO_MAX_NODE_SIZE)
        return HFS_RO_FORMAT;
    if (size > v->scratch_size)
        return HFS_RO_BUFFER;
    rc = read_fork(v, &v->catalog, 0, v->scratch, size);
    if (rc)
        return rc;
    if (v->scratch[8] != 1 || v->scratch[9] != 0 ||
        be32(v->scratch + 4) || be16(v->scratch + 10) != 3)
        return HFS_RO_FORMAT;
    const uint8_t *h;
    size_t length;
    rc = node_record(v->scratch, size, 0, &h, &length);
    if (rc || length < 106)
        return HFS_RO_FORMAT;
    uint16_t depth = be16(h);
    uint32_t root = be32(h + 2);
    v->leaf_records = be32(h + 6);
    v->first_leaf = be32(h + 10);
    v->last_leaf = be32(h + 14);
    v->total_nodes = be32(h + 22);
    v->node_size = size;
    if (!depth || depth > 32 || !root || root >= v->total_nodes ||
        !v->leaf_records || !v->first_leaf || !v->last_leaf ||
        v->first_leaf >= v->total_nodes || v->last_leaf >= v->total_nodes ||
        v->catalog.size != (uint64_t)v->total_nodes * size ||
        be32(h + 26) > v->total_nodes || be16(h + 20) != 516 ||
        (be32(h + 38) & 6u) != 6u || h[36])
        return HFS_RO_FORMAT;
    return HFS_RO_OK;
}

int hfs_ro_mount(struct hfs_ro_volume *v, hfs_ro_read_fn read,
                 void *context, uint64_t base, uint64_t length,
                 void *scratch, size_t scratch_size)
{
    if (!v)
        return HFS_RO_ARGUMENT;
    memset(v, 0, sizeof(*v));
    if (!read || !scratch || length < 2048 || base > UINT64_MAX - length)
        return HFS_RO_ARGUMENT;
    v->read = read;
    v->context = context;
    v->base = base;
    v->length = length;
    v->scratch = scratch;
    v->scratch_size = scratch_size;
    uint8_t header[512];
    int rc = read_volume(v, 1024, header, sizeof(header));
    if (rc)
        return rc;
    if (be16(header) != 0x482b || be16(header + 2) != 4)
        return HFS_RO_UNSUPPORTED;
    v->attributes = be32(header + 4);
    v->block_size = be32(header + 40);
    v->blocks = be32(header + 44);
    v->free_blocks = be32(header + 48);
    if (!power2(v->block_size) || v->block_size < 512 || !v->blocks ||
        v->free_blocks > v->blocks ||
        (uint64_t)v->blocks * v->block_size > length)
        return HFS_RO_FORMAT;
    v->length = (uint64_t)v->blocks * v->block_size;
    if (!(v->attributes & (1u << 8)) || (v->attributes & (1u << 11)))
        return HFS_RO_DIRTY;
    if (v->attributes & (1u << 13)) {
        if (be32(header + 8) != 0x4846534au)
            return HFS_RO_UNSUPPORTED;
        rc = check_journal(v, be32(header + 12));
        if (rc)
            return rc;
    }
    rc = parse_fork(v, header + 272, &v->catalog);
    if (rc)
        return rc;
    rc = read_catalog_header(v);
    if (rc)
        return rc;
    v->mounted = 1;
    return HFS_RO_OK;
}

void hfs_ro_unmount(struct hfs_ro_volume *v)
{
    if (v)
        memset(v, 0, sizeof(*v));
}

static int decode_name(const uint8_t *p, uint16_t units, char *out)
{
    size_t used = 0;
    for (unsigned int i = 0; i < units; ++i) {
        uint32_t ch = be16(p + i * 2);
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (++i >= units)
                return HFS_RO_FORMAT;
            uint32_t low = be16(p + i * 2);
            if (low < 0xdc00 || low > 0xdfff)
                return HFS_RO_FORMAT;
            ch = 0x10000u + ((ch - 0xd800u) << 10) + low - 0xdc00u;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return HFS_RO_FORMAT;
        }
        if (!ch || ch == ':')
            return HFS_RO_UNSUPPORTED;
        if (ch == '/')
            ch = ':';
        if (ch < 0x80) {
            out[used++] = (char)ch;
        } else if (ch < 0x800) {
            out[used++] = (char)(0xc0 | (ch >> 6));
            out[used++] = (char)(0x80 | (ch & 63));
        } else if (ch < 0x10000) {
            out[used++] = (char)(0xe0 | (ch >> 12));
            out[used++] = (char)(0x80 | ((ch >> 6) & 63));
            out[used++] = (char)(0x80 | (ch & 63));
        } else {
            out[used++] = (char)(0xf0 | (ch >> 18));
            out[used++] = (char)(0x80 | ((ch >> 12) & 63));
            out[used++] = (char)(0x80 | ((ch >> 6) & 63));
            out[used++] = (char)(0x80 | (ch & 63));
        }
    }
    out[used] = 0;
    if (!strcmp(out, ".") || !strcmp(out, ".."))
        return HFS_RO_UNSUPPORTED;
    return HFS_RO_OK;
}

static int parse_record(struct hfs_ro_volume *v, const uint8_t *p,
                         size_t length, struct hfs_ro_entry *e)
{
    if (length < 10)
        return HFS_RO_FORMAT;
    uint16_t key_size = be16(p), units = be16(p + 6);
    if (units > 255 || key_size != 6u + units * 2u ||
        (size_t)key_size + 4u > length)
        return HFS_RO_FORMAT;
    const uint8_t *data = p + key_size + 2;
    size_t data_size = length - key_size - 2;
    uint16_t kind = be16(data);
    if (kind == 3 || kind == 4) {
        if (units || data_size < 10 || be16(data + 8) > 255 ||
            10u + 2u * be16(data + 8) > data_size)
            return HFS_RO_FORMAT;
        return 0;
    }
    if ((kind != 1 && kind != 2) || !units ||
        data_size < (kind == 1 ? 88u : 248u))
        return HFS_RO_FORMAT;
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->id = be32(data + 8);
    e->parent = be32(p + 2);
    e->mode = be16(data + 42);
    if (!e->parent || e->id < 2)
        return HFS_RO_FORMAT;
    int rc = decode_name(p + 8, units, e->name);
    if (rc == HFS_RO_UNSUPPORTED) {
        e->unsupported = 1;
        strcpy(e->name, "<unsupported-name>");
    } else if (rc) {
        return rc;
    }
    if (kind == 2) {
        rc = parse_fork(v, data + 88, &e->data);
        if (rc)
            return rc;
        uint16_t mode_type = e->mode & 0170000;
        e->unsupported |= (mode_type && mode_type != 0100000) ||
            (data[41] & 0x20) ||
            (be32(data + 48) == 0x686c6e6bu &&
             be32(data + 52) == 0x6866732bu) ||
            (be32(data + 48) == 0x66647270u &&
             be32(data + 52) == 0x4d414353u);
    } else {
        uint16_t mode_type = e->mode & 0170000;
        e->unsupported |= mode_type && mode_type != 0040000;
    }
    return 1;
}

int hfs_ro_iter_init(const struct hfs_ro_volume *v, uint32_t parent,
                     struct hfs_ro_iterator *it)
{
    if (!v || !v->mounted || !it || parent < 2)
        return HFS_RO_ARGUMENT;
    memset(it, 0, sizeof(*it));
    it->parent = parent;

    /* The directory index is sorted by parent because catalog keys are. */
    unsigned int lo = 0, hi = v->dir_index_count;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        if (v->dir_index[mid].parent < parent)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < v->dir_index_count && v->dir_index[lo].parent == parent) {
        it->node = v->dir_index[lo].node;
        it->record = v->dir_index[lo].record;
        const uint8_t *n;
        int rc = read_catalog_node((struct hfs_ro_volume *)v, it->node, &n);
        if (rc)
            return rc;
        it->previous = be32(n + 4);
        it->indexed = 1;
        return HFS_RO_OK;
    }
    if (v->dir_index_complete) {
        it->node = 0; /* Known-empty directory. */
        it->indexed = 1;
        return HFS_RO_OK;
    }
    it->node = v->first_leaf; /* Rare overflow fallback. */
    return HFS_RO_OK;
}

int hfs_ro_iter_next(struct hfs_ro_volume *v, struct hfs_ro_iterator *it,
                     struct hfs_ro_entry *entry)
{
    if (!v || !v->mounted || !it || !entry)
        return HFS_RO_ARGUMENT;
    while (it->node) {
        if (it->node >= v->total_nodes || it->visited >= v->total_nodes)
            return HFS_RO_FORMAT;
        const uint8_t *n;
        int rc = read_catalog_node(v, it->node, &n);
        if (rc)
            return rc;
        uint16_t count = be16(n + 10);
        uint32_t next = be32(n);
        if (n[8] != 255 || n[9] != 1 || !count ||
            be32(n + 4) != it->previous || next >= v->total_nodes ||
            next == it->node || it->record > count)
            return HFS_RO_FORMAT;
        while (it->record < count) {
            const uint8_t *p;
            size_t length;
            rc = node_record(n, v->node_size, it->record++, &p, &length);
            if (rc)
                return rc;
            uint32_t key_parent;
            uint16_t key_kind;
            rc = record_parent_kind(p, length, &key_parent, &key_kind);
            if (rc)
                return rc;
            /* The fast index validated the complete leaf chain at mount, so
             * an indexed walk may stop as soon as the ordered parent changes.
             * Legacy/unindexed callers still parse every record to retain the
             * original corruption-detection behaviour. */
            if (it->indexed && key_parent > it->parent)
                return 0;
            if (!it->indexed) {
                if (it->records_seen >= v->leaf_records)
                    return HFS_RO_FORMAT;
                ++it->records_seen;
            }
            rc = parse_record(v, p, length, entry);
            if (rc < 0)
                return rc;
            if (key_parent == it->parent && rc == 1 &&
                entry->parent == it->parent)
                return 1;
        }
        if (!it->indexed && !next &&
            (it->node != v->last_leaf || it->records_seen != v->leaf_records))
            return HFS_RO_FORMAT;
        it->previous = it->node;
        it->node = next;
        it->record = 0;
        ++it->visited;
    }
    return 0;
}

int hfs_ro_find_id(struct hfs_ro_volume *v, uint32_t id,
                   struct hfs_ro_entry *entry)
{
    if (!v || !v->mounted || !entry || id < 2)
        return HFS_RO_ARGUMENT;
    if (id == HFS_RO_ROOT_ID) {
        memset(entry, 0, sizeof(*entry));
        entry->id = HFS_RO_ROOT_ID;
        entry->parent = 1;
        entry->kind = 1;
        strcpy(entry->name, "/");
        return HFS_RO_OK;
    }
    uint32_t node = v->first_leaf, previous = 0, visited = 0, seen = 0;
    while (node) {
        if (node >= v->total_nodes || visited++ >= v->total_nodes)
            return HFS_RO_FORMAT;
        const uint8_t *n;
        int rc = read_catalog_node(v, node, &n);
        if (rc)
            return rc;
        uint16_t count = be16(n + 10);
        uint32_t next = be32(n);
        if (n[8] != 255 || n[9] != 1 || !count || be32(n + 4) != previous ||
            next >= v->total_nodes || next == node)
            return HFS_RO_FORMAT;
        for (uint16_t i = 0; i < count; ++i) {
            const uint8_t *record;
            size_t length;
            rc = node_record(n, v->node_size, i, &record, &length);
            if (rc)
                return rc;
            if (seen++ >= v->leaf_records)
                return HFS_RO_FORMAT;
            rc = parse_record(v, record, length, entry);
            if (rc < 0)
                return rc;
            if (rc == 1 && entry->id == id)
                return HFS_RO_OK;
        }
        previous = node;
        node = next;
    }
    return HFS_RO_NOT_FOUND;
}

int hfs_ro_lookup(struct hfs_ro_volume *v, const char *path,
                  struct hfs_ro_entry *entry)
{
    if (!v || !v->mounted || !path || *path != '/' || !entry)
        return HFS_RO_ARGUMENT;
    memset(entry, 0, sizeof(*entry));
    entry->id = HFS_RO_ROOT_ID;
    entry->parent = 1;
    entry->kind = 1;
    strcpy(entry->name, "/");
    while (*path) {
        while (*path == '/')
            ++path;
        if (!*path)
            return entry->kind == 1 ? HFS_RO_OK : HFS_RO_NOT_DIR;
        const char *end = path;
        while (*end && *end != '/')
            ++end;
        size_t length = (size_t)(end - path);
        if (length >= HFS_RO_NAME_BYTES)
            return HFS_RO_UNSUPPORTED;
        if ((length == 1 && path[0] == '.') ||
            (length == 2 && path[0] == '.' && path[1] == '.'))
            return HFS_RO_UNSUPPORTED;
        if (entry->kind != 1)
            return HFS_RO_NOT_DIR;
        if (entry->unsupported)
            return HFS_RO_UNSUPPORTED;
        struct hfs_ro_iterator it;
        int rc = hfs_ro_iter_init(v, entry->id, &it);
        if (rc)
            return rc;
        int found = 0;
        while ((rc = hfs_ro_iter_next(v, &it, entry)) > 0) {
            if (strlen(entry->name) == length &&
                !memcmp(entry->name, path, length)) {
                found = 1;
                break;
            }
        }
        if (rc < 0)
            return rc;
        if (!found)
            return HFS_RO_NOT_FOUND;
        path = end;
    }
    return HFS_RO_OK;
}

int hfs_ro_pread(struct hfs_ro_volume *v, const struct hfs_ro_entry *entry,
                 uint64_t offset, void *buffer, size_t length, size_t *done)
{
    if (done)
        *done = 0;
    if (!v || !v->mounted || !entry || (!buffer && length) || !done)
        return HFS_RO_ARGUMENT;
    if (entry->kind == 1)
        return HFS_RO_IS_DIR;
    if (entry->kind != 2 || entry->unsupported || !inline_fork(&entry->data))
        return HFS_RO_UNSUPPORTED;
    if (offset >= entry->data.size || !length)
        return HFS_RO_OK;
    if (entry->data.size - offset < length)
        length = (size_t)(entry->data.size - offset);
    int rc = read_fork(v, &entry->data, offset, buffer, length);
    if (!rc)
        *done = length;
    return rc;
}

const char *hfs_ro_strerror(int error)
{
    switch (error) {
    case HFS_RO_OK: return "success";
    case HFS_RO_IO: return "image read failed";
    case HFS_RO_FORMAT: return "invalid or inconsistent HFS+ structure";
    case HFS_RO_UNSUPPORTED: return "feature outside this prototype's supported subset";
    case HFS_RO_DIRTY: return "volume requires consistency checking or journal replay";
    case HFS_RO_NOT_FOUND: return "exact path not found";
    case HFS_RO_NOT_DIR: return "path component is not a directory";
    case HFS_RO_IS_DIR: return "cannot read a directory as a file";
    case HFS_RO_ARGUMENT: return "invalid argument or unmounted volume";
    case HFS_RO_BUFFER: return "scratch buffer is smaller than the catalog node";
    default: return "unknown error";
    }
}
