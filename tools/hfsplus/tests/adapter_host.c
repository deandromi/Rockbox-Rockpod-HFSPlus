/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host-side HFS+ adapter harness.  This is NOT firmware: it links the real
 * hfsplus_rb.c/hfsplus_ro.c/hfsplus_partition.c against a small in-memory
 * ATA stub so overlay ownership, crash ordering, ENOSPC and namespace paths
 * can be exercised deterministically on a development machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "config.h"
#include "hfsplus_rb.h"
#include "storage.h"
#include "mutex.h"
#include "fs_attr.h"

static uint8_t *disk;
static size_t disk_size;
static uint32_t sector_size = 512;
static unsigned long write_count;
static unsigned long fail_after;
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

void storage_get_info(int drive, struct storage_info *info)
{
    (void)drive;
    info->sector_size = sector_size;
    info->num_sectors = disk_size / sector_size;
}

int storage_read_sectors(sector_t start, int count, void *buf)
{
    if (count < 0 || start > disk_size / sector_size ||
        (sector_t)count > disk_size / sector_size - start)
        return -1;
    memcpy(buf, disk + start * sector_size, (size_t)count * sector_size);
    return 0;
}

int storage_write_sectors(sector_t start, int count, const void *buf)
{
    if (fail_after && ++write_count >= fail_after)
        return -1;
    if (count < 0 || start > disk_size / sector_size ||
        (sector_t)count > disk_size / sector_size - start)
        return -1;
    memcpy(disk + start * sector_size, buf, (size_t)count * sector_size);
    return 0;
}

int storage_flush(void) { return 0; }
void mutex_init(struct mutex *m) { (void)m; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
void fat_empty_fat_direntry(struct fat_direntry *e) { memset(e, 0, sizeof(*e)); }

/* Deterministic clock for timestamps; the host's localtime/strftime is not
 * part of the adapter under test. */
struct tm *get_time(void)
{
    static struct tm t;
    if (!t.tm_year) {
        t.tm_year = 126; t.tm_mon = 0; t.tm_mday = 1;
        t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0;
    }
    return &t;
}

void dostime_localtime(time_t value, uint16_t *dosdate, uint16_t *dostime)
{
    struct tm tmv;
    struct tm *t = localtime_r(&value, &tmv);
    if (!t) { *dosdate = 0x21; *dostime = 0; return; }
    int year = t->tm_year + 1900;
    if (year < 1980) year = 1980;
    *dosdate = (uint16_t)(((year - 1980) << 9) | ((t->tm_mon + 1) << 5) |
                          t->tm_mday);
    *dostime = (uint16_t)((t->tm_hour << 11) | (t->tm_min << 5) |
                          (t->tm_sec / 2));
}

static int find(struct fat_file *dir, const char *name,
                struct fat_direntry *out)
{
    struct fat_filestr st;
    struct fat_dirscan_info sc;
    struct fat_direntry e;
    int rc;
    memset(&st, 0, sizeof(st));
    memset(&sc, 0, sizeof(sc));
    sc.entry = FAT_DIRSCAN_RW_VAL;
    st.fatfilep = dir;
    while ((rc = hfsplus_readdir(&st, &sc, &e)) > 0) {
        if (!strcmp((char *)e.name, name)) {
            if (out)
                *out = e;
            return 1;
        }
    }
    return rc;
}

static int open_path(const char *first, const char *second,
                     struct fat_file *root, struct fat_file *out)
{
    struct fat_direntry e;
    struct fat_file dir;
    if (find(root, first, &e) <= 0)
        return -1;
    if (!second)
        return hfsplus_open(root, e.firstcluster, out);
    if (hfsplus_open(root, e.firstcluster, &dir))
        return -1;
    if (find(&dir, second, &e) <= 0)
        return -1;
    return hfsplus_open(&dir, e.firstcluster, out);
}

static int write_file(struct fat_file *file, const char *text)
{
    struct fat_filestr fs;
    uint8_t buf[512];
    size_t len = strlen(text) + 1;
    memset(&fs, 0, sizeof(fs));
    memset(buf, 0, sizeof(buf));
    memcpy(buf, text, len);
    fs.fatfilep = file;
    if (hfsplus_readwrite(&fs, 1, buf, true) != 1)
        return -1;
    return hfsplus_closewrite(&fs, (uint32_t)len, NULL);
}

static int read_file(struct fat_file *file, char *out, size_t cap)
{
    struct fat_filestr fs;
    uint8_t buf[512];
    memset(&fs, 0, sizeof(fs));
    fs.fatfilep = file;
    if (hfsplus_seek(&fs, 0) < 0)
        return -1;
    memset(buf, 0, sizeof(buf));
    if (hfsplus_readwrite(&fs, 1, buf, false) < 0)
        return -1;
    if (cap)
        snprintf(out, cap, "%s", (char *)buf);
    return 0;
}

static int mount_image(void)
{
    struct partinfo parts[4];
    int multiplier = 0;
    int n;
    hfsplus_init();
    n = hfsplus_partitions(0, parts, 4, &multiplier);
    if (n <= 0)
        return -1;
    for (int i = 0; i < n; ++i)
        if (parts[i].type == 0xaf)
            return hfsplus_mount(0, 0, parts[i].start, parts[i].size);
    return -1;
}

static int load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f || fseek(f, 0, SEEK_END))
        return -1;
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET))
        return -1;
    disk = malloc((size_t)n);
    if (!disk || fread(disk, 1, (size_t)n, f) != (size_t)n)
        return -1;
    fclose(f);
    disk_size = (size_t)n;
    return 0;
}

