/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  TX calibration by itself (Settings > TX Cal, the config socket's txcal_*): band by band, 160 m to
 *  6 m, an FM carrier for 2.5 s; the last second of the radio's own power and SWR readings is
 *  averaged and the band output gain moved by what is missing to the PWR setting, 10 log(PWR / P)
 *  dB, up to 7 carriers a band, 2 s between them; a band whose power stays put as the gain moves
 *  is held by the limiter: at its full power. The radio's meter is the reference: as good as
 *  it is. It stops at once on an SWR over 2 (no dummy load), on no power, on Stop, or when
 *  something else transmits; each band gets its mode and split back, the radio its band.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#include "tx_autocal.h"

#include "cfg/db.h"
#include "cfg/settings_manager.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
    #include "cJSON.h"
    #include "cfg/cfg_api.h"
    #include "radio.h"
    #include "radio_types.h"
    #include "scheduler.h"
    #include "tx_info.h"

    #include "lvgl/lvgl.h"
    #include <pthread.h>
}

#define TICK_MS     100
#define SWITCH_T    15      /* ticks after the band change (and the last carrier), before the next */
#define SETTLE_T    15      /* the carrier: settle, */
#define SAMPLE_T    10      /* then the readings averaged */
#define COOL_T      20      /* between two carriers */
#define TRIES       7
#define GRACE_T     10      /* after the carrier: the radio reports RX a few packets later */
#define FLAT_W      0.15f   /* the power did not move with the gain: the limiter holds it */
#define GAIN_MAX    6.0f    /* the band output gain's range (Settings) */
#define GAIN_STEP   0.2f
#define CLOSE_W     0.3f    /* near enough */
#define SWR_MAX     2.0f
#define MIN_W       0.3f    /* less than this: nothing comes out, stop */

typedef enum { S_SWITCH, S_KEY, S_COOL } state_t;

static lv_timer_t        *timer;
static state_t            state;
static int                tick, tries;
static std::vector<int>   bands;
static size_t             band_i;
static int32_t            home_band;
static int32_t            saved_mode, saved_split;
static float              pwr_sum, swr_max, last_p;
static int                samples;
static pthread_mutex_t    mux = PTHREAD_MUTEX_INITIALIZER;
static std::string        status;   /* the lines so far, the last one what it does now */

static void say(const std::string &line, bool new_line) {
    pthread_mutex_lock(&mux);
    size_t nl = status.find_last_of('\n');

    if (!new_line && !status.empty()) status.erase(nl == std::string::npos ? 0 : nl + 1);
    else if (!status.empty()) status += '\n';
    status += line;
    pthread_mutex_unlock(&mux);
    fprintf(stderr, "txcal: %s\n", line.c_str());
}

static std::string band_name(int id) {
    BandInfoLoadResult b = BandsTable::get_by_id(id);
    return b.rc == SUCCESS ? b.value.name : std::to_string(id);
}

