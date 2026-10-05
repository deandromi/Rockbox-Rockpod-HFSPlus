/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockpod Contributors
 *
 * S5L8702 H.264 Hardware Video Decoder - Proof of Concept
 *
 * This plugin probes the undocumented hardware H.264 video decoder
 * discovered via reverse engineering of the iPod Classic 6G firmware.
 * It enables the decoder clock gate, reads hardware status registers,
 * initializes the video post-processing pipeline, and logs all
 * register values for analysis.
 *
 * This is a diagnostic/bringup tool for hardware validation.
 * It does NOT decode actual video.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "plugin.h"

/* We need direct access to PWRCON and other SoC registers.
 * plugin.h includes system.h which includes config.h, giving us
 * CONFIG_CPU == S5L8702. s5l87xx.h provides REG32_PTR_T, PWRCON(),
 * VIC macros, and the I/O base addresses. */
#include "s5l87xx.h"

/*
 * ========================================================================
 *  S5L8702 Video Decoder Register Map
 *
 *  Discovered via reverse engineering of retailos.bin (iPod Classic 6G).
 *  See report.md for full details.
 * ========================================================================
 */

/* --- H.264 Hardware Decoder (6 sub-blocks) --- */

#define VDEC_MAIN_BASE      0x39600000  /* Global interrupt status */
#define VDEC_CORE_BASE      0x39610000  /* SPS/PPS, slice header, MB config */
#define VDEC_BSDMA_BASE     0x39630000  /* NAL unit bitstream DMA engine */
#define VDEC_XFORM_BASE     0x39641000  /* IDCT / transform unit */
#define VDEC_STATUS_BASE    0x39650000  /* Decoder status, IRQ enable */
#define VDEC_OUTDMA_BASE    0x39660000  /* Decoded frame output DMA */

/* Main Control (0x39600000) */
#define VDEC_MAIN_STATUS    (*(REG32_PTR_T)(VDEC_MAIN_BASE + 0x00))

/* Decoder Core (0x39610000) */
#define VDEC_CORE_SLICECFG  (*(REG32_PTR_T)(VDEC_CORE_BASE + 0x10))
#define VDEC_CORE_SLICEHDR  (*(REG32_PTR_T)(VDEC_CORE_BASE + 0x14))
#define VDEC_CORE_CMD       (*(REG32_PTR_T)(VDEC_CORE_BASE + 0x34))
#define VDEC_CORE_MBCFG     (*(REG32_PTR_T)(VDEC_CORE_BASE + 0x9C))
#define VDEC_CORE_NUMREF    (*(REG32_PTR_T)(VDEC_CORE_BASE + 0xA0))
#define VDEC_CORE_RSV0      (*(REG32_PTR_T)(VDEC_CORE_BASE + 0xA4))
#define VDEC_CORE_RSV1      (*(REG32_PTR_T)(VDEC_CORE_BASE + 0xA8))
#define VDEC_CORE_IRQCLR    (*(REG32_PTR_T)(VDEC_CORE_BASE + 0x10000))

/* Bitstream DMA (0x39630000) */
#define VDEC_BSDMA_CFG      (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x04))
#define VDEC_BSDMA_IRQCLR   (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x100))
#define VDEC_BSDMA_START    (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x110))
#define VDEC_BSDMA_CLEAR    (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x30000))
#define VDEC_BSDMA_SRCADDR  (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x30018))
#define VDEC_BSDMA_ENDADDR  (*(REG32_PTR_T)(VDEC_BSDMA_BASE + 0x3001C))

/* Transform Unit (0x39641000) */
#define VDEC_XFORM_CFG0     (*(REG32_PTR_T)(VDEC_XFORM_BASE + 0x200))
#define VDEC_XFORM_QPCFG    (*(REG32_PTR_T)(VDEC_XFORM_BASE + 0x804))

/* Status / IRQ (0x39650000) */
#define VDEC_STAT_IRQ       (*(REG32_PTR_T)(VDEC_STATUS_BASE + 0x0C))
#define VDEC_STAT_DEBLOCK   (*(REG32_PTR_T)(VDEC_STATUS_BASE + 0x10))
#define VDEC_STAT_IRQEN     (*(REG32_PTR_T)(VDEC_STATUS_BASE + 0x14))

/* Output DMA (0x39660000) */
#define VDEC_OUTDMA_STATUS  (*(REG32_PTR_T)(VDEC_OUTDMA_BASE + 0x00))

/* --- Video Post-Processing Pipeline (3 blocks) --- */

#define VPP_SCALER_BASE     0x39100000  /* Polyphase scaling filter */
#define VPP_LAYER_BASE      0x39200000  /* Video layer DMA / pixel format */
#define VPP_POSTPROC_BASE   0x39300000  /* CSC, overlay, gamma */