static int save_image(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(disk, 1, disk_size, f) != disk_size)
        return -1;
    return fclose(f);
}

static int test_rw(void)
{
    struct fat_file root, dir, file;
    struct fat_direntry e;
    char text[64];
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, "d", ATTR_DIRECTORY, &dir, NULL) == 0);
    CHECK(hfsplus_create_file(&dir, "hello.txt", ATTR_ARCHIVE, &file, NULL) == 0);
    CHECK(write_file(&file, "hello world") == 0);
    CHECK(read_file(&file, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "hello world"));
    CHECK(hfsplus_rename(&dir, &file, (const unsigned char *)"renamed.txt") == 0);
    CHECK(hfsplus_closewrite(&(struct fat_filestr){ .fatfilep = &file }, 12,
                             NULL) == 0);
    hfsplus_unmount(0);
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(open_path("d", "renamed.txt", &root, &file) == 0);
    CHECK(read_file(&file, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "hello world"));
    CHECK(find(&root, "d", &e) > 0);
    CHECK(hfsplus_open(&root, e.firstcluster, &dir) == 0);
    CHECK(hfsplus_remove(&file, FAT_RM_ALL) == 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_readonly(void)
{
    struct fat_file root, file;
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(!hfsplus_writable(&root));
    CHECK(hfsplus_create_file(&root, "blocked.txt", ATTR_ARCHIVE, &file,
                              NULL) < 0);
    CHECK(open_path("Music", "empty.bin", &root, &file) == 0);
    CHECK(!hfsplus_can_write_existing(&file));
    hfsplus_unmount(0);
    return 0;
}

static int test_reserved(void)
{
    struct fat_file root, file;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, ".rockpod-rw", ATTR_ARCHIVE, &file,
                              NULL) == HFS_RO_ARGUMENT);
    hfsplus_unmount(0);
    return 0;
}

static int test_enospc(void)
{
    struct fat_file root, file;
    char name[32];
    int rc = 0;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    unsigned int flush_before = hfsplus_io_flushes(0);
    for (int i = 0; i < 2048; ++i) {
        snprintf(name, sizeof(name), "f%04d", i);
        rc = hfsplus_create_file(&root, name, ATTR_ARCHIVE, &file, NULL);
        CHECK(rc == 0);
    }
    unsigned int flush_after = hfsplus_io_flushes(0);
    /* One entry sync per create, plus one header reserve per 64 slots.  The
     * old per-slot high-water update cost a second flush on every create. */
    CHECK(flush_after - flush_before <= 2048u + 64u);
    snprintf(name, sizeof(name), "overflow");
    rc = hfsplus_create_file(&root, name, ATTR_ARCHIVE, &file, NULL);
    CHECK(rc == FAT_RC_ENOSPC);
    hfsplus_unmount(0);
    return 0;
}

