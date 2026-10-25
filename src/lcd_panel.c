/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers Plus: the radio's state on a KX3-style segment display, laid out as the X6100 KX3 screen
 *  whose author agreed to it, on amber glass: the S, SWR and RF scales on top, VFO A in seven
 *  segments, the filter graphic and VFO B in fourteen segments under it, a spectrum strip (the
 *  spectrum itself, screen_main.c), power, IF, then the time and the voltage on the right. The unlit
 *  segments show as a faint outline. In pixels.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#include "lcd_panel.h"

#include "cfg/cfg_api.h"
#include "clock.h"
#include "freq_scale.h"
#include "info_bar.h"
#include "gui_styles.h"
#include "radio.h"
#include "radio_types.h"
#include "screen_main.h"
#include "smeter.h"
#include "theme.h"
#include "tx_meters.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t      *panel;
static lv_obj_t      *strip;            /* the spectrum strip */
static lv_timer_t    *timer;

/* CW: the decoded text in the strip's right half while keying or while text comes in */
#define CW_HOLD_MS  10000
static pthread_mutex_t cw_mux = PTHREAD_MUTEX_INITIALIZER;
static char            cw_line[160];
static uint16_t        cw_seq;
static uint32_t        cw_ms;           /* lv_tick_get() of the last text */
static lv_coord_t     ox, oy;           /* the drawing's top left on the screen */
static lv_draw_ctx_t *ctx;

/* What is on the glass: drawn again only when it changes */
typedef struct {
    int32_t fa, fb, lo, hi;
    int8_t  mode, agc;
    uint8_t flags;      /* tx, split */
    uint16_t icons;
    int16_t sdb, swr10, pwr10, volts10, minute;
    uint16_t cw_seq;
    bool    cw;
    int32_t bw;
} lcd_state_t;

static lcd_state_t shown;

enum { I_ATT, I_PRE };

static lv_coord_t U(int v) {   /* the places are in pixels */
    return v;
}

static lv_color_t ink(bool lit) {
    return lit ? TC(text) : TC(track);
}

/* Text with its left edge at x and its middle at y (units); returns the x after it */
static int text(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_draw_label_dsc_t dsc;
    lv_point_t          size;

    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.color = ink(lit);
    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (!lit) return x + size.x;
    lv_area_t a = {.x1 = ox + U(x), .y1 = oy + U(y) - size.y / 2};
    a.x2 = a.x1 + size.x;
    a.y2 = a.y1 + size.y;
    lv_draw_label(ctx, &dsc, &a, s, NULL);
    return x + size.x;
}

/* Centred on x */
static void text_c(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_point_t size;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    text(x - size.x / 2, y, s, lit, font);
}

/* Ending at x */
static int text_r(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_point_t size;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    x -= size.x;
    text(x, y, s, lit, font);
    return x;
}

static void rect(int x1, int y1, int x2, int y2, bool lit) {
    lv_draw_rect_dsc_t dsc;

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(lit);
    lv_area_t a = {ox + U(x1), oy + U(y1), ox + U(x2), oy + U(y2)};
    lv_draw_rect(ctx, &dsc, &a);
}

static void line(int x1, int y1, int x2, int y2, int w, bool lit) {
    lv_draw_line_dsc_t dsc;

    if (!lit) return;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = ink(lit);
    dsc.width = w;
    lv_point_t p1 = {ox + U(x1), oy + U(y1)}, p2 = {ox + U(x2), oy + U(y2)};
    lv_draw_line(ctx, &dsc, &p1, &p2);
}

static void poly(const lv_point_t *u, int n, bool lit) {
    lv_draw_rect_dsc_t dsc;
    lv_point_t         p[8];

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(lit);
    for (int i = 0; i < n; i++) p[i] = (lv_point_t){ox + U(u[i].x), oy + U(u[i].y)};
    lv_draw_polygon(ctx, &dsc, p, n);
}

/* A triangle pointing up (dir 0), down (1), left (2) or right (3), centred on x, y */
static void tri(int x, int y, int r, int dir, bool lit) {
    lv_point_t p[3];

    switch (dir) {
        case 0: p[0] = (lv_point_t){x - r, y + r}; p[1] = (lv_point_t){x + r, y + r}; p[2] = (lv_point_t){x, y - r}; break;
        case 1: p[0] = (lv_point_t){x - r, y - r}; p[1] = (lv_point_t){x + r, y - r}; p[2] = (lv_point_t){x, y + r}; break;
        case 2: p[0] = (lv_point_t){x + r, y - r}; p[1] = (lv_point_t){x + r, y + r}; p[2] = (lv_point_t){x - r, y}; break;
        default: p[0] = (lv_point_t){x - r, y - r}; p[1] = (lv_point_t){x - r, y + r}; p[2] = (lv_point_t){x + r, y}; break;
    }
    poly(p, 3, lit);
}

