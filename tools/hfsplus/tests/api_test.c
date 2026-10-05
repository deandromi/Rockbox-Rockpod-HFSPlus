/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hfsplus_ro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int reads;
static int fail_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx;
    (void)off;
    (void)buf;
    (void)len;
    ++reads;
    return -1;
}

int main(void)
{
    struct hfs_ro_volume v;
    struct hfs_ro_entry e;
    struct hfs_ro_iterator it;
    uint8_t scratch[512], buf[8];
    size_t done = 99;
    memset(&v, 0, sizeof(v));
    memset(&e, 0, sizeof(e));
    assert(hfs_ro_mount(NULL, fail_read, NULL, 0, 4096, scratch, 512) == HFS_RO_ARGUMENT);
    assert(hfs_ro_mount(&v, fail_read, NULL, UINT64_MAX - 4, 4096, scratch, 512) == HFS_RO_ARGUMENT);
    assert(!reads && !v.mounted);
    assert(hfs_ro_mount(&v, fail_read, NULL, 0, 4096, scratch, 512) == HFS_RO_IO);
    assert(reads == 1 && !v.mounted);
    assert(hfs_ro_lookup(&v, "/", &e) == HFS_RO_ARGUMENT);
    assert(hfs_ro_iter_init(&v, 2, &it) == HFS_RO_ARGUMENT);
    assert(hfs_ro_pread(&v, &e, 0, buf, sizeof(buf), &done) == HFS_RO_ARGUMENT);
    assert(!done);
    v.mounted = 1;
    e.kind = 2;
    e.data.size = 1;
    e.data.blocks = 1;
    e.data.extents[0].count = 1;
    assert(!hfs_ro_pread(&v, &e, UINT64_MAX, buf, sizeof(buf), &done));
    assert(!done && reads == 1);
    assert(hfs_ro_pread(&v, &e, 0, NULL, 1, &done) == HFS_RO_ARGUMENT);
    e.kind = 1;
    assert(hfs_ro_pread(&v, &e, 0, buf, 1, &done) == HFS_RO_IS_DIR);
    hfs_ro_unmount(&v);
    assert(!v.mounted && !v.read && !v.scratch);
    puts("API argument, overflow, failed-mount and EOF checks passed");
    return 0;
}
