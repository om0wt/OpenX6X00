/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers: the radio's state on a KX3-style segment display (lcd_panel.c)
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#pragma once

#include "lvgl/lvgl.h"

/* Over the top of the main screen, height px tall: the display in the middle of it */
lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height);
