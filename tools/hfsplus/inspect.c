/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "hfsplus_ro.h"
#include "hfsplus_partition.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int image_read(void *context, uint64_t off, void *buffer, size_t length)
{
    FILE *f = context;
    if (off > INT64_MAX || fseeko(f, (off_t)off, SEEK_SET))
        return -1;
    return fread(buffer, 1, length, f) == length ? 0 : -1;
}

static int number(const char *s, uint64_t *out)
{
    char *end;
    if (*s < '0' || *s > '9')
        return -1;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end)
        return -1;
    *out = (uint64_t)n;
    return 0;
}

static void print_name(const char *s)
{
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch == 127 || ch == '\\')
            printf("\\x%02x", ch);
        else
            putchar(ch);
    }
}

int main(int argc, char **argv)
{
    uint64_t offset = 0, length = 0, skip = 0, limit = UINT64_MAX;
    int length_set = 0, scan = 0, i = 1;
    uint32_t sector = 512;
    while (i < argc && !strncmp(argv[i], "--", 2)) {
        if (!strcmp(argv[i], "--scan")) {
            scan = 1;
            ++i;
            continue;
        }
        if (i + 1 >= argc)
            goto usage;
        uint64_t n;
        if (number(argv[i + 1], &n))
            goto usage;
        if (!strcmp(argv[i], "--offset"))
            offset = n;
        else if (!strcmp(argv[i], "--length")) {
            length = n;
            length_set = 1;
        } else if (!strcmp(argv[i], "--skip"))
            skip = n;
        else if (!strcmp(argv[i], "--count"))
            limit = n;
        else if (!strcmp(argv[i], "--sector") && (n == 512 || n == 4096))
            sector = (uint32_t)n;
        else
            goto usage;
        i += 2;
    }
    if (argc - i < 2 || argc - i > 3)
        goto usage;
    const char *image = argv[i], *command = argv[i + 1];
    const char *path = argc - i == 3 ? argv[i + 2] : "/";
    if (strcmp(command, "info") && strcmp(command, "ls") && strcmp(command, "cat"))
        goto usage;
    if ((!strcmp(command, "cat") && argc - i != 3) ||
        (!strcmp(command, "info") && argc - i != 2))
        goto usage;
    struct stat st;
    if (stat(image, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
        fprintf(stderr, "Input must be a regular image file; device paths are refused.\n");
        return 2;
    }
    FILE *f = fopen(image, "rb");
    if (!f) {
        perror("image");
        return 2;
    }
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        offset > (uint64_t)st.st_size) {
        fprintf(stderr, "Invalid image or offset.\n");
        fclose(f);
        return 2;
    }
    if (!length_set)
        length = (uint64_t)st.st_size - offset;
    if (length > (uint64_t)st.st_size - offset) {
        fprintf(stderr, "Volume window extends beyond image.\n");
        fclose(f);
        return 2;
    }
    uint8_t scratch[HFS_RO_MAX_NODE_SIZE];
    struct hfs_ro_volume v;
    struct hfs_ro_entry entry;
    if (scan) {
        struct hfs_partition parts[4];
        int count = hfs_partition_scan(image_read, f, (uint64_t)st.st_size,
                                       sector, parts, 4);
        if (count <= 0) {
            fprintf(stderr, "Partition error %d\n", count);
            fclose(f);
            return 1;
        }
        int chosen = -1;
        for (int n = 0; n < count; ++n)
            if (parts[n].type == 0xaf) {
                chosen = n;
                break;
            }
        if (chosen < 0) {
            fprintf(stderr, "No HFS+ partition candidate\n");
            fclose(f);
            return 1;
        }
        offset = parts[chosen].offset;
        length = parts[chosen].length;
    }
    int rc = hfs_ro_mount(&v, image_read, f, offset, length,
                          scratch, sizeof(scratch));
    if (rc)
        goto done;
    if (!strcmp(command, "info")) {
        printf("HFS+ read-only prototype\nblock_size=%" PRIu32
               "\nblocks=%" PRIu32 "\nfree_blocks=%" PRIu32
               "\nnode_size=%u\njournaled=%u\n",
               v.block_size, v.blocks, v.free_blocks, v.node_size,
               !!(v.attributes & (1u << 13)));
        goto done;
    }
    rc = hfs_ro_lookup(&v, path, &entry);
    if (rc)
        goto done;
    if (!strcmp(command, "ls")) {
        if (entry.kind != 1) {
            rc = HFS_RO_NOT_DIR;
            goto done;
        }
        if (entry.unsupported) {
            rc = HFS_RO_UNSUPPORTED;
            goto done;
        }
        struct hfs_ro_iterator it;
        rc = hfs_ro_iter_init(&v, entry.id, &it);
        if (rc)
            goto done;
        while ((rc = hfs_ro_iter_next(&v, &it, &entry)) > 0) {
            printf("%c\t%" PRIu64 "\t", entry.kind == 1 ? 'd' : 'f',
                   entry.data.size);
            print_name(entry.name);
            if (entry.unsupported)
                printf(" [unsupported object]");
            putchar('\n');
        }
    } else {
        uint8_t buffer[16384];
        do {
            size_t request = sizeof(buffer), got;
            if (limit < request)
                request = (size_t)limit;
            rc = hfs_ro_pread(&v, &entry, skip, buffer, request, &got);
            if (rc || !got)
                break;
            if (fwrite(buffer, 1, got, stdout) != got) {
                rc = HFS_RO_IO;
                break;
            }
            skip += got;
            limit -= got;
        } while (limit);
    }
done:
    hfs_ro_unmount(&v);
    fclose(f);
    if (!rc && fflush(stdout))
        rc = HFS_RO_IO;
    if (rc)
        fprintf(stderr, "HFS+ error %d: %s\n", rc, hfs_ro_strerror(rc));
    return rc ? 1 : 0;
usage:
    fprintf(stderr, "Usage: %s [--scan] [--sector 512|4096] [--offset BYTES] [--length BYTES] "
            "[--skip BYTES] [--count BYTES] IMAGE {info|ls [PATH]|cat PATH}\n"
            "Image files only; this program cannot write to the input.\n", argv[0]);
    return 2;
}