/* Scaler (0x39100000) */
#define VPP_SC_CTRL         (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x000))
#define VPP_SC_CFG1         (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x004))
#define VPP_SC_CFG2         (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x008))
#define VPP_SC_CFG3         (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x00C))
#define VPP_SC_HSCALE       (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x028))
#define VPP_SC_VSCALE       (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x02C))
#define VPP_SC_HSCALE2      (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x030))
#define VPP_SC_VSCALE2      (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x034))
#define VPP_SC_VSCALE3      (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x038))
#define VPP_SC_LUMATAPS     (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x03C))
#define VPP_SC_LUMABITS     (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x040))
#define VPP_SC_ENABLE       (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x200))
#define VPP_SC_COMMIT       (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x20C))
#define VPP_SC_GO           (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x3C0))
#define VPP_SC_GOCOMMIT     (*(REG32_PTR_T)(VPP_SCALER_BASE + 0x3CC))

/* Layer Controller (0x39200000) */
#define VPP_LC_MODE         (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x000))
#define VPP_LC_ENABLE       (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x004))
#define VPP_LC_PIXFMT       (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x008))
#define VPP_LC_PIXDETAIL    (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x00C))
#define VPP_LC_DMACFG       (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x048))
#define VPP_LC_DMAADDR0     (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x080))
#define VPP_LC_DMAADDR1     (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x084))
#define VPP_LC_DMAADDR2     (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x088))
#define VPP_LC_COMMIT       (*(REG32_PTR_T)(VPP_LAYER_BASE + 0x800))

/* Post-Processor (0x39300000) */
#define VPP_PP_CTRL         (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x000))
#define VPP_PP_MODECFG      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x008))
#define VPP_PP_OUTFMT       (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x00C))
#define VPP_PP_ENABLE       (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x010))
/* Color matrix: 6 x 32-bit coefficients at +0x01C..+0x030 */
#define VPP_PP_CMAT(n)      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x01C + (n)*4))
#define VPP_PP_HTIMING      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x034))
/* CSC registers */
#define VPP_PP_CSCMODE      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x180))
#define VPP_PP_CSC_Y        (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x184))
#define VPP_PP_CSC_CBCR     (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x188))
#define VPP_PP_CSC_OFS      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x18C))
#define VPP_PP_CSC_RSV      (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x190))
#define VPP_PP_OUTPIXFMT    (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x1C0))
#define VPP_PP_CLEAR        (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x3C0))
#define VPP_PP_COMMIT       (*(REG32_PTR_T)(VPP_POSTPROC_BASE + 0x3D0))

/* --- Clock / IRQ / Power Constants --- */

/* PWRCON(1) bit 25: clock gate for the video decoder region.
 * RE found the firmware calling FUN_0036d3f0(0x39, 1, 1) which
 * turns out to write GPIOCMD, not PWRCON. The actual PWRCON gate
 * for these blocks is already enabled at boot (bit 25 = 0).
 * Kept for reference. */
#define CLOCKGATE_VDEC      57
#define CLOCKGATE_VDEC_BIT  (1 << (CLOCKGATE_VDEC & 0x1f))
#define CLOCKGATE_VDEC_REG  1  /* PWRCON(1) */

/* GPIO power enable: the firmware sets GPIO port 7 pin 1 HIGH
 * to physically power the video decoder (via GPIOCMD register).
 * GPIO pin 57 = port (57>>3)=7, pin (57&7)=1.
 * GPIOCMD value: (port << 16) | (pin << 8) | direction_value
 *   0xf = output HIGH, 0xe = output LOW */
#define VDEC_GPIO_PORT      7
#define VDEC_GPIO_PIN       1
#define GPIOCMD_OUTPUT_HI   0x0f
#define GPIOCMD_OUTPUT_LO   0x0e
#define VDEC_GPIO_CMD_ON    ((VDEC_GPIO_PORT << 16) | \
                             (VDEC_GPIO_PIN << 8) | GPIOCMD_OUTPUT_HI)
#define VDEC_GPIO_CMD_OFF   ((VDEC_GPIO_PORT << 16) | \
                             (VDEC_GPIO_PIN << 8) | GPIOCMD_OUTPUT_LO)

/* PWRCON(0) bit 6: suspected additional clock gate for video
 * decoder blocks. In the log, bit 6 is SET (disabled) while
 * the VPP writes don't persist. The IRAM function
 * thunk_EXT_FUN_220043e8(1, 0x40) likely clears this bit. */
#define VDEC_PWRCON0_BIT    (1 << 6)  /* 0x40 */

/* IRQ 14 is the video decoder interrupt.
 * Note: Rockbox labels this IRQ_LCD.
 * We do NOT enable this IRQ from the plugin (would trigger UIRQ panic).
 * Poll-only mode. */
#define IRQ_VDEC            14