/* A frame around [x1, x2] x [y1, y2], the corners round by r (units) */
static void frame(int x1, int y1, int x2, int y2, int r, bool lit) {
    lv_draw_rect_dsc_t dsc;

    if (!lit) return;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa       = LV_OPA_TRANSP;
    dsc.border_color = ink(lit);
    dsc.border_width = 2;
    dsc.radius       = U(r);
    lv_area_t a = {ox + U(x1), oy + U(y1), ox + U(x2), oy + U(y2)};
    lv_draw_rect(ctx, &dsc, &a);
}

/* How a display's places are drawn */
typedef struct {
    int  dw_pct;        /* the digit's width, % of its place */
    int  thick;         /* the stroke: the digit's width / thick */
    int  lean_pct;      /* the top this % of the height to the right */
    bool outline;       /* the unlit segments as a faint outline (Plus) */
    bool one_point;     /* only the decimal point, before the 10 Hz (Plus); else after MHz and kHz */
} seg_style_t;

static const seg_style_t *style;    /* the display being drawn */
static lv_coord_t         lean_y2;  /* its bottom: the lean is 0 there */

static lv_color_t faint(void) {
    return lv_color_mix(TC(text), TC(bg), 32);
}

static void lean(lv_point_t *p, int n) {
    for (int k = 0; k < n; k++) p[k].x += (lean_y2 - p[k].y) * style->lean_pct / 100;
}

/* One segment from p1 to p2, t thick, pointed at both ends, gap px short of them: filled in ink, or
 * outlined faintly when unlit */
static void stroke(lv_point_t p1, lv_point_t p2, lv_coord_t t, lv_coord_t gap, bool lit) {
    float len = sqrtf((float)(p2.x - p1.x) * (p2.x - p1.x) + (float)(p2.y - p1.y) * (p2.y - p1.y));

    if (len < 1.0f || (!lit && !style->outline)) return;
    float dx = (p2.x - p1.x) / len, dy = (p2.y - p1.y) / len, h = t / 2.0f;
    float ax = p1.x + dx * gap, ay = p1.y + dy * gap, bx = p2.x - dx * gap, by = p2.y - dy * gap;
    lv_point_t p[6] = {
        {ax, ay}, {ax + (dx - dy) * h, ay + (dy + dx) * h}, {bx - (dx + dy) * h, by - (dy - dx) * h},
        {bx, by}, {bx - (dx - dy) * h, by - (dy + dx) * h}, {ax + (dx + dy) * h, ay + (dy - dx) * h},
    };

    lean(p, 6);
    if (lit) {
        lv_draw_rect_dsc_t dsc;

        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = ink(true);
        lv_draw_polygon(ctx, &dsc, p, 6);
        return;
    }
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = faint();
    dsc.width = 1;
    for (int k = 0; k < 6; k++) lv_draw_line(ctx, &dsc, &p[k], &p[(k + 1) % 6]);
}

/* A digit in seven segments (bits a..g: top, top right, bottom right, bottom, bottom left, top
 * left, middle) in box */
static void seg7(const lv_area_t *box, uint8_t lit) {
    lv_coord_t w = lv_area_get_width(box);
    lv_coord_t t = LV_MAX(w / style->thick, 3), g = LV_MAX(1, t / 6);
    lv_coord_t l = box->x1 + t / 2, r = box->x2 - t / 2;
    lv_coord_t top = box->y1 + t / 2, m = (box->y1 + box->y2) / 2, b = box->y2 - t / 2;
    const lv_coord_t s[7][4] = {
        {l, top, r, top}, {r, top, r, m}, {r, m, r, b}, {l, b, r, b}, {l, m, l, b}, {l, top, l, m}, {l, m, r, m},
    };

    for (int i = 0; i < 7; i++)
        stroke((lv_point_t){s[i][0], s[i][1]}, (lv_point_t){s[i][2], s[i][3]}, t, g, lit & 1 << i);
}