static int test_rename_parent(void)
{
    struct fat_file root, dir, child, pre;
    struct fat_direntry e;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, "d", ATTR_DIRECTORY, &dir, NULL) == 0);
    CHECK(hfsplus_create_file(&dir, "child.txt", ATTR_ARCHIVE, &child, NULL) == 0);
    CHECK(hfsplus_remove(&child, FAT_RM_ALL) == 0);
    CHECK(hfsplus_create_file(&dir, "pre.txt", ATTR_ARCHIVE, &pre, NULL) == 0);
    CHECK(hfsplus_rename(&root, &dir, (const unsigned char *)"d2") == 0);
    hfsplus_unmount(0);
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(find(&root, "d2", &e) > 0);
    CHECK(hfsplus_open(&root, e.firstcluster, &dir) == 0);
    CHECK(find(&dir, "pre.txt", &e) > 0);
    CHECK(find(&dir, "child.txt", &e) <= 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_duplicate(void)
{
    struct fat_file root, a, b;
    char text[64];
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(open_path("Music", "empty.bin", &root, &a) == 0);
    CHECK(open_path("Music", "empty.bin", &root, &b) == 0);
    CHECK(write_file(&a, "A") == 0);
    CHECK(write_file(&b, "B") == 0);
    CHECK(a.hfs_overlay && b.hfs_overlay);
    CHECK(a.firstcluster == b.firstcluster);
    CHECK(read_file(&a, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "B"));
    hfsplus_unmount(0);
    return 0;
}

static int test_twofiles(void)
{
    struct fat_file root, a, b;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, "a.txt", ATTR_ARCHIVE, &a, NULL) == 0);
    CHECK(hfsplus_create_file(&root, "b.txt", ATTR_ARCHIVE, &b, NULL) == 0);
    CHECK(write_file(&a, "AAAAAAAA") == 0);
    CHECK(write_file(&b, "BBBBBBBB") == 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_timestamps(void)
{
    struct fat_file root, file;
    struct fat_direntry e;
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(find(&root, "Music", &e) > 0);
    CHECK(e.wrtdate != 0 && e.wrttime != 0);
    CHECK(hfsplus_create_file(&root, "stamp.txt", ATTR_ARCHIVE, &file,
                              &e) == 0);
    CHECK(e.wrtdate != 0 && e.wrttime != 0);
    /* The host clock is fixed at 2026; a Unix/HFS epoch mix would show 1980. */
    CHECK((e.wrtdate >> 9) + 1980 >= 2020);
    hfsplus_unmount(0);
    return 0;
}

static int test_legacy_duplicate(void)
{
    struct fat_file root, file;
    struct fat_direntry e;
    char text[64];
    int count = 0;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    {
        struct fat_filestr st;
        struct fat_dirscan_info sc;
        memset(&st, 0, sizeof(st));
        memset(&sc, 0, sizeof(sc));
        sc.entry = FAT_DIRSCAN_RW_VAL;
        st.fatfilep = &root;
        while (hfsplus_readdir(&st, &sc, &e) > 0)
            if (!strcmp((char *)e.name, "a.txt"))
                ++count;
    }
    CHECK(count == 1);
    CHECK(open_path("a.txt", NULL, &root, &file) == 0);
    CHECK(read_file(&file, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "BBBBBBBB"));
    hfsplus_unmount(0);
    return 0;
}

static int test_legacy_baseid(void)
{
    struct fat_file root, file;
    char text[64];
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(open_path("a.txt", NULL, &root, &file) == 0);
    CHECK(read_file(&file, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "AAAAAAAA"));
    hfsplus_unmount(0);
    return 0;
}

static int test_verify(void)
{
    struct fat_file root, file;
    char text[64];
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(find(&root, "recovered.txt", NULL) > 0);
    CHECK(open_path("recovered.txt", NULL, &root, &file) == 0);
    CHECK(read_file(&file, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "recovered"));
    hfsplus_unmount(0);
    return 0;
}

static int test_recovery(void)
{
    struct fat_file root, file;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, "recovered.txt", ATTR_ARCHIVE, &file,
                              NULL) == 0);
    CHECK(write_file(&file, "recovered") == 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_fault(void)
{
    struct fat_file root, file;
    int rc;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    rc = hfsplus_create_file(&root, "fault.bin", ATTR_ARCHIVE, &file, NULL);
    CHECK(rc < 0 || write_file(&file, "must not corrupt") < 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_diag(void)
{
    int rc = mount_image();
    int reason = hfsplus_overlay_last_reason();
    printf("mount=%d ready=%d stage=%d error=%d slot=%d reason=%d "
           "reason_name=%s id=%08lx\n",
           rc, hfsplus_overlay_ready(0) ? 1 : 0,
           hfsplus_overlay_last_stage(), hfsplus_overlay_last_error(),
           hfsplus_overlay_last_slot(), reason,
           hfsplus_overlay_reason_name(reason),
           (unsigned long)hfsplus_overlay_diag_id());
    printf("index_cache=%d overlay_version=%d\n",
           hfsplus_index_cache_used(0), hfsplus_overlay_format_version(0));
    printf("io_reads=%u io_writes=%u io_flushes=%u\n",
           hfsplus_io_reads(0), hfsplus_io_writes(0), hfsplus_io_flushes(0));
    printf("high_water=%u op_create=%u op_write=%u op_close=%u "
           "op_rename=%u op_remove=%u op_settings=%u\n",
           hfsplus_overlay_high_water(0),
           hfsplus_io_op_count(0, HFSPLUS_OP_CREATE),
           hfsplus_io_op_count(0, HFSPLUS_OP_WRITE),
           hfsplus_io_op_count(0, HFSPLUS_OP_CLOSE),
           hfsplus_io_op_count(0, HFSPLUS_OP_RENAME),
           hfsplus_io_op_count(0, HFSPLUS_OP_REMOVE),
           hfsplus_io_op_count(0, HFSPLUS_OP_SETTINGS));
    hfsplus_unmount(0);
    return 0;
}

static int test_ops(void)
{
    struct fat_file root, file;
    unsigned int create, write, close, rename, remove, settings;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    create = hfsplus_io_op_count(0, HFSPLUS_OP_CREATE);
    write = hfsplus_io_op_count(0, HFSPLUS_OP_WRITE);
    close = hfsplus_io_op_count(0, HFSPLUS_OP_CLOSE);
    rename = hfsplus_io_op_count(0, HFSPLUS_OP_RENAME);
    remove = hfsplus_io_op_count(0, HFSPLUS_OP_REMOVE);
    settings = hfsplus_io_op_count(0, HFSPLUS_OP_SETTINGS);
    CHECK(hfsplus_create_file(&root, "config.cfg", ATTR_ARCHIVE, &file,
                              NULL) == 0);
    CHECK(write_file(&file, "settings") == 0);
    CHECK(hfsplus_rename(&root, &file,
                         (const unsigned char *)"config.cfg.new") == 0);
    CHECK(hfsplus_remove(&file, FAT_RM_ALL) == 0);
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_CREATE) == create + 1);
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_WRITE) == write + 1);
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_CLOSE) == close + 1);
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_RENAME) == rename + 1);
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_REMOVE) == remove + 1);
    /* Create, data write, close, rename and remove are all attributable to
     * the settings basename. */
    CHECK(hfsplus_io_op_count(0, HFSPLUS_OP_SETTINGS) >= settings + 4);
    CHECK(hfsplus_io_op_flushes(0, HFSPLUS_OP_SETTINGS) > 0);
    printf("op_create=%u op_write=%u op_close=%u op_rename=%u "
           "op_remove=%u op_settings=%u flush_settings=%u\n",
           hfsplus_io_op_count(0, HFSPLUS_OP_CREATE),
           hfsplus_io_op_count(0, HFSPLUS_OP_WRITE),
           hfsplus_io_op_count(0, HFSPLUS_OP_CLOSE),
           hfsplus_io_op_count(0, HFSPLUS_OP_RENAME),
           hfsplus_io_op_count(0, HFSPLUS_OP_REMOVE),
           hfsplus_io_op_count(0, HFSPLUS_OP_SETTINGS),
           hfsplus_io_op_flushes(0, HFSPLUS_OP_SETTINGS));
    hfsplus_unmount(0);
    return 0;
}

