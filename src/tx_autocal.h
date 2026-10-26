/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  TX calibration by itself: the band output gain of every HF band and 6 m set so the radio's own
 *  meter reads the PWR setting (tx_autocal.cpp). It transmits: into a dummy load only.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL thread: start (false when it runs already or cannot: TX, no band gain on this base), stop */
bool tx_autocal_start(void);
void tx_autocal_stop(const char *why);
bool tx_autocal_running(void);

/* Any thread: what it does or what it did, one line per band */
void tx_autocal_status(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