static std::string fmt(const char *f, ...) {
    char    buf[160];
    va_list ap;

    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

/* The band being measured gets its mode and split back */
static void band_restore(void) {
    cparam_i_set(cfg_cur_mode, saved_mode);
    param_i_set(cfg_band_split, saved_split);
}

static void finish(const std::string &why) {
    lv_timer_del(timer);
    timer = NULL;
    radio_set_ptt(false);
    if (band_i < bands.size()) band_restore();
    param_i_set(cfg_band_id, home_band);
    say(why, true);
}

static void band_begin(void) {
    param_i_set(cfg_band_id, bands[band_i]);
    saved_mode  = cparam_i_get(cfg_cur_mode);
    saved_split = param_i_get(cfg_band_split);
    param_i_set(cfg_band_split, 0);
    cparam_i_set(cfg_cur_mode, radio_mode_nfm);   /* a steady carrier */
    tries  = 0;
    last_p = -1;
    state  = S_SWITCH;
    tick  = 0;
}

static void band_next(void) {
    band_restore();
    if (++band_i < bands.size()) {
        band_begin();
    } else {
        finish("Done.");
    }
}

static void timer_cb(lv_timer_t *t) {
    float target = param_f_get(cfg_pwr);
    float gain   = cfg_sm.p_band_dac_offset.get();

    tick++;
    if (param_i_get(cfg_band_id) != bands[band_i]) {
        finish("Stopped: the band was changed.");
        return;
    }
    switch (state) {
        case S_SWITCH:
            if (tick < SWITCH_T) return;
            if (radio_get_state() != RADIO_RX) {
                finish("Stopped: something else transmits.");
                return;
            }
            say(fmt("%s: carrier %d, gain %+.1f dB", band_name(bands[band_i]).c_str(), tries + 1, gain), true);
            radio_set_ptt(true);
            state   = S_KEY;
            tick    = 0;
            pwr_sum = 0;
            swr_max = 0;
            samples = 0;
            return;

        case S_KEY: {
            float pwr, swr;

            if (tick > 5 && radio_get_state() != RADIO_TX) {
                finish("Stopped: the radio did not transmit (receive only here?).");
                return;
            }
            if (tick <= SETTLE_T) return;
            tx_info_get(&pwr, &swr);
            pwr_sum += pwr;
            swr_max = LV_MAX(swr_max, swr);
            samples++;
            if (swr > SWR_MAX) {
                finish(fmt("Stopped: SWR %.1f on %s. Connect a dummy load.", swr, band_name(bands[band_i]).c_str()));
                return;
            }
            if (tick < SETTLE_T + SAMPLE_T) return;

            radio_set_ptt(false);
            float p = pwr_sum / samples;

            if (p < MIN_W) {
                finish(fmt("Stopped: %.1f W on %s, no power out.", p, band_name(bands[band_i]).c_str()));
                return;
            }
            float want = gain + 10.0f * log10f(target / p);
            float next = LV_CLAMP(-GAIN_MAX, roundf(want / GAIN_STEP) * GAIN_STEP, GAIN_MAX);
            const std::string name = band_name(bands[band_i]);

            tries++;
            if (p > target && last_p >= 0 && fabsf(p - last_p) < FLAT_W) {
                /* lower gain, the same power: the limiter holds it, the band is at its full power */
                say(fmt("%s: %.1f W, gain %+.1f dB (held by the limiter)", name.c_str(), p, gain), false);
                band_next();
                return;
            }
            last_p = p;
            if (fabsf(p - target) <= CLOSE_W || fabsf(next - gain) < GAIN_STEP / 2) {
                say(fmt("%s: %.1f W, gain %+.1f dB", name.c_str(), p, gain), false);
                band_next();
                return;
            }
            if (next == gain || tries >= TRIES) {
                say(fmt("%s: %.1f W at gain %+.1f dB%s", name.c_str(), p, gain,
                        next == gain && want > GAIN_MAX ? " (the most it gives)" : " (not settled)"), false);
                band_next();
                return;
            }
            say(fmt("%s: %.1f W at %+.1f dB, next %+.1f dB", name.c_str(), p, gain, next), false);
            cfg_sm.p_band_dac_offset.set(next);
            state = S_COOL;
            tick  = 0;
            return;
        }

        case S_COOL:
            if (tick > GRACE_T && radio_get_state() != RADIO_RX) {
                finish("Stopped: something else transmits.");
                return;
            }
            if (tick < COOL_T) return;
            state = S_SWITCH;
            tick  = SWITCH_T;   /* the band is set already */
            return;
    }
    (void)t;
}

bool tx_autocal_start(void) {
    if (timer || radio_get_state() != RADIO_RX || !(radio_caps() & HW_CAP_BAND_GAIN)) {
        return false;
    }
    bands.clear();
    for (const BandInfo &b : BandsTable::all_bands()) {
        /* the ham bands the X6100 sends on: 160 m to 6 m (the broadcast ones are 100 and up) */
        if (static_cast<int>(b.type) == 1 && b.id > 0 && b.id < 100 && b.stop_freq <= 54000000) bands.push_back(b.id);
    }
    if (bands.empty()) return false;

    pthread_mutex_lock(&mux);
    status.clear();
    pthread_mutex_unlock(&mux);
    say(fmt("Target %.1f W (PWR), %zu bands, into a dummy load.", param_f_get(cfg_pwr), bands.size()), true);
    home_band = param_i_get(cfg_band_id);
    band_i    = 0;
    band_begin();
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
    return true;
}

void tx_autocal_stop(const char *why) {
    if (timer) finish(why ? why : "Stopped.");
}

bool tx_autocal_running(void) {
    return timer != NULL;
}

/* The newest lines that fit */
void tx_autocal_status(char *buf, size_t size) {
    pthread_mutex_lock(&mux);
    size_t from = status.size() < size ? 0 : status.find('\n', status.size() - size + 1);

    snprintf(buf, size, "%s", from == std::string::npos ? "" : status.c_str() + from + (from ? 1 : 0));
    pthread_mutex_unlock(&mux);
}

static void socket_start_cb(void *arg) {
    tx_autocal_start();
}

static void socket_stop_cb(void *arg) {
    tx_autocal_stop("Stopped (remote).");
}

bool tx_autocal_socket(const char *cmd, char *out, size_t size, int *l, const char **err) {
    const bool start = !strcmp(cmd, "txcal_start");

    if (start || !strcmp(cmd, "txcal_stop")) {
        if (!scheduler_put_noargs(start ? socket_start_cb : socket_stop_cb)) *err = "Busy, try again";
        return true;
    }
    if (strcmp(cmd, "txcal_get")) return false;

    static char status[1500];   /* the newest lines, the answer fits in 2048 */
    cJSON      *o = cJSON_CreateObject();

    tx_autocal_status(status, sizeof(status));
    cJSON_AddStringToObject(o, "t", "txcal");
    cJSON_AddBoolToObject(o, "running", tx_autocal_running());
    cJSON_AddStringToObject(o, "status", status);
    char *s = cJSON_PrintUnformatted(o);

    *l = snprintf(out, size, "%s\n", s);
    cJSON_free(s);
    cJSON_Delete(o);
    return true;
}