static int test_grow_copy(void)
{
    struct fat_file root, file;
    struct fat_filestr fs;
    const size_t first = 80 * 512;   /* 40 KiB: more than two copy chunks */
    const size_t second = 512;
    uint8_t *in = malloc(first + second);
    uint8_t *out = malloc(first + second);
    int rc;
    CHECK(in && out);
    for (size_t i = 0; i < first; ++i)
        in[i] = (uint8_t)(i * 7u);
    memset(in + first, 0xaa, second);
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(hfsplus_create_file(&root, "big.bin", ATTR_ARCHIVE, &file, NULL) == 0);
    memset(&fs, 0, sizeof(fs));
    fs.fatfilep = &file;
    CHECK(hfsplus_readwrite(&fs, first / 512, in, true) ==
          (long)(first / 512));
    CHECK(hfsplus_closewrite(&fs, (uint32_t)first, NULL) == 0);
    /* Extend without rewriting the old bytes, forcing a realloc copy. */
    CHECK(hfsplus_seek(&fs, first / 512) == 0);
    CHECK(hfsplus_readwrite(&fs, second / 512, in + first, true) == 1);
    CHECK(hfsplus_closewrite(&fs, (uint32_t)(first + second), NULL) == 0);
    /* Exercise the same entry after a remount. */
    hfsplus_unmount(0);
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(open_path("big.bin", NULL, &root, &file) == 0);
    memset(&fs, 0, sizeof(fs));
    fs.fatfilep = &file;
    CHECK(hfsplus_seek(&fs, 0) == 0);
    rc = (int)hfsplus_readwrite(&fs, (first + second) / 512, out, false);
    CHECK(rc == (int)((first + second) / 512));
    CHECK(!memcmp(out, in, first + second));
    hfsplus_unmount(0);
    free(in);
    free(out);
    return 0;
}

