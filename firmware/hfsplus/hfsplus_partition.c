/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hfsplus_partition.h"
#include <string.h>

static uint16_t be16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p + 4) << 32 | le32(p); }

static uint32_t crc(uint32_t value, const uint8_t *p, size_t length)
{
    while (length--) {
        value ^= *p++;
        for (unsigned int i = 0; i < 8; ++i)
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return value;
}

static int bounded(hfs_ro_read_fn read, void *ctx, uint64_t size,
                    uint64_t offset, void *out, size_t length)
{
    if (offset > size || length > size - offset)
        return HFS_RO_FORMAT;
    return read(ctx, offset, out, length) ? HFS_RO_IO : 0;
}

static int add(struct hfs_partition *parts, unsigned int cap, int count,
               uint64_t size, uint32_t sector, uint64_t start, uint64_t blocks,
               uint32_t unit, uint8_t type)
{
    if (!blocks || !unit || start > size / unit || blocks > size / unit - start)
        return HFS_RO_FORMAT;
    uint64_t offset = start * unit, length = blocks * unit;
    if (offset % sector || length % sector)
        return HFS_RO_UNSUPPORTED;
    for (int i = 0; i < count; ++i)
        if (offset < parts[i].offset + parts[i].length &&
            parts[i].offset < offset + length)
            return HFS_RO_FORMAT;
    if ((unsigned int)count >= cap)
        return HFS_RO_BUFFER;
    parts[count].offset = offset;
    parts[count].length = length;
    parts[count].unit = unit;
    parts[count].type = type;
    return count + 1;
}

static int probe(hfs_ro_read_fn read, void *ctx, uint64_t size,
                  uint64_t offset, uint64_t length)
{
    uint8_t b[512];
    if (length < 1536 || offset > size || length > size - offset)
        return 0;
    if (bounded(read, ctx, size, offset + 1024, b, 2))
        return 0;
    if (be16(b) == 0x482b || be16(b) == 0x4858)
        return 0xaf;
    if (bounded(read, ctx, size, offset, b, sizeof(b)))
        return 0;
    uint16_t sec = (uint16_t)((uint16_t)b[12] << 8 | b[11]);
    if (b[510] == 0x55 && b[511] == 0xaa && sec >= 512 && sec <= 4096 &&
        !(sec & (sec - 1)) && b[13] && !(b[13] & (b[13] - 1)) && b[16])
        return 0x0c;
    return 0;
}

static int apm(hfs_ro_read_fn read, void *ctx, uint64_t size, uint32_t sector,
                uint32_t unit, struct hfs_partition *parts, unsigned int cap)
{
    uint8_t b[512];
    int rc = bounded(read, ctx, size, unit, b, sizeof(b));
    if (rc)
        return rc;
    if (be16(b) != 0x504d)
        return HFS_RO_UNSUPPORTED;
    uint32_t entries = be32(b + 4);
    if (!entries || entries > 4096 || (uint64_t)(entries + 1) * unit > size)
        return HFS_RO_FORMAT;
    int count = 0;
    for (uint32_t i = 1; i <= entries; ++i) {
        rc = bounded(read, ctx, size, (uint64_t)i * unit, b, sizeof(b));
        if (rc)
            return rc;
        if (be16(b) != 0x504d || be32(b + 4) != entries)
            return HFS_RO_FORMAT;
        if (!memcmp(b + 48, "Apple_HFS", 10)) {
            if (!be32(b + 8))
                return HFS_RO_FORMAT;
            count = add(parts, cap, count, size, sector, be32(b + 8),
                         be32(b + 12), unit, 0xaf);
            if (count < 0)
                return count;
        }
    }
    return count;
}

static int gpt(hfs_ro_read_fn read, void *ctx, uint64_t size, uint32_t sector,
                uint32_t unit, struct hfs_partition *parts, unsigned int cap)
{
    static const uint8_t hfs_guid[16] =
        {0x00,0x53,0x46,0x48,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
    static const uint8_t data_guid[16] =
        {0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    uint8_t b[512];
    int rc = bounded(read, ctx, size, unit, b, sizeof(b));
    if (rc)
        return rc;
    if (memcmp(b, "EFI PART", 8))
        return HFS_RO_UNSUPPORTED;
    uint32_t hs = le32(b + 12), expected = le32(b + 16);
    if (le32(b + 8) != 0x10000 || hs < 92 || hs > sizeof(b) ||
        le64(b + 24) != 1 || le64(b + 32) >= size / unit)
        return HFS_RO_FORMAT;
    memset(b + 16, 0, 4);
    if (~crc(UINT32_MAX, b, hs) != expected)
        return HFS_RO_FORMAT;
    uint64_t first = le64(b + 40), last = le64(b + 48), table = le64(b + 72);
    uint32_t entries = le32(b + 80), es = le32(b + 84), table_crc = le32(b + 88);
    if (first < 2 || first > last || last >= size / unit || table < 2 ||
        table >= first || !entries || entries > 4096 || es < 128 || es > 512 ||
        (es & (es - 1)) || (uint64_t)entries * es > (first - table) * unit)
        return HFS_RO_FORMAT;
    uint64_t bytes = (uint64_t)entries * es, offset = table * unit;
    uint32_t value = UINT32_MAX;
    while (bytes) {
        size_t n = bytes > sizeof(b) ? sizeof(b) : (size_t)bytes;
        rc = bounded(read, ctx, size, offset, b, n);
        if (rc)
            return rc;
        value = crc(value, b, n);
        bytes -= n;
        offset += n;
    }
    if (~value != table_crc)
        return HFS_RO_FORMAT;
    int count = 0;
    for (uint32_t i = 0; i < entries; ++i) {
        rc = bounded(read, ctx, size, table * unit + (uint64_t)i * es, b, es);
        if (rc)
            return rc;
        int type = !memcmp(b, hfs_guid, 16) ? 0xaf :
                   !memcmp(b, data_guid, 16) ? 0x0c : 0;
        if (!type)
            continue;
        uint64_t begin = le64(b + 32), end = le64(b + 40);
        if (begin < first || begin > end || end > last)
            return HFS_RO_FORMAT;
        count = add(parts, cap, count, size, sector, begin, end - begin + 1,
                     unit, (uint8_t)type);
        if (count < 0)
            return count;
    }
    return count;
}

int hfs_partition_scan(hfs_ro_read_fn read, void *ctx, uint64_t size,
                       uint32_t sector, struct hfs_partition *parts,
                       unsigned int cap)
{
    if (!read || !parts || !cap || sector < 512 || sector > 4096 ||
        (sector & (sector - 1)) || size < 2048)
        return HFS_RO_ARGUMENT;
    uint8_t mbr[512];
    int rc = bounded(read, ctx, size, 0, mbr, sizeof(mbr));
    if (rc)
        return rc;
    if (be16(mbr) == 0x4552) {
        uint32_t unit = be16(mbr + 2);
        if (unit < 512 || unit > 4096 || (unit & (unit - 1)))
            return HFS_RO_UNSUPPORTED;
        return apm(read, ctx, size, sector, unit, parts, cap);
    }
    /* iPod Classic exposes 4 KiB virtual partition-table units even when
     * the ATA layer reports 512-byte logical sectors.  Try 4096 first so
     * an accidental plausible signature at the 512-byte interpretation
     * cannot shadow the real iPod partition.  The reported sector size
     * remains the fallback for ordinary disks. */
    uint32_t units[2] = { 4096, sector };
    int raw = probe(read, ctx, size, 0, size);
    if (raw)
        return add(parts, cap, 0, size, sector, 0, size / sector, sector, (uint8_t)raw);
    if (mbr[510] != 0x55 || mbr[511] != 0xaa)
        return apm(read, ctx, size, sector, 512, parts, cap);
    int protective = 0;
    for (unsigned int i = 0; i < 4; ++i)
        protective |= mbr[446 + 16 * i + 4] == 0xee;
    if (protective) {
        for (unsigned int u = 0; u < 2; ++u) {
            rc = gpt(read, ctx, size, sector, units[u], parts, cap);
            if (rc != HFS_RO_UNSUPPORTED)
                return rc;
        }
        return HFS_RO_UNSUPPORTED;
    }
    /* The iPod Classic USB stack writes MBR start/size values in 4096-byte
     * virtual-sector units.  On the ATA side the filesystem probe can run
     * before Rockbox has established that virtual-sector multiplier, so a
     * perfectly valid Apple_HFS entry can fail the byte-level probe even
     * though its MBR geometry is unambiguous.
     *
     * Prefer an explicit Apple_HFS (0xaf) MBR entry interpreted in the
     * iPod's 4096-byte virtual units.  hfsplus_mount() independently checks
     * the H+/HX signature and complete volume structure, so accepting the
     * partition geometry here does not bypass HFS+ validation. */
    for (unsigned int i = 0; i < 4; ++i) {
        const uint8_t *p = mbr + 446 + i * 16;
        uint64_t begin = le32(p + 8), blocks = le32(p + 12);
        if (p[4] != 0xaf || !begin || !blocks)
            continue;
        int count = add(parts, cap, 0, size, sector, begin, blocks, 4096, 0xaf);
        if (count > 0)
            return count;
        if (count != HFS_RO_FORMAT)
            return count;
        /* A size-reporting mismatch is still worth falling through to the
         * generic scanner, which may be able to interpret the device using
         * its reported logical-sector size. */
    }

    for (unsigned int u = 0; u < 2; ++u) {
        int count = 0, found = 0;
        for (unsigned int i = 0; i < 4; ++i) {
            const uint8_t *p = mbr + 446 + i * 16;
            uint64_t begin = le32(p + 8), blocks = le32(p + 12);
            if (!p[4] || p[4] == 5 || p[4] == 15 || !begin || !blocks)
                continue;
            rc = probe(read, ctx, size, begin * units[u], blocks * units[u]);
            if (rc)
                found = 1;
            count = add(parts, cap, count, size, sector, begin, blocks, units[u], p[4]);
            if (count < 0)
                break;
        }
        if (count > 0 && found)
            return count;
    }
    return HFS_RO_UNSUPPORTED;
}