/* VPP control register bit definitions (common to all 3 blocks) */
#define VPP_CTRL_ENABLE     (1 << 0)  /* write 1 to start */
#define VPP_CTRL_BUSY       (1 << 1)  /* read-only: 1 = busy */

/* Maximum idle-wait iterations before giving up */
#define VPP_IDLE_TIMEOUT    100

/* Log file path */
#define LOG_PATH            "/vdec_poc.log"

/*
 * ========================================================================
 *  Global State
 * ========================================================================
 */

static int log_fd = -1;
static bool decoder_powered = false;
static bool pipeline_active = false;

/*
 * ========================================================================
 *  Logging
 * ========================================================================
 */

static void log_open(void)
{
    log_fd = rb->open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (log_fd >= 0)
    {
        rb->fdprintf(log_fd, "\n=== H264 PoC session start [tick %ld] ===\n",
                     *rb->current_tick);
    }
    else
    {
        rb->splashf(HZ * 2, "Warning: cannot open %s", LOG_PATH);
    }
}

static void log_close(void)
{
    if (log_fd >= 0)
    {
        rb->fdprintf(log_fd, "=== session end [tick %ld] ===\n",
                     *rb->current_tick);
        rb->close(log_fd);
        log_fd = -1;
    }
}

/* Formatted log write. Also splashes briefly on screen. */
static void logf_msg(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (log_fd >= 0)
        rb->fdprintf(log_fd, "[%08lx] %s\n", *rb->current_tick, buf);
}

/* Log a single register read */
static uint32_t log_reg_read(const char *name, volatile uint32_t *reg)
{
    uint32_t val = *reg;
    if (log_fd >= 0)
        rb->fdprintf(log_fd, "[%08lx]   %-24s @ 0x%08lx = 0x%08lx\n",
                     *rb->current_tick, name,
                     (unsigned long)(uintptr_t)reg, (unsigned long)val);
    return val;
}

/*
 * ========================================================================
 *  Clock Gate Control
 *
 *  clockgate_enable() is not in the plugin API, so we inline the logic.
 *  From clocking-s5l8702.c:232:
 *    int i = gate >> 5;
 *    uint32_t bit = 1 << (gate & 0x1f);
 *    if (enable) PWRCON(i) &= ~bit;
 *    else PWRCON(i) |= bit;
 * ========================================================================
 */

static void vdec_clockgate(bool enable)
{
    uint32_t before = PWRCON(CLOCKGATE_VDEC_REG);

    if (enable)
        PWRCON(CLOCKGATE_VDEC_REG) &= ~CLOCKGATE_VDEC_BIT;
    else
        PWRCON(CLOCKGATE_VDEC_REG) |= CLOCKGATE_VDEC_BIT;

    uint32_t after = PWRCON(CLOCKGATE_VDEC_REG);

    logf_msg("CLOCKGATE %s: PWRCON(%d) 0x%08lx -> 0x%08lx (bit %d = %d)",
             enable ? "ON" : "OFF",
             CLOCKGATE_VDEC_REG,
             (unsigned long)before, (unsigned long)after,
             CLOCKGATE_VDEC, enable ? 1 : 0);

    decoder_powered = enable;
}

/*
 * Full power-on sequence discovered from firmware RE:
 *
 * 1. thunk_EXT_FUN_220043e8(1, 0x40) — likely clears PWRCON(0) bit 6
 * 2. thunk_EXT_FUN_22002980()        — sync/barrier (unknown)
 * 3. GPIO pin 57 (port 7 pin 1) → output HIGH via GPIOCMD
 *
 * This function attempts all discoverable power steps.
 */
static void vdec_full_power(bool enable)
{
    logf_msg("=== Full Power %s Sequence ===", enable ? "ON" : "OFF");

    /* Step 1: PWRCON(1) bit 25 — clock gate */
    vdec_clockgate(enable);

    /* Step 2: PWRCON(0) bit 6 — suspected additional gate
     * The IRAM function thunk_EXT_FUN_220043e8(1, 0x40) likely does this.
     * 0x40 = bit 6 of PWRCON(0). */
    {
        uint32_t before = PWRCON(0);
        if (enable)
            PWRCON(0) &= ~VDEC_PWRCON0_BIT;
        else
            PWRCON(0) |= VDEC_PWRCON0_BIT;
        uint32_t after = PWRCON(0);
        logf_msg("PWRCON(0) bit 6: 0x%08lx -> 0x%08lx",
                 (unsigned long)before, (unsigned long)after);
    }

    /* Step 3: GPIO power enable — port 7 pin 1
     * FUN_0036d3f0(0x39, 1, 1) writes GPIOCMD to set this HIGH. */
    if (enable)
        GPIOCMD = VDEC_GPIO_CMD_ON;
    else
        GPIOCMD = VDEC_GPIO_CMD_OFF;
    logf_msg("GPIOCMD = 0x%08lx (GPIO 7.1 %s)",
             (unsigned long)(enable ? VDEC_GPIO_CMD_ON : VDEC_GPIO_CMD_OFF),
             enable ? "HIGH" : "LOW");

    /* Brief delay for clocks/power to stabilize */
    rb->sleep(HZ / 10);

    /* Log all PWRCON registers for analysis */
    if (log_fd >= 0)
    {
        int i;
        for (i = 0; i <= 4; i++)
            rb->fdprintf(log_fd, "[%08lx]   PWRCON(%d) = 0x%08lx\n",
                         *rb->current_tick, i, (unsigned long)PWRCON(i));
    }

    /* Verify: try writing a test value to scaler and read it back */
    if (enable)
    {
        uint32_t test_before = VPP_SC_HSCALE;
        VPP_SC_HSCALE = 0x8000000;
        uint32_t test_after = VPP_SC_HSCALE;
        logf_msg("Write test: SC_HSCALE 0x%08lx -> write 0x08000000 -> read 0x%08lx %s",
                 (unsigned long)test_before, (unsigned long)test_after,
                 (test_after == 0x8000000) ? "OK!" : "FAILED");
    }

    logf_msg("=== Full Power %s Done ===", enable ? "ON" : "OFF");
}

