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

/* The config socket's commands (cfg_socket.cpp), on its thread; false when cmd is not one of them:
 *    {"cmd":"txcal_start"} / {"cmd":"txcal_stop"}  -> {"t":"ok"}, run on the LVGL thread
 *    {"cmd":"txcal_get"}                          -> {"t":"txcal","running":bool,"status":"lines"}
 * The answer goes to out (*l its length), or an error to *err */
bool tx_autocal_socket(const char *cmd, char *out, size_t size, int *l, const char **err);

#ifdef __cplusplus
}
#endif
