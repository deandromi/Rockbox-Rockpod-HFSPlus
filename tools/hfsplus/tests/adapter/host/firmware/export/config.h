#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#define IPOD_6G 1
#define MEMORYSIZE 64
#define NUM_VOLUMES 1
#define NUM_DRIVES 1
#define MAX_PARTITIONS_PER_DRIVE 4
#define STORAGE_ATA 1
#define CONFIG_STORAGE STORAGE_ATA
#define HAVE_LBA48 1
#define ROCKBOX_LITTLE_ENDIAN 1
#define HAVE_HFSPLUS_RO 1
#define HAVE_STORAGE_FLUSH 1
#define MODEL_NAME "host"
#define TARGET_ID 71
#endif
