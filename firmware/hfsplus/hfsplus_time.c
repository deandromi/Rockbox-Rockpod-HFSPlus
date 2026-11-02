/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HFS+/FAT timestamp conversion used by the overlay namespace and adapter.
 */
#include "hfsplus_time.h"
#include "timefuncs.h"
#include <time.h>
#include <stdint.h>

#define HFS_UNIX_EPOCH_DELTA 2082844800u

uint32_t hfs_now(void)
{
    struct tm *tm = get_time();
    if (!tm)
        return 0;
    time_t value = mktime(tm);
    return value > 0 ? (uint32_t)value : 0;
}

time_t hfs_time_to_unix(uint32_t hfs_time)
{
    if (!hfs_time)
        return 0;
    int64_t value = (int64_t)hfs_time - (int64_t)HFS_UNIX_EPOCH_DELTA;
    if (value < 0)
        value = 0;
    if (value > (int64_t)INT32_MAX)
        value = INT32_MAX;
    return (time_t)value;
}

void set_fat_time(struct fat_direntry *fatent, time_t when)
{
    uint16_t date = 0, t = 0;
    dostime_localtime(when, &date, &t);
    fatent->wrtdate = date;
    fatent->wrttime = t;
    fatent->lstaccdate = date;
}
