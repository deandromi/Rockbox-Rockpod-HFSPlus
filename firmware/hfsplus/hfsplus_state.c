/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hfsplus_state.h"

struct hfs_state hfs_states[HFS_NUM_VOLUMES];

struct hfs_state *hfs_state_get(int volume)
{
    return (unsigned int)volume < HFS_NUM_VOLUMES ?
           &hfs_states[volume] : &hfs_states[0];
}