/*
 * ========================================================================
 *  Register Status Reading
 * ========================================================================
 */

struct vdec_status
{
    /* H.264 decoder blocks */
    uint32_t main_status;
    uint32_t core_cmd;
    uint32_t stat_irq;
    uint32_t stat_irqen;
    uint32_t outdma_status;
    uint32_t bsdma_cfg;
    uint32_t xform_cfg0;

    /* VPP pipeline blocks */
    uint32_t sc_ctrl;
    uint32_t lc_mode;
    uint32_t pp_ctrl;
};

static void vdec_read_status(struct vdec_status *st)
{
    logf_msg("--- Register Status Dump ---");

    logf_msg("H.264 Decoder:");
    st->main_status  = log_reg_read("MAIN_STATUS",  &VDEC_MAIN_STATUS);
    st->core_cmd     = log_reg_read("CORE_CMD",     &VDEC_CORE_CMD);
    st->stat_irq     = log_reg_read("STAT_IRQ",     &VDEC_STAT_IRQ);
    st->stat_irqen   = log_reg_read("STAT_IRQEN",   &VDEC_STAT_IRQEN);
    st->outdma_status= log_reg_read("OUTDMA_STATUS", &VDEC_OUTDMA_STATUS);
    st->bsdma_cfg    = log_reg_read("BSDMA_CFG",    &VDEC_BSDMA_CFG);
    st->xform_cfg0   = log_reg_read("XFORM_CFG0",   &VDEC_XFORM_CFG0);

    logf_msg("Video Post-Processing:");
    st->sc_ctrl      = log_reg_read("SC_CTRL",      &VPP_SC_CTRL);
    st->lc_mode      = log_reg_read("LC_MODE",      &VPP_LC_MODE);
    st->pp_ctrl      = log_reg_read("PP_CTRL",      &VPP_PP_CTRL);

    logf_msg("--- End Status Dump ---");
}

static void vdec_display_status(struct vdec_status *st)
{
    rb->lcd_clear_display();
    rb->lcd_putsf(0, 0, "=== H264 HW Status ===");
    rb->lcd_putsf(0, 1, "MAIN  :0x%08lx", (unsigned long)st->main_status);
    rb->lcd_putsf(0, 2, "CORE  :0x%08lx", (unsigned long)st->core_cmd);
    rb->lcd_putsf(0, 3, "IRQ   :0x%08lx", (unsigned long)st->stat_irq);
    rb->lcd_putsf(0, 4, "IRQEN :0x%08lx", (unsigned long)st->stat_irqen);
    rb->lcd_putsf(0, 5, "ODMA  :0x%08lx", (unsigned long)st->outdma_status);
    rb->lcd_putsf(0, 6, "BSDMA :0x%08lx", (unsigned long)st->bsdma_cfg);
    rb->lcd_putsf(0, 7, "XFORM :0x%08lx", (unsigned long)st->xform_cfg0);
    rb->lcd_putsf(0, 8, "--- VPP Pipeline ---");
    rb->lcd_putsf(0, 9, "SCALE :0x%08lx", (unsigned long)st->sc_ctrl);
    rb->lcd_putsf(0, 10, "LAYER :0x%08lx", (unsigned long)st->lc_mode);
    rb->lcd_putsf(0, 11, "POSTP :0x%08lx", (unsigned long)st->pp_ctrl);
    rb->lcd_putsf(0, 12, "PWR: %s  VPP: %s",
                  decoder_powered ? "ON" : "OFF",
                  pipeline_active ? "ON" : "OFF");
    rb->lcd_update();
    rb->button_get_w_tmo(HZ * 3);
}

