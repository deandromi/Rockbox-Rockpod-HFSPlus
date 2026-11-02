#ifndef STORAGE_H
#define STORAGE_H
#include "mv.h"
struct storage_info { uint32_t sector_size; uint64_t num_sectors; };
void storage_get_info(int drive, struct storage_info *info);
int storage_read_sectors(sector_t start, int count, void *buf);
int storage_write_sectors(sector_t start, int count, const void *buf);
int storage_flush(void);
#endif