/* Enumerate a directory and count the two fixture files.  "x" is reserved for
 * the high-water test's corrupt slot-500 entry, which must stay invisible when
 * the v3 bound excludes it. */
static int test_readdir(void)
{
    struct fat_file root;
    struct fat_direntry e;
    int a = 0, b = 0, x = 0;
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    CHECK(find(&root, "a.txt", &e) == 1);
    {
        struct fat_filestr st;
        struct fat_dirscan_info sc;
        memset(&st, 0, sizeof(st));
        memset(&sc, 0, sizeof(sc));
        sc.entry = FAT_DIRSCAN_RW_VAL;
        st.fatfilep = &root;
        while (hfsplus_readdir(&st, &sc, &e) > 0) {
            if (!strcmp((char *)e.name, "a.txt"))
                ++a;
            else if (!strcmp((char *)e.name, "b.txt"))
                ++b;
            else if (!strcmp((char *)e.name, "x"))
                ++x;
        }
    }
    CHECK(a == 1 && b == 1 && x == 0);
    hfsplus_unmount(0);
    return 0;
}

static int test_find(void)
{
    struct fat_file root, file;
    struct fat_filestr st;
    struct fat_direntry e;
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    memset(&st, 0, sizeof(st));
    st.fatfilep = &root;

    CHECK(hfsplus_find(&st, "Music", &e) == 1);
    CHECK(!strcmp((char *)e.name, "Music"));
    CHECK(hfsplus_find(&st, "missing.txt", &e) == 0);
    CHECK(hfsplus_find(&st, "missing.txt", &e) == 0); /* negative cache */

    CHECK(hfsplus_create_file(&root, "new.txt", ATTR_ARCHIVE, &file,
                              NULL) == 0);
    CHECK(hfsplus_find(&st, "new.txt", &e) == 1);
    CHECK(hfsplus_rename(&root, &file,
                         (const unsigned char *)"renamed.txt") == 0);
    CHECK(hfsplus_find(&st, "new.txt", &e) == 0);
    CHECK(hfsplus_find(&st, "renamed.txt", &e) == 1);
    CHECK(hfsplus_remove(&file, FAT_RM_ALL) == 0);
    CHECK(hfsplus_find(&st, "renamed.txt", &e) == 0);

    hfsplus_unmount(0);
    return 0;
}