/* Fourteen segments: a b c d e f as seven, g1 g2 the two halves of the middle, h i j the
 * diagonal, centre and diagonal over it, k l m under it */
enum { A = 1, B = 2, C = 4, D = 8, E = 16, F = 32, G1 = 64, G2 = 128, H = 256, I = 512, J = 1024, K = 2048,
       L = 4096, M = 8192 };

static uint16_t seg14_of(char c) {
    static const uint16_t digit[10] = {A | B | C | D | E | F, B | C, A | B | G1 | G2 | E | D,
                                       A | B | C | D | G2, F | G1 | G2 | B | C, A | F | G1 | G2 | C | D,
                                       A | F | E | D | C | G1 | G2, A | B | C, A | B | C | D | E | F | G1 | G2,
                                       A | B | C | D | F | G1 | G2};
    static const uint16_t alpha[26] = {
        A | B | C | E | F | G1 | G2, A | B | C | D | I | L | G2, A | D | E | F, A | B | C | D | I | L,
        A | D | E | F | G1 | G2, A | E | F | G1, A | C | D | E | F | G2, B | C | E | F | G1 | G2, A | D | I | L,
        B | C | D | E, E | F | G1 | J | M, D | E | F, B | C | E | F | H | J, B | C | E | F | H | M,
        A | B | C | D | E | F, A | B | E | F | G1 | G2, A | B | C | D | E | F | M, A | B | E | F | G1 | G2 | M,
        A | C | D | F | G1 | G2, A | I | L, B | C | D | E | F, E | F | K | J, B | C | E | F | K | M, H | J | K | M,
        H | J | L, A | D | J | K};

    if (c >= '0' && c <= '9') return digit[c - '0'];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z') return alpha[c - 'A'];
    switch (c) {
        case '-': return G1 | G2;
        case '?': return A | B | G2 | L;
        case '/': return J | K;
        case '=': return G1 | G2 | D;
        case '+': return G1 | G2 | I | L;
        case '<': return J | M;
        case '>': return H | K;
        case '\'': return I;
    }
    return 0;
}

static void seg14(const lv_area_t *box, uint16_t lit) {
    lv_coord_t w = lv_area_get_width(box);
    lv_coord_t t = LV_MAX(w / style->thick, 2), g = LV_MAX(1, t / 3);
    lv_coord_t l = box->x1 + t / 2, r = box->x2 - t / 2, c = (l + r) / 2;
    lv_coord_t top = box->y1 + t / 2, m = (box->y1 + box->y2) / 2, b = box->y2 - t / 2;
    const lv_coord_t s[14][4] = {
        {l, top, r, top}, {r, top, r, m}, {r, m, r, b}, {l, b, r, b}, {l, m, l, b}, {l, top, l, m},
        {l, m, c, m},     {c, m, r, m},   {l, top, c, m}, {c, top, c, m}, {r, top, c, m}, {l, b, c, m},
        {c, m, c, b},     {r, b, c, m},
    };

    for (int i = 0; i < 14; i++)
        stroke((lv_point_t){s[i][0], s[i][1]}, (lv_point_t){s[i][2], s[i][3]}, t, g, lit & 1 << i);
}

/* A frequency on 8 places, 10 Hz the last one: "  14.100.00", the points after places 2 and 5 */
static void freq_cells(int32_t f, char cells[9]) {
    char s[16];

    snprintf(s, sizeof(s), "%d%03d%02d", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f / 10 % 100));
    snprintf(cells, 9, "%8.8s", s + LV_MAX(0, (int)strlen(s) - 8));
}

static const seg_style_t plus_a = {80, 5, 12, true, true}, plus_b = {80, 8, 12, true, true};
static const seg_style_t plus_text = {80, 7, 12, true, false};

/* A point t square at the bottom right of a place, leaning with it */
static void point(lv_coord_t x, lv_coord_t y2, lv_coord_t t) {
    lv_draw_rect_dsc_t dsc;
    lv_point_t         p[4] = {{x, y2 - t}, {x + t, y2 - t}, {x + t, y2}, {x, y2}};

    lean(p, 4);
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = ink(true);
    lv_draw_polygon(ctx, &dsc, p, 4);
}