/*
 * ========================================================================
 *  Extended Register Dump
 *
 *  Reads a broader sweep of registers from each block and logs them.
 * ========================================================================
 */

static void vdec_dump_regs(void)
{
    logf_msg("=== Extended Register Dump ===");

    logf_msg("Decoder Core (0x39610000):");
    for (unsigned i = 0; i <= 0xA8; i += 4)
    {
        volatile uint32_t *reg = (REG32_PTR_T)(VDEC_CORE_BASE + i);
        char name[32];
        rb->snprintf(name, sizeof(name), "CORE+0x%03x", i);
        log_reg_read(name, reg);
    }

    logf_msg("Status/IRQ (0x39650000):");
    for (unsigned i = 0; i <= 0x1C; i += 4)
    {
        volatile uint32_t *reg = (REG32_PTR_T)(VDEC_STATUS_BASE + i);
        char name[32];
        rb->snprintf(name, sizeof(name), "STAT+0x%03x", i);
        log_reg_read(name, reg);
    }

    logf_msg("Output DMA (0x39660000):");
    for (unsigned i = 0; i <= 0x3C; i += 4)
    {
        volatile uint32_t *reg = (REG32_PTR_T)(VDEC_OUTDMA_BASE + i);
        char name[32];
        rb->snprintf(name, sizeof(name), "ODMA+0x%03x", i);
        log_reg_read(name, reg);
    }

    logf_msg("Transform (0x39641000):");
    log_reg_read("XFORM+0x200", &VDEC_XFORM_CFG0);
    log_reg_read("XFORM+0x804", &VDEC_XFORM_QPCFG);

    logf_msg("Scaler (0x39100000):");
    for (unsigned i = 0; i <= 0x44; i += 4)
    {
        volatile uint32_t *reg = (REG32_PTR_T)(VPP_SCALER_BASE + i);
        char name[32];
        rb->snprintf(name, sizeof(name), "SC+0x%03x", i);
        log_reg_read(name, reg);
    }
    log_reg_read("SC+0x200 (enable)", &VPP_SC_ENABLE);
    log_reg_read("SC+0x3C0 (go)",     &VPP_SC_GO);

    logf_msg("Layer Ctrl (0x39200000):");
    for (unsigned i = 0; i <= 0x10; i += 4)
    {
        volatile uint32_t *reg = (REG32_PTR_T)(VPP_LAYER_BASE + i);
        char name[32];
        rb->snprintf(name, sizeof(name), "LC+0x%03x", i);
        log_reg_read(name, reg);
    }
    log_reg_read("LC+0x048 (dmacfg)", &VPP_LC_DMACFG);
    log_reg_read("LC+0x080 (dma0)",   &VPP_LC_DMAADDR0);
    log_reg_read("LC+0x084 (dma1)",   &VPP_LC_DMAADDR1);

    logf_msg("Post-Proc (0x39300000):");
    log_reg_read("PP+0x000 (ctrl)",    &VPP_PP_CTRL);
    log_reg_read("PP+0x008 (mode)",    &VPP_PP_MODECFG);
    log_reg_read("PP+0x00C (outfmt)",  &VPP_PP_OUTFMT);
    log_reg_read("PP+0x010 (enable)",  &VPP_PP_ENABLE);
    log_reg_read("PP+0x180 (csc)",     &VPP_PP_CSCMODE);
    log_reg_read("PP+0x184 (csc_y)",   &VPP_PP_CSC_Y);
    log_reg_read("PP+0x188 (csc_cc)",  &VPP_PP_CSC_CBCR);
    log_reg_read("PP+0x18C (csc_ofs)", &VPP_PP_CSC_OFS);
    log_reg_read("PP+0x1C0 (pxfmt)",   &VPP_PP_OUTPIXFMT);

    logf_msg("PWRCON state:");
    if (log_fd >= 0)
    {
        int i;
        for (i = 0; i <= 4; i++)
            rb->fdprintf(log_fd, "[%08lx]   PWRCON(%d) = 0x%08lx\n",
                         *rb->current_tick, i, (unsigned long)PWRCON(i));
    }

    logf_msg("VIC status for IRQ %d (video decoder):", IRQ_VDEC);
    if (log_fd >= 0)
    {
        rb->fdprintf(log_fd, "[%08lx]   VIC0RAWINTR    = 0x%08lx (bit %d = %ld)\n",
                     *rb->current_tick, (unsigned long)VIC0RAWINTR,
                     IRQ_VDEC, (unsigned long)((VIC0RAWINTR >> IRQ_VDEC) & 1));
        rb->fdprintf(log_fd, "[%08lx]   VIC0IRQSTATUS  = 0x%08lx (bit %d = %ld)\n",
                     *rb->current_tick, (unsigned long)VIC0IRQSTATUS,
                     IRQ_VDEC, (unsigned long)((VIC0IRQSTATUS >> IRQ_VDEC) & 1));
        rb->fdprintf(log_fd, "[%08lx]   VIC0INTENABLE  = 0x%08lx (bit %d = %ld)\n",
                     *rb->current_tick, (unsigned long)VICINTENABLE(0),
                     IRQ_VDEC, (unsigned long)((VICINTENABLE(0) >> IRQ_VDEC) & 1));
    }

    logf_msg("=== End Extended Dump ===");

    rb->splashf(HZ * 2, "Dumped regs to %s", LOG_PATH);
}

