/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "hfsplus_rb.h"
#include "storage.h"
#include "mutex.h"
#include <string.h>

static const uint8_t *disk_image;
static size_t disk_length;
static uint32_t physical_sector;
static int out_of_bounds, writes;
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

void mutex_init(struct mutex *m) { (void)m; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }

void ata_get_info(IF_MD(int drive,) struct storage_info *info)
{
    memset(info, 0, sizeof(*info));
    info->sector_size = physical_sector;
    info->num_sectors = disk_length / physical_sector;
}

int storage_read_sectors(IF_MD(int drive,) sector_t start, int count, void *buf)
{
    if (count < 0 || start > disk_length / physical_sector ||
        (sector_t)count > disk_length / physical_sector - start) {
        ++out_of_bounds;
        return -1;
    }
    memcpy(buf, disk_image + start * physical_sector, (size_t)count * physical_sector);
    return 0;
}

int storage_write_sectors(IF_MD(int drive,) sector_t start, int count, const void *buf)
{
    (void)start; (void)count; (void)buf;
    ++writes;
    return -1;
}

void fat_empty_fat_direntry(struct fat_direntry *e) { memset(e, 0, sizeof(*e)); }

int run_rb_test(const uint8_t *image, size_t length, uint32_t sector)
{
    disk_image = image;
    disk_length = length;
    physical_sector = sector;
    out_of_bounds = writes = 0;
    hfsplus_init();
    struct partinfo parts[4];
    int multiplier = 0;
    int n = hfsplus_partitions(0, parts, 4, &multiplier);
    CHECK(n > 0 && multiplier > 0);
    int chosen = -1;
    for (int i = 0; i < n; ++i)
        if (parts[i].type == 0xaf) { chosen = i; break; }
    CHECK(chosen >= 0);
    CHECK(hfsplus_mount(0, 0, parts[chosen].start, parts[chosen].size) == 0);
    CHECK(hfsplus_mounted(0));
    struct fat_file root, dir, file;
    struct fat_filestr stream;
    struct fat_dirscan_info scan;
    struct fat_direntry e;
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(fat_is_readonly(&root));
    memset(&stream, 0, sizeof(stream));
    memset(&scan, 0, sizeof(scan));
    scan.entry = FAT_DIRSCAN_RW_VAL;
    stream.fatfilep = &root;
    int rc, found = 0;
    while ((rc = hfsplus_readdir(&stream, &scan, &e)) > 0)
        if (!strcmp((char *)e.name, ".rockbox")) {
            CHECK(!hfsplus_open(&root, e.firstcluster, &dir));
            found = 1;
            break;
        }
    CHECK(found && rc > 0);
    memset(&scan, 0, sizeof(scan));
    scan.entry = FAT_DIRSCAN_RW_VAL;
    stream.fatfilep = &dir;
    found = 0;
    while ((rc = hfsplus_readdir(&stream, &scan, &e)) > 0)
        if (!strcmp((char *)e.name, "rockbox.ipod")) {
            CHECK(!hfsplus_open(&dir, e.firstcluster, &file));
            found = 1;
            break;
        }
    CHECK(found && rc > 0);
    stream.fatfilep = &file;
    stream.lastsector = 0;
    stream.eof = false;
    uint8_t raw[4097];
    memset(raw, 0xa5, sizeof(raw));
    CHECK(hfsplus_readwrite(&stream, 1, raw + 1, false) == 1);
    CHECK(!memcmp(raw + 1, "TEST ONLY!!!", 12));
    CHECK(raw[0] == 0xa5);
    for (uint32_t i = 13; i <= sector; ++i)
        CHECK(raw[i] == 0);
    CHECK(hfsplus_readwrite(&stream, 1, raw + 1, false) == 0);
    CHECK(!hfsplus_seek(&stream, 0));
    CHECK(hfsplus_readwrite(&stream, 1, raw + 1, true) < 0);
    CHECK(hfsplus_readwrite(&stream, 1, raw + 1, false) == 1);
    CHECK(hfsplus_seek(&stream, 2) == FAT_SEEK_EOF);
    sector_t size, free;
    CHECK(hfsplus_size(0, &size, &free) && size > free);
    CHECK(!writes && !out_of_bounds);
    hfsplus_unmount(0);
    CHECK(!hfsplus_mounted(0));
    return 0;
}
