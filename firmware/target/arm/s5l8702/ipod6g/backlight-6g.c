/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: backlight-nano2g.c 28601 2010-11-14 20:39:18Z theseven $
 *
 * Copyright (C) 2009 by Dave Chapman
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
#include <stdbool.h>

#include "config.h"
#include "kernel.h"
#include "backlight.h"
#include "backlight-target.h"
#include "pmu-target.h"
#include "storage.h"

#ifdef HAVE_LCD_SLEEP
bool lcd_active(void);
void lcd_awake(void);
void lcd_update(void);
#endif

/* Last values written to the PMU backlight registers.  Unknown after init
 * (forced write) and after kill; off only means the 0x29 latch was cleared
 * through this driver. */
static bool backlight_pmu_on;
static int backlight_pmu_brightness = -1;

void backlight_hw_brightness(int brightness)
{
    if (brightness == backlight_pmu_brightness)
        return;

    pmu_write(0x28, brightness);
    backlight_pmu_brightness = brightness;
}

void backlight_hw_on(void)
{
    bool was_off = !backlight_pmu_on;
#ifdef HAVE_LCD_SLEEP
    bool lcd_was_asleep = !lcd_active();
#endif

    /* Only a real off->on transition should wake storage, and only if it may
     * actually be gated.  backlight_on() is called for every key event, so
     * this keeps the request out of the common already-on path. */
    if (was_off && storage_get_ssd_mode() && !storage_disk_is_active())
        storage_post_event(Q_STORAGE_PRE_WAKE, 0);

#ifdef HAVE_LCD_SLEEP
    if (lcd_was_asleep)
    {
        int brightness = backlight_pmu_brightness;

        lcd_awake();
        /* Force the backlight latch write after LCD resume and re-assert
         * the last brightness (the cache is not trusted across LCD sleep). */
        was_off = true;
        if (brightness >= 0)
            pmu_write(0x28, brightness);
    }
#endif

    if (was_off)
    {
        pmu_write(0x29, 1);
        backlight_pmu_on = true;
    }
}

void backlight_hw_off(void)
{
    if (!backlight_pmu_on)
        return;

    pmu_write(0x29, 0);
    backlight_pmu_on = false;
}

bool backlight_hw_init(void)
{
    /* PMU state is unknown here: force all init writes. */
    backlight_pmu_on = false;
    backlight_pmu_brightness = -1;

    /* LEDCTL: overvoltage protection enabled, OCP limit is 500mA */
    pmu_write(0x2a, 0x05);

    pmu_write(0x2b, 0x14);  /* T_dimstep = 16 * value / 32768 */
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);

    backlight_hw_on();

    return true;
}

/* Kill the backlight, instantly. */
void backlight_hw_kill(void)
{
    /* Shutdown path: write unconditionally and leave the cache invalid. */
    backlight_pmu_brightness = -1;
    pmu_write(0x2b, 0);  /* T_dimstep = 0 */
    pmu_write(0x29, 0);
    backlight_pmu_on = false;
}