/*
 * ========================================================================
 *  IRQ Clear
 *
 *  Reproduces the interrupt acknowledgment from FUN_0004c758:
 *    - Clears decoder core IRQ
 *    - Clears DMA IRQ
 *    - Clears output DMA
 *    - Clears main status
 * ========================================================================
 */

static void vdec_irq_clear(void)
{
    logf_msg("Clearing all video decoder IRQs");

    VDEC_CORE_IRQCLR   = 0xFFFFFFFF;  /* 0x39610000 + 0x10000 */
    VDEC_BSDMA_IRQCLR  = 0xFFFFFFFF;  /* 0x39630000 + 0x100 */
    VDEC_OUTDMA_STATUS  = 0xFFFFFFFF;  /* 0x39660000 + 0x00 */
    VDEC_MAIN_STATUS    = 0xFFFFFFFF;  /* 0x39600000 + 0x00 */

    /* Read back to verify */
    uint32_t main_after = VDEC_MAIN_STATUS;
    uint32_t outdma_after = VDEC_OUTDMA_STATUS;

    logf_msg("After clear: MAIN=0x%08lx OUTDMA=0x%08lx",
             (unsigned long)main_after, (unsigned long)outdma_after);

    rb->splashf(HZ, "IRQs cleared: M=%08lx O=%08lx",
                (unsigned long)main_after, (unsigned long)outdma_after);
}

/*
 * ========================================================================
 *  Video Post-Processing Pipeline Init
 *
 *  Reproduces the startup sequence from FUN_00168450:
 *    1. Init scaler @ 0x39100000
 *    2. Init layer controller @ 0x39200000
 *    3. Init post-processor @ 0x39300000
 *
 *  Each block follows the same control register pattern:
 *    Bit 0 = enable, Bit 1 = busy (read-only)
 * ========================================================================
 */

/* Wait for a VPP block to go idle (bit 1 = 0) */
static bool vpp_wait_idle(volatile uint32_t *ctrl, const char *name)
{
    int timeout = VPP_IDLE_TIMEOUT;
    while ((*ctrl & VPP_CTRL_BUSY) && --timeout > 0)
        rb->sleep(1);  /* ~10ms per tick */

    if (timeout <= 0)
    {
        logf_msg("TIMEOUT waiting for %s idle (ctrl=0x%08lx)",
                 name, (unsigned long)*ctrl);
        return false;
    }
    logf_msg("%s now idle (ctrl=0x%08lx)", name, (unsigned long)*ctrl);
    return true;
}

static void vpp_init_scaler(void)
{
    logf_msg("Init Scaler @ 0x%08x", VPP_SCALER_BASE);

    /* Clear enable, preserve status bits */
    VPP_SC_CTRL &= 0x02;

    /* Clear configuration registers */
    VPP_SC_CFG1 = 0;
    VPP_SC_CFG2 = 0;
    VPP_SC_CFG3 = 0;

    /* Set 1:1 scale factors (0x8000000 = 1.0 in 4.28 fixed-point) */
    VPP_SC_HSCALE  = 0x8000000;
    VPP_SC_VSCALE  = 0x8000000;
    VPP_SC_HSCALE2 = 0x8000000;
    VPP_SC_VSCALE2 = 0x8000000;
    VPP_SC_VSCALE3 = 0x8000000;

    /* Filter configuration */
    VPP_SC_LUMATAPS = 0x40;  /* luma filter tap count */
    VPP_SC_LUMABITS = 0x10;  /* luma precision/phase bits */

    /* Enable filter block */
    VPP_SC_ENABLE = 1;
    VPP_SC_COMMIT = 1;

    logf_msg("Scaler init complete");
}

static void vpp_init_layer(void)
{
    logf_msg("Init Layer Controller @ 0x%08x", VPP_LAYER_BASE);

    /* Layer mode */
    VPP_LC_MODE = 6;
    VPP_LC_ENABLE = 0;
    VPP_LC_PIXFMT = 0;
    VPP_LC_PIXDETAIL = 0;

    /* Commit */
    VPP_LC_COMMIT = 1;

    logf_msg("Layer controller init complete");
}

