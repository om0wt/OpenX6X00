/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers Plus: the radio's state on a KX3-style segment display (lcd_panel.c)
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#pragma once

#include "lvgl/lvgl.h"

/* Over the main screen from the top, height px tall */
lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height);

/* Plus: where the spectrum strip is */
#define LCD_PLUS_LCD_H      286     /* the KX3 display over it */
#define LCD_PLUS_STRIP_Y    294
#define LCD_PLUS_STRIP_H    112
#define LCD_PLUS_STRIP_W    800

/* Plus: the spectrum strip, in CW while keying or while decoded text comes in the left half,
 * the text in the right half */
void lcd_panel_set_strip(lv_obj_t *spectrum);

/* Any thread: CW decoder text, shown in the strip's lower half (Plus) */
void lcd_panel_cw_text(const char *text);