static int count_children(struct fat_file *dir, const char *prefix)
{
    struct fat_filestr st;
    struct fat_dirscan_info sc;
    struct fat_direntry e;
    int count = 0;
    memset(&st, 0, sizeof(st));
    memset(&sc, 0, sizeof(sc));
    sc.entry = FAT_DIRSCAN_RW_VAL;
    st.fatfilep = dir;
    while (hfsplus_readdir(&st, &sc, &e) > 0) {
        if (!strncmp((char *)e.name, prefix, strlen(prefix)))
            ++count;
    }
    return count;
}

static int test_readdir_buckets(void)
{
    struct fat_file root, dir, file;
    char name[32];
    CHECK(mount_image() == 0);
    CHECK(hfsplus_overlay_ready(0));
    CHECK(!hfsplus_open_root(0, &root));
    for (int d = 0; d < 3; ++d) {
        snprintf(name, sizeof(name), "dir%d", d);
        CHECK(hfsplus_create_file(&root, name, ATTR_DIRECTORY, &dir, NULL) == 0);
        for (int i = 0; i < 40; ++i) {
            snprintf(name, sizeof(name), "d%d_f%02d.txt", d, i);
            CHECK(hfsplus_create_file(&dir, name, ATTR_ARCHIVE, &file,
                                      NULL) == 0);
        }
    }
    for (int d = 0; d < 3; ++d) {
        snprintf(name, sizeof(name), "dir%d", d);
        CHECK(open_path(name, NULL, &root, &dir) == 0);
        snprintf(name, sizeof(name), "d%d_", d);
        CHECK(count_children(&dir, name) == 40);
    }
    CHECK(count_children(&root, "d") == 3);
    hfsplus_unmount(0);
    CHECK(mount_image() == 0);
    CHECK(!hfsplus_open_root(0, &root));
    for (int d = 0; d < 3; ++d) {
        snprintf(name, sizeof(name), "dir%d", d);
        CHECK(open_path(name, NULL, &root, &dir) == 0);
        snprintf(name, sizeof(name), "d%d_", d);
        CHECK(count_children(&dir, name) == 40);
    }
    hfsplus_unmount(0);
    return 0;
}

int main(int argc, char **argv)
{
    const char *test;
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s IMAGE OUTPUT TEST [FAIL_AFTER]\n", argv[0]);
        return 2;
    }
    if (load_image(argv[1]))
        return 2;
    test = argv[3];
    if (argc == 5)
        fail_after = strtoul(argv[4], NULL, 10);
    int rc;
    if (!strcmp(test, "rw")) rc = test_rw();
    else if (!strcmp(test, "readonly")) rc = test_readonly();
    else if (!strcmp(test, "reserved")) rc = test_reserved();
    else if (!strcmp(test, "enospc")) rc = test_enospc();
    else if (!strcmp(test, "rename-parent")) rc = test_rename_parent();
    else if (!strcmp(test, "duplicate")) rc = test_duplicate();
    else if (!strcmp(test, "recovery")) rc = test_recovery();
    else if (!strcmp(test, "verify")) rc = test_verify();
    else if (!strcmp(test, "twofiles")) rc = test_twofiles();
    else if (!strcmp(test, "timestamps")) rc = test_timestamps();
    else if (!strcmp(test, "legacy-duplicate")) rc = test_legacy_duplicate();
    else if (!strcmp(test, "legacy-baseid")) rc = test_legacy_baseid();
    else if (!strcmp(test, "fault")) rc = test_fault();
    else if (!strcmp(test, "diag")) rc = test_diag();
    else if (!strcmp(test, "ops")) rc = test_ops();
    else if (!strcmp(test, "grow-copy")) rc = test_grow_copy();
    else if (!strcmp(test, "readdir")) rc = test_readdir();
    else if (!strcmp(test, "find")) rc = test_find();
    else if (!strcmp(test, "readdir-buckets")) rc = test_readdir_buckets();
    else { fprintf(stderr, "unknown test\n"); return 2; }
    if (save_image(argv[2]))
        return 2;
    if (rc)
        fprintf(stderr, "%s failed at line %d\n", test, rc);
    return rc ? 1 : 0;
}