static void vpp_init_postproc(void)
{
    logf_msg("Init Post-Processor @ 0x%08x", VPP_POSTPROC_BASE);

    /* Clear enable */
    VPP_PP_CTRL &= 0x02;

    /* Output format: progressive */
    VPP_PP_OUTFMT = 6;

    /* Pipeline enable */
    VPP_PP_ENABLE = 1;

    /* Identity color matrix (0x800 = 1.0 in 1.11 fixed-point) */
    for (int i = 0; i < 6; i++)
        VPP_PP_CMAT(i) = 0x800;

    /* BT.601 color space conversion */
    VPP_PP_CSCMODE = (VPP_PP_CSCMODE & 0xE0) | 0x10;
    VPP_PP_CSC_Y    = 0x800000;  /* Y coeff = 1.0 (8.24 FP) */
    VPP_PP_CSC_CBCR = 0x800000;  /* CbCr coeff = 1.0 */
    VPP_PP_CSC_OFS  = 0x80;      /* 128 offset for unsigned chroma */
    VPP_PP_CSC_RSV  = 0;

    /* Output pixel format */
    VPP_PP_OUTPIXFMT = 0x11;

    /* Clear and commit */
    VPP_PP_CLEAR  = 0;
    VPP_PP_COMMIT = 1;

    logf_msg("Post-processor init complete");
}

static void vpp_init_pipeline(void)
{
    if (!decoder_powered)
    {
        rb->splash(HZ, "Power on decoder first!");
        return;
    }

    logf_msg("=== VPP Pipeline Init ===");

    /* Order matters: scaler -> layer -> postproc (from firmware RE) */
    vpp_init_scaler();
    vpp_init_layer();
    vpp_init_postproc();

    pipeline_active = true;

    logf_msg("=== VPP Pipeline Init Done ===");
    rb->splash(HZ, "VPP pipeline initialized");
}

/*
 * ========================================================================
 *  Video Post-Processing Pipeline Shutdown
 *
 *  Reproduces the shutdown from FUN_00166d9c (param2=0):
 *    1. Disable post-processor, wait idle
 *    2. Disable layer controller, wait idle
 *    3. Disable scaler, wait idle
 * ========================================================================
 */

static void vpp_shutdown_pipeline(void)
{
    logf_msg("=== VPP Pipeline Shutdown ===");

    /* Reverse order: postproc -> layer -> scaler */

    logf_msg("Disabling post-processor...");
    VPP_PP_CTRL &= ~VPP_CTRL_ENABLE;
    vpp_wait_idle(&VPP_PP_CTRL, "PostProc");

    logf_msg("Disabling layer controller...");
    VPP_LC_MODE &= ~VPP_CTRL_ENABLE;
    vpp_wait_idle(&VPP_LC_MODE, "Layer");

    logf_msg("Disabling scaler...");
    VPP_SC_CTRL &= ~VPP_CTRL_ENABLE;
    vpp_wait_idle(&VPP_SC_CTRL, "Scaler");

    pipeline_active = false;

    logf_msg("=== VPP Pipeline Shutdown Done ===");
    rb->splash(HZ, "VPP pipeline shut down");
}

/*
 * ========================================================================
 *  H.264 Slice Submission Skeleton
 *
 *  This reproduces the exact register write sequence from FUN_000692e0
 *  (h264_slice_submit) in the iPod Classic firmware. It is DISABLED
 *  by default — this is documentation in code form.
 *
 *  Calling this without a valid bitstream and properly configured
 *  reference frames would hang the decoder or corrupt memory.
 * ========================================================================
 */