/* The places of a display: x1..x2, y1..y2 in units, as st says */
static void digits(int x1, int x2, int y1, int y2, const char cells[9], bool fourteen, const seg_style_t *st) {
    static const uint8_t seven[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    int                  pitch = (x2 - x1) / 8, dw = pitch * st->dw_pct / 100;

    style   = st;
    lean_y2 = oy + U(y2);
    for (int i = 0; i < 8; i++) {
        lv_area_t box = {ox + U(x1 + i * pitch), oy + U(y1), ox + U(x1 + i * pitch + dw), oy + U(y2)};
        char      ch  = cells[i];

        if (fourteen)
            seg14(&box, seg14_of(ch));
        else
            seg7(&box, ch >= '0' && ch <= '9' ? seven[ch - '0'] : 0);
        if ((st->one_point ? i == 5 : i == 2 || i == 5) && ch != ' ') {   /* the decimal point; MHz and kHz */
            lv_coord_t t = LV_MAX(U(dw) / st->thick, fourteen ? 2 : 3);
            point(ox + U(x1 + i * pitch + dw + (pitch - dw) / 2) - t / 2, oy + U(y2), t);
        }
    }
}

/* Plus: the layout of the X6100 KX3 screen in the photo its author agreed to, on the amber glass, the
 * volts after the time; in pixels. The spectrum strip is the spectrum itself (screen_main.c) */
#define PLUS_S1     32
#define PLUS_S9     204
#define PLUS_S60    470

static int plus_s_x(int db) {
    if (db <= S1) return PLUS_S1;
    if (db <= S9) return PLUS_S1 + (db - S1) * (PLUS_S9 - PLUS_S1) / (S9 - S1);
    return LV_MIN(PLUS_S60, PLUS_S9 + (db - S9) * (PLUS_S60 - PLUS_S9) / 60);
}

/* A ruler from x1 to x2, its base at y: a tick every step px, the long ones at majors[] */
static void ruler(int x1, int x2, int y, float step, const int *majors, int n) {
    line(x1, y, x2, y, 2, true);
    for (float x = x1; x <= x2 + 0.5f; x += step) line(x, y - 5, x, y, 1, true);
    for (int i = 0; i < n; i++) line(majors[i], y - 11, majors[i], y, 2, true);
}

static void plus_scales(const lcd_state_t *st) {
    static const int  s_major[] = {32, 74, 117, 161, 204, 292, 380, 470};
    static const char *s_text[] = {"S1", "3", "5", "7", "9", "20", "40", "60"};
    static const int  swr_major[] = {31, 108, 184}, rf_major[] = {227, 345, 463};
    bool tx = st->flags & 1;

    for (int i = 0; i < 8; i++) text_c(s_major[i] + (i ? 0 : 6), 17, s_text[i], true, &sony_20);
    ruler(31, 470, 58, 8.6f, s_major, 8);
    if (!tx && st->sdb > S1) rect(31, 46, plus_s_x(st->sdb), 54, true);

    ruler(31, 185, 95, 7.7f, swr_major, 3);
    ruler(227, 463, 95, 11.8f, rf_major, 3);
    text(31, 110, "SWR", true, &sony_20);
    text_c(108, 110, "2", true, &sony_20);
    text_c(184, 110, "3", true, &sony_20);
    text(228, 110, "RF", true, &sony_20);
    text_c(345, 110, "5", true, &sony_20);
    text_c(461, 110, "10", true, &sony_20);
    if (tx) {
        rect(31, 83, 31 + LV_CLAMP(0, (st->swr10 - 10) * 154 / 20, 154), 91, true);   /* 1.0 to 3.0 */
        rect(227, 83, 227 + LV_CLAMP(0, st->pwr10 * 236 / 100, 236), 91, true);      /* 0 to 10 W */
    }
}

static void plus_filter(const lcd_state_t *st) {
    int lo = LV_MIN(abs(st->lo), abs(st->hi)), hi = LV_MAX(abs(st->lo), abs(st->hi));

    for (int i = 0; i < 10; i++) {
        int        x = 46 + i * 15, hz = (i * 2 + 1) * 4000 / 20;
        lv_point_t p[4] = {{x + 8, 280}, {x + 19, 280}, {x + 11, 312}, {x, 312}};
        poly(p, 4, hz >= lo && hz <= hi);
    }
    line(17, 332, 47, 321, 2, true);
    line(47, 321, 183, 321, 2, true);
    line(183, 321, 217, 332, 2, true);
    tri(66, 328, 3, 0, true);
    tri(172, 328, 3, 0, true);
}

static void plus_icons(const lcd_state_t *st) {
    static const char agc[] = {'-', 'S', 'F', 'A'};   /* radio_agc_t order */
    char s[8];

    line(220, 283, 220, 309, 2, true);
    snprintf(s, sizeof(s), "AGC-%c", agc[st->agc & 3]);
    text(245, 300, s, st->agc != radio_agc_off, &sony_22);
    text(328, 300, "PRE", st->icons & 1 << I_PRE, &sony_22);
    text(380, 300, "ATT", st->icons & 1 << I_ATT, &sony_22);
    line(228, 326, 296, 326, 2, true);
    text(299, 326, "XFIL", true, &sony_20);
    line(341, 326, 377, 326, 2, true);
    line(228, 326, 228, 333, 2, true);
    line(377, 326, 377, 333, 2, true);
    text(232, 338, "FL1", true, &sony_16);

    /* the VFOs, the mode */
    bool b = param_i_get(cfg_band_current_vfo) == RADIO_VFO_B;
    frame(546, 9, 590, 42, 4, true);
    text_c(568, 25, b ? "B" : "A", true, &sony_24);
    frame(730, 58, 774, 89, 4, true);
    text_c(752, 73, b ? "A" : "B", true, &sony_24);
    frame(546, 58, 629, 88, 4, st->flags & 2);
    text_c(587, 73, "SPLT", st->flags & 2, &sony_22);
    text_c(668, 73, "TX", st->flags & 1, &sony_22);
    text_r(774, 27, info_params_mode_label_get(), true, &sony_36);
}

static void plus_info(const lcd_state_t *st) {
    char s[32];
    int  x;

    snprintf(s, sizeof(s), "%d.%dW", st->pwr10 / 10, st->pwr10 % 10);
    x = text(30, 427, s, true, &sony_24);
    snprintf(s, sizeof(s), "IF %d.%dk", (int)(st->bw / 1000), (int)(st->bw / 100 % 10));
    text(x + 22, 427, s, true, &sony_24);
    /* the volts after the time, on the right */
    snprintf(s, sizeof(s), "%d.%dV", st->volts10 / 10, st->volts10 % 10);
    x = text_r(770, 427, s, true, &sony_24);
    snprintf(s, sizeof(s), "%02d:%02dZ", st->minute / 60, st->minute % 60);
    text_r(x - 22, 427, s, true, &sony_24);
}

/* CW: the strip's right half, the newest text in fourteen segments, ending at the right */
#define CW_CELLS    16

static void plus_cw(const lcd_state_t *st) {
    char       t[sizeof(cw_line)];
    const int  x1 = LCD_PLUS_STRIP_W / 2 + 8, x2 = 792, pitch = (x2 - x1) / CW_CELLS;
    const int  y1 = LCD_PLUS_STRIP_Y + 13, y2 = LCD_PLUS_STRIP_Y + LCD_PLUS_STRIP_H - 13;

    if (!st->cw) return;
    pthread_mutex_lock(&cw_mux);
    strcpy(t, cw_line);
    pthread_mutex_unlock(&cw_mux);
    frame(LCD_PLUS_STRIP_W / 2, LCD_PLUS_STRIP_Y, 799, LCD_PLUS_STRIP_Y + LCD_PLUS_STRIP_H - 1, 0, true);

    size_t n = strlen(t);
    const char *s = t + (n > CW_CELLS ? n - CW_CELLS : 0);
    int         first = CW_CELLS - (int)strlen(s);

    style   = &plus_text;
    lean_y2 = oy + y2;
    for (int i = 0; i < CW_CELLS; i++) {
        lv_area_t box = {ox + x1 + i * pitch, oy + y1, ox + x1 + i * pitch + pitch * 3 / 4, oy + y2};
        char      ch  = i < first ? ' ' : s[i - first];

        seg14(&box, seg14_of(ch));
        if (ch == '.' || ch == ',') point(box.x2 - 3, box.y2, 4);
    }
}

static void draw_plus(void) {
    char cells[9];

    plus_scales(&shown);
    freq_cells(shown.fa, cells);
    digits(32, 720, 136, 258, cells, false, &plus_a);
    plus_filter(&shown);
    plus_icons(&shown);
    freq_cells(shown.fb, cells);
    digits(430, 782, 285, 338, cells, true, &plus_b);
    plus_info(&shown);
    plus_cw(&shown);
}

static void draw_cb(lv_event_t *e) {
    lv_area_t a;

    ctx = lv_event_get_draw_ctx(e);
    lv_obj_get_coords(panel, &a);
    ox = a.x1;
    oy = a.y1;
    draw_plus();
}

static void read_state(lcd_state_t *st) {
    bool  tx = radio_get_state() == RADIO_TX;
    float pwr = 0, swr = 1;

    memset(st, 0, sizeof(*st));
    st->fa   = cparam_i_get(cfg_fg_freq);
    st->fb   = cparam_i_get(cfg_bg_freq);
    st->lo   = cparam_i_get(cfg_cur_filter_low);
    st->hi   = cparam_i_get(cfg_cur_filter_high);
    st->mode = cparam_i_get(cfg_cur_mode);
    st->agc  = cparam_i_get(cfg_cur_agc);

    float    ext, bat;
    uint8_t  cap;
    bool     charge;
    time_t   now = time(NULL);
    struct tm t;

    if (tx) tx_info_get(&pwr, &swr);
    clock_power_get(&ext, &bat, &cap, &charge);
    gmtime_r(&now, &t);
    st->sdb     = (int16_t)meter_get_db();
    st->swr10   = swr * 10 + 0.5f;
    st->pwr10   = (tx ? pwr : param_f_get(cfg_pwr)) * 10 + 0.5f;
    st->volts10 = (ext >= 3.0f ? ext : bat) * 10 + 0.5f;
    st->minute  = t.tm_hour * 60 + t.tm_min;
    st->bw      = cparam_i_get(cfg_cur_filter_bw);
    pthread_mutex_lock(&cw_mux);
    st->cw_seq = cw_seq;
    st->cw     = (st->mode == radio_mode_cw || st->mode == radio_mode_cwr) &&
                 (tx || (cw_line[0] && lv_tick_elaps(cw_ms) < CW_HOLD_MS));
    pthread_mutex_unlock(&cw_mux);
    st->flags = tx | (param_i_get(cfg_band_split) ? 2 : 0);
    st->icons = (cparam_i_get(cfg_cur_att) ? 1 << I_ATT : 0) | (cparam_i_get(cfg_cur_pre) ? 1 << I_PRE : 0);
}

/* ponytail: polled 10 times a second and drawn again on a change, subscriptions when it matters */
static void poll_cb(lv_timer_t *t) {
    lcd_state_t st;

    read_state(&st);
    if (memcmp(&st, &shown, sizeof(st))) {
        if (strip && st.cw != shown.cw) {   /* CW: the spectrum in the left half, the text in the right */
            lv_obj_set_width(strip, st.cw ? LCD_PLUS_STRIP_W / 2 : LCD_PLUS_STRIP_W);
            freq_scale_set_width(st.cw ? LCD_PLUS_STRIP_W / 2 : LCD_PLUS_STRIP_W);
        }
        shown = st;
        lv_obj_invalidate(panel);
    }
}

static void delete_cb(lv_event_t *e) {
    lv_timer_del(timer);
    timer = NULL;
    panel = NULL;
    strip = NULL;
}

lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height) {
    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, 800, height);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, TC(bg), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(panel, delete_cb, LV_EVENT_DELETE, NULL);
    read_state(&shown);
    timer = lv_timer_create(poll_cb, 100, NULL);
    return panel;
}

void lcd_panel_set_strip(lv_obj_t *spectrum) {
    strip = spectrum;
}

/* Any thread: the CW decoder's text, kept to the last line's worth */
void lcd_panel_cw_text(const char *text) {
    pthread_mutex_lock(&cw_mux);
    size_t n = strlen(cw_line), add = strlen(text);
    if (n + add >= sizeof(cw_line)) {
        size_t drop = n + add - sizeof(cw_line) + 1;
        if (drop > n) drop = n;
        memmove(cw_line, cw_line + drop, n - drop + 1);
        n -= drop;
    }
    snprintf(cw_line + n, sizeof(cw_line) - n, "%s", text);
    for (char *c = cw_line + n; *c; c++) {
        if (*c == '\n' || *c == '\r') *c = ' ';
        if (!strncmp(c, "<?>", 3)) {   /* the decoder's unknown sign: one place */
            *c = '?';
            memmove(c + 1, c + 3, strlen(c + 3) + 1);
        }
    }
    cw_seq++;
    cw_ms = lv_tick_get();
    pthread_mutex_unlock(&cw_mux);
}
