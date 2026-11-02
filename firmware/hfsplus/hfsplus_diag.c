/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Overlay diagnostics: stage/reason/slot recording and the public debug
 * accessors.  Validation policy lives in hfsplus_overlay_validate.c.
 */
#include "hfsplus_diag.h"
#include "hfsplus_rb.h"
#include "system.h"

int overlay_diag_fail(struct hfs_state *s, int stage, int error, int slot)
{
    s->diag.ovl_stage = stage;
    s->diag.ovl_error = error;
    s->diag.ovl_slot = slot;
    return error;
}

/* Apply a validator result to the per-volume diagnostics.  The validator
 * itself has no access to hfs_state, so validation and reporting stay
 * separable. */
int overlay_diag_validation(struct hfs_state *s,
                            const struct hfs_overlay_validation *vr)
{
    s->diag.ovl_reason = (int)vr->reason;
    if (vr->slot >= 0 && vr->slot < (int)HFS_OVL_MAX_ENTRIES) {
        const struct hfs_overlay_entry *e = &s->overlay.entries[vr->slot];
        s->diag.ovl_id = e->id;
        s->diag.ovl_parent = e->parent;
        s->diag.ovl_flags = e->flags;
        s->diag.ovl_data_start = e->data_start;
        s->diag.ovl_data_blocks = e->data_blocks;
        s->diag.ovl_kind = e->kind;
        s->diag.ovl_name_len = e->name_len;
    }
    return overlay_diag_fail(s, 11, HFS_RO_FORMAT, vr->slot);
}

int hfsplus_last_error_vol(int volume)
{
    return hfs_state_get(volume)->diag.last_error;
}

int hfsplus_last_stage_vol(int volume)
{
    return hfs_state_get(volume)->diag.last_stage;
}

int hfsplus_index_cache_used(int volume)
{
    return hfs_state_get(volume)->diag.index_cache;
}

int hfsplus_overlay_format_version(int volume)
{
    return (int)hfs_state_get(volume)->overlay.version;
}

unsigned int hfsplus_io_reads(int volume)
{
    return hfs_state_get(volume)->io.reads;
}

unsigned int hfsplus_io_writes(int volume)
{
    return hfs_state_get(volume)->io.writes;
}

unsigned int hfsplus_io_flushes(int volume)
{
    return hfs_state_get(volume)->diag.flush_ops;
}

int hfsplus_overlay_last_error_vol(int volume)
{
    return hfs_state_get(volume)->diag.ovl_error;
}

int hfsplus_overlay_last_stage_vol(int volume)
{
    return hfs_state_get(volume)->diag.ovl_stage;
}

int hfsplus_overlay_last_slot_vol(int volume)
{
    return hfs_state_get(volume)->diag.ovl_slot;
}

int hfsplus_overlay_last_reason_vol(int volume)
{
    return hfs_state_get(volume)->diag.ovl_reason;
}

/* Keep in enum order; values are append-only. */
static const char *const overlay_reason_names[] = {
    "none",
    "layout",
    "entry-id",
    "duplicate-id",
    "entry-name",
    "entry-flags",
    "entry-kind",
    "tombstone",
    "base-id",
    "data-range",
    "data-size",
    "data-unallocated",
    "data-overlap",
    "parent",
    "next-id",
};

const char *hfsplus_overlay_reason_name(int reason)
{
    if (reason < 0 ||
        (unsigned int)reason >= ARRAYLEN(overlay_reason_names))
        return "unknown";
    return overlay_reason_names[reason];
}

uint32_t hfsplus_overlay_diag_id_vol(int volume) { return hfs_state_get(volume)->diag.ovl_id; }
uint32_t hfsplus_overlay_diag_parent_vol(int volume) { return hfs_state_get(volume)->diag.ovl_parent; }
uint32_t hfsplus_overlay_diag_flags_vol(int volume) { return hfs_state_get(volume)->diag.ovl_flags; }
uint32_t hfsplus_overlay_diag_data_start_vol(int volume) { return hfs_state_get(volume)->diag.ovl_data_start; }
uint32_t hfsplus_overlay_diag_data_blocks_vol(int volume) { return hfs_state_get(volume)->diag.ovl_data_blocks; }
unsigned int hfsplus_overlay_diag_kind_vol(int volume) { return hfs_state_get(volume)->diag.ovl_kind; }
unsigned int hfsplus_overlay_diag_name_len_vol(int volume) { return hfs_state_get(volume)->diag.ovl_name_len; }

int hfsplus_overlay_last_error(void) { return hfsplus_overlay_last_error_vol(0); }
int hfsplus_overlay_last_stage(void) { return hfsplus_overlay_last_stage_vol(0); }
int hfsplus_overlay_last_slot(void) { return hfsplus_overlay_last_slot_vol(0); }
int hfsplus_overlay_last_reason(void) { return hfsplus_overlay_last_reason_vol(0); }
uint32_t hfsplus_overlay_diag_id(void) { return hfsplus_overlay_diag_id_vol(0); }
uint32_t hfsplus_overlay_diag_parent(void) { return hfsplus_overlay_diag_parent_vol(0); }
uint32_t hfsplus_overlay_diag_flags(void) { return hfsplus_overlay_diag_flags_vol(0); }
uint32_t hfsplus_overlay_diag_data_start(void) { return hfsplus_overlay_diag_data_start_vol(0); }
uint32_t hfsplus_overlay_diag_data_blocks(void) { return hfsplus_overlay_diag_data_blocks_vol(0); }
unsigned int hfsplus_overlay_diag_kind(void) { return hfsplus_overlay_diag_kind_vol(0); }
unsigned int hfsplus_overlay_diag_name_len(void) { return hfsplus_overlay_diag_name_len_vol(0); }