#if 0  /* DISABLED - skeleton only, do not enable without valid NAL data */
static void h264_submit_slice(
    void     *bitstream_phys,   /* physical address of NAL unit data */
    uint32_t  bitstream_size,   /* size in bytes */
    uint16_t  pic_width,        /* picture width in pixels */
    uint16_t  pic_height,       /* picture height in pixels */
    uint8_t   num_ref_frames,   /* from SPS: max number of reference frames */
    uint8_t   chroma_format,    /* chroma_format_idc (usually 1 = 4:2:0) */
    uint32_t  slice_cfg,        /* packed: weighted_pred | transform_8x8 |
                                   direct_spatial | entropy_mode | slice_type */
    uint8_t   num_ref_idx,      /* num_ref_idx_active */
    uint8_t   deblock_mode,     /* deblocking filter mode */
    int8_t    chroma_qp_offset  /* chroma QP offset from PPS */
)
{
    uint32_t mb_width  = pic_width  >> 4;  /* macroblocks = pixels / 16 */
    uint32_t mb_height = pic_height >> 4;
    uint32_t total_mbs = mb_width * mb_height;

    /* Compute log2(total_mbs) for MB config register */
    uint32_t log2_mbs = 0;
    uint32_t tmp = total_mbs;
    while (tmp > 1) { tmp >>= 1; log2_mbs++; }

    /* 1. Transform unit configuration */
    VDEC_XFORM_QPCFG = ((uint32_t)chroma_qp_offset << 8) | 0x01;

    /* 2. Deblocking filter configuration */
    VDEC_STAT_DEBLOCK = deblock_mode;

    /* 3. Decoder core: slice parameters */
    VDEC_CORE_SLICECFG = slice_cfg | 0x0C02;  /* OR'd with constant */
    VDEC_CORE_SLICEHDR = num_ref_idx;

    /* 4. Macroblock configuration:
     *    bits[7:0]   = chroma_format
     *    bits[11:4]  = log2(total_mbs) << 4
     *    bits[23:8]  = num_ref_frames << 8 */
    VDEC_CORE_MBCFG = (num_ref_frames << 8) | (log2_mbs << 4) | chroma_format;
    VDEC_CORE_NUMREF = num_ref_frames;
    VDEC_CORE_RSV0 = 0;
    VDEC_CORE_RSV1 = 0;

    /* 5. Start decode command */
    VDEC_CORE_CMD = 3;

    /* 6. Bitstream DMA setup */
    VDEC_BSDMA_CLEAR   = 0xFFFFFFFF;
    VDEC_BSDMA_SRCADDR = (uint32_t)(uintptr_t)bitstream_phys;
    VDEC_BSDMA_ENDADDR = (uint32_t)(uintptr_t)bitstream_phys + bitstream_size;

    /* 7. Start DMA */
    VDEC_BSDMA_START = 2;

    /* After this, firmware calls h264_wait_complete(1, 3)
     * which polls the status register or waits on a semaphore
     * for the FrameDone interrupt (bit 0x40 in OUTDMA_STATUS) */
}
#endif  /* disabled skeleton */

/*
 * ========================================================================
 *  Menu and Plugin Entry Point
 * ========================================================================
 */

enum menu_action {
    MENU_FULL_POWER_ON = 0,
    MENU_READ_STATUS,
    MENU_INIT_VPP,
    MENU_DUMP_REGS,
    MENU_SHUTDOWN_VPP,
    MENU_CLEAR_IRQS,
    MENU_FULL_POWER_OFF,
    MENU_QUIT,
};

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    int selection = 0;
    bool quit = false;
    struct vdec_status st;

    MENUITEM_STRINGLIST(menu, "H264 Video Decoder PoC", NULL,
                        "FULL Power ON (gate+GPIO)",
                        "Read HW status",
                        "Init VPP pipeline",
                        "Dump all regs -> log",
                        "Shutdown VPP",
                        "Clear IRQs",
                        "FULL Power OFF",
                        "Quit");

    log_open();
    logf_msg("Plugin started");
    if (log_fd >= 0)
    {
        int i;
        for (i = 0; i <= 4; i++)
            rb->fdprintf(log_fd, "[%08lx]   PWRCON(%d) = 0x%08lx\n",
                         *rb->current_tick, i, (unsigned long)PWRCON(i));
    }

    while (!quit)
    {
        switch (rb->do_menu(&menu, &selection, NULL, false))
        {
            case MENU_FULL_POWER_ON:
                if (decoder_powered)
                {
                    rb->splash(HZ, "Already powered on");
                    break;
                }
                vdec_full_power(true);
                rb->splash(HZ, "Full power sequence done");
                break;

            case MENU_READ_STATUS:
                if (!decoder_powered)
                {
                    rb->splash(HZ, "Power on decoder first!");
                    break;
                }
                rb->memset(&st, 0, sizeof(st));
                vdec_read_status(&st);
                vdec_display_status(&st);
                break;

            case MENU_INIT_VPP:
                vpp_init_pipeline();
                break;

            case MENU_SHUTDOWN_VPP:
                if (!pipeline_active)
                {
                    rb->splash(HZ, "Pipeline not active");
                    break;
                }
                vpp_shutdown_pipeline();
                break;

            case MENU_CLEAR_IRQS:
                if (!decoder_powered)
                {
                    rb->splash(HZ, "Power on decoder first!");
                    break;
                }
                vdec_irq_clear();
                break;

            case MENU_DUMP_REGS:
                if (!decoder_powered)
                {
                    rb->splash(HZ, "Power on decoder first!");
                    break;
                }
                vdec_dump_regs();
                break;

            case MENU_FULL_POWER_OFF:
                if (pipeline_active)
                    vpp_shutdown_pipeline();
                if (decoder_powered)
                    vdec_full_power(false);
                rb->splash(HZ, "Full power off done");
                break;

            case MENU_QUIT:
            default:
                quit = true;
                break;
        }
    }

    /* Clean shutdown */
    if (pipeline_active)
    {
        logf_msg("Cleanup: shutting down VPP pipeline");
        vpp_shutdown_pipeline();
    }
    if (decoder_powered)
    {
        logf_msg("Cleanup: full power off");
        vdec_full_power(false);
    }

    logf_msg("Plugin exit OK");
    log_close();

    return PLUGIN_OK;
}
