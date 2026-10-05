/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int run_rb_test(const uint8_t *image, size_t length, uint32_t sector);
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END))
        return 2;
    long size = ftell(f);
    if (size < 1 || fseek(f, 0, SEEK_SET))
        return 2;
    uint8_t *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size)
        return 2;
    fclose(f);
    int rc = run_rb_test(bytes, (size_t)size, (uint32_t)strtoul(argv[2], NULL, 10));
    free(bytes);
    if (rc)
        fprintf(stderr, "Adapter assertion failed at line %d\n", rc);
    return rc ? 1 : 0;
}
