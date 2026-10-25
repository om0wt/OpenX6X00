/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers: the radio's state on a segment display laid out as the LCD drawn in Elecraft's KX3
 *  owner's manual (Rev C5, page 8): the bar graph with its scales and the filter graphic on the
 *  left, VFO A in seven segments, the icons and VFO B in fourteen segments under it, the VFO and
 *  mode icons on the right. As on the backlit glass, only what is lit shows, in ink.
 *  The places are the manual's drawing, 1248 x 313 units, scaled to the 800 px of the screen.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#include "lcd_panel.h"

#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "radio.h"
#include "radio_types.h"
#include "screen_main.h"
#include "smeter.h"
#include "theme.h"
#include "tx_meters.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ART_W   1248
#define ART_H   313

static lv_obj_t      *panel;
static lv_timer_t    *timer;
static lv_coord_t     ox, oy;           /* the drawing's top left on the screen */
static lv_draw_ctx_t *ctx;

/* What is on the glass: drawn again only when it changes */
typedef struct {
    int32_t fa, fb, lo, hi;
    int8_t  mode, agc, bars, swr_bars, rf_bars;
    uint8_t flags;      /* tx, split, lock */
    uint16_t icons;
} lcd_state_t;

static lcd_state_t shown;

enum { I_VOX, I_ATU, I_RIT, I_XIT, I_SPLT, I_ATT, I_PRE, I_NB, I_NR, I_NTCH };

static lv_coord_t U(int v) {
    return v * 800 / ART_W;
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
    if (!lit) return x + size.x * ART_W / 800;
    lv_area_t a = {.x1 = ox + U(x), .y1 = oy + U(y) - size.y / 2};
    a.x2 = a.x1 + size.x;
    a.y2 = a.y1 + size.y;
    lv_draw_label(ctx, &dsc, &a, s, NULL);
    return x + size.x * ART_W / 800;
}

/* Centred on x */
static void text_c(int x, int y, const char *s, bool lit, const lv_font_t *font) {
    lv_point_t size;

    lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    text(x - size.x * ART_W / 1600, y, s, lit, font);
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

static void boxed(int x1, int x2, int y, const char *s, bool lit) {
    frame(x1, y - 16, x2, y + 16, 8, lit);
    text_c((x1 + x2) / 2, y, s, lit, &sony_18);
}

static void lock_icon(int x, int y, bool lit) {
    frame(x - 9, y - 16, x + 9, y + 2, 9, lit);
    rect(x - 14, y - 4, x + 14, y + 16, lit);
}

/* A digit in seven segments (bits a..g: top, top right, bottom right, bottom, bottom left, top
 * left, middle) in box, leaning right like a segment display */
static void seg7(const lv_draw_rect_dsc_t *dsc, const lv_area_t *box, uint8_t lit) {
    lv_coord_t w = lv_area_get_width(box), h = lv_area_get_height(box);
    lv_coord_t t = LV_MAX(w / 6, 3), g = 1;
    lv_coord_t xl = box->x1 + t / 2, xr = box->x2 - t / 2;
    lv_coord_t yt = box->y1 + t / 2, ym = (box->y1 + box->y2) / 2, yb = box->y2 - t / 2;
    /* horizontal: y, from x to x; vertical: x, from y to y */
    const lv_coord_t seg[7][4] = {
        {yt, xl, xr, 0}, {xr, yt, ym, 1}, {xr, ym, yb, 1}, {yb, xl, xr, 0},
        {xl, ym, yb, 1}, {xl, yt, ym, 1}, {ym, xl, xr, 0},
    };

    for (int i = 0; i < 7; i++) {
        if (!(lit & 1 << i)) continue;
        lv_coord_t c = seg[i][0], a = seg[i][1] + g, b = seg[i][2] - g;
        lv_point_t p[6];

        if (seg[i][3]) {
            p[0] = (lv_point_t){c, a};
            p[1] = (lv_point_t){c + t / 2, a + t / 2};
            p[2] = (lv_point_t){c + t / 2, b - t / 2};
            p[3] = (lv_point_t){c, b};
            p[4] = (lv_point_t){c - t / 2, b - t / 2};
            p[5] = (lv_point_t){c - t / 2, a + t / 2};
        } else {
            p[0] = (lv_point_t){a, c};
            p[1] = (lv_point_t){a + t / 2, c - t / 2};
            p[2] = (lv_point_t){b - t / 2, c - t / 2};
            p[3] = (lv_point_t){b, c};
            p[4] = (lv_point_t){b - t / 2, c + t / 2};
            p[5] = (lv_point_t){a + t / 2, c + t / 2};
        }
        for (int k = 0; k < 6; k++) p[k].x += (box->y2 - p[k].y) * w / (8 * h);   /* the lean */
        lv_draw_polygon(ctx, dsc, p, 6);
    }
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
    if (c == '-') return G1 | G2;
    return 0;
}

static void seg14(const lv_area_t *box, uint16_t lit, lv_color_t color) {
    lv_coord_t w = lv_area_get_width(box), h = lv_area_get_height(box);
    lv_coord_t t = LV_MAX(w / 8, 2);
    lv_coord_t l = box->x1 + t / 2, r = box->x2 - t / 2, c = (l + r) / 2;
    lv_coord_t top = box->y1 + t / 2, m = (box->y1 + box->y2) / 2, b = box->y2 - t / 2;
    const lv_coord_t s[14][4] = {
        {l, top, r, top}, {r, top, r, m}, {r, m, r, b}, {l, b, r, b}, {l, m, l, b}, {l, top, l, m},
        {l, m, c, m},     {c, m, r, m},   {l, top, c, m}, {c, top, c, m}, {r, top, c, m}, {l, b, c, m},
        {c, m, c, b},     {r, b, c, m},
    };
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = t;
    for (int i = 0; i < 14; i++) {
        if (!(lit & 1 << i)) continue;
        lv_point_t p1 = {s[i][0], s[i][1]}, p2 = {s[i][2], s[i][3]};
        /* a gap of half the stroke at both ends, the segments apart */
        float len = sqrtf((float)(p2.x - p1.x) * (p2.x - p1.x) + (float)(p2.y - p1.y) * (p2.y - p1.y));
        lv_coord_t dx = (p2.x - p1.x) * t / (2 * len), dy = (p2.y - p1.y) * t / (2 * len);
        p1.x += dx; p1.y += dy; p2.x -= dx; p2.y -= dy;
        p1.x += (box->y2 - p1.y) * w / (8 * h);
        p2.x += (box->y2 - p2.y) * w / (8 * h);
        lv_draw_line(ctx, &dsc, &p1, &p2);
    }
}

/* A frequency on 8 places, 10 Hz the last one: "  14.100.00", the points after places 2 and 5 */
static void freq_cells(int32_t f, char cells[9]) {
    char s[16];

    snprintf(s, sizeof(s), "%d%03d%02d", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f / 10 % 100));
    snprintf(cells, 9, "%8.8s", s + LV_MAX(0, (int)strlen(s) - 8));
}

/* The places of a display: x1..x2, y1..y2 in units; only the lit segments, no outlines of the rest */
static void digits(int x1, int x2, int y1, int y2, const char cells[9], bool fourteen) {
    static const uint8_t seven[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    lv_draw_rect_dsc_t   on;
    int                  pitch = (x2 - x1) / 8, dw = pitch * (fourteen ? 3 : 4) / 5;   /* VFO B: apart */

    lv_draw_rect_dsc_init(&on);
    on.bg_color = ink(true);
    for (int i = 0; i < 8; i++) {
        lv_area_t box = {ox + U(x1 + i * pitch), oy + U(y1), ox + U(x1 + i * pitch + dw), oy + U(y2)};
        char      ch  = cells[i];

        if (fourteen)
            seg14(&box, seg14_of(ch), ink(true));
        else if (ch >= '0' && ch <= '9')
            seg7(&on, &box, seven[ch - '0']);
        if ((i == 2 || i == 5) && ch != ' ') {   /* the point after the MHz and the kHz */
            int px = x1 + i * pitch + dw + (pitch - dw) / 2, d = fourteen ? 4 : 6;
            rect(px - d, y2 - 2 * d, px + d, y2, true);
        }
    }
}

/* The bar graph: 20 bars, S1..S9 on the first 9, +20..+60 on the other 11 */
#define BARS        20
#define BAR_X(i)    (51 + (i) * 17)

static int s_bars(float db) {
    int n = db < S1 ? 0 : db <= S9 ? 1 + (int)((db - S1) / 6) : 9 + (int)((db - S9) * 11 / 60 + 0.5f);
    return LV_CLAMP(0, n, BARS);
}

static void bar_graph(const lcd_state_t *st) {
    static const struct { int bar; const char *s; } top[] = {
        {0, "S1"}, {2, "3"}, {4, "5"}, {6, "7"}, {8, "9"}, {12, "+20"}, {15, "40"}, {19, "60"}};
    bool tx = st->flags & 1;

    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++)
        text_c(BAR_X(top[i].bar) + 5, 73, top[i].s, true, &sony_14);
    text_c(283, 46, "CWT", false, &sony_14);
    tri(283, 90, 7, 1, false);
    frame(318, 38, 360, 54, 8, false);   /* message play/rec */
    line(51, 98, BAR_X(BARS - 1) + 11, 98, 2, true);
    for (int i = 0; i < BARS; i++) {
        line(BAR_X(i) + 5, 92, BAR_X(i) + 5, 98, 2, true);
        bool lit = tx ? (i < 7 ? i < st->swr_bars : i >= 9 && i - 9 < st->rf_bars) : i < st->bars;
        rect(BAR_X(i), 108, BAR_X(i) + 11, 137, lit);
    }
    /* the TX scales: SWR and CMP under the first 7 bars, RF and ALC under the last 11 */
    line(51, 150, BAR_X(6) + 11, 150, 2, true);
    line(BAR_X(9), 150, BAR_X(19) + 11, 150, 2, true);
    for (int i = 0; i < BARS; i++)
        if (i < 7 || i >= 9) line(BAR_X(i) + 5, 150, BAR_X(i) + 5, 156, 2, true);
    text(33, 176, "SWR", true, &sony_14);
    text_c(BAR_X(3) + 5, 176, "2", true, &sony_14);
    text_c(BAR_X(6) + 5, 176, "3", true, &sony_14);
    text(33, 198, "CMP", true, &sony_14);
    text_c(BAR_X(3) + 5, 198, "10", true, &sony_14);
    text_c(BAR_X(6) + 5, 198, "20", true, &sony_14);
    text(BAR_X(9) - 8, 176, "RF", true, &sony_14);
    text_c(BAR_X(14) + 5, 176, "50", true, &sony_14);
    text_c(BAR_X(19) + 5, 176, "100", true, &sony_14);
    text_c(BAR_X(12), 198, "ALC", true, &sony_14);
    line(BAR_X(9), 190, BAR_X(9), 200, 2, true);
    line(BAR_X(9), 200, BAR_X(10), 200, 2, true);
    line(BAR_X(14) + 11, 200, BAR_X(15), 200, 2, true);
    line(BAR_X(15), 190, BAR_X(15), 200, 2, true);
}

/* The filter graphic: 12 slanted stripes over 0..4 kHz of audio, the passband's lit */
static void filter(const lcd_state_t *st) {
    int lo = LV_MIN(abs(st->lo), abs(st->hi)), hi = LV_MAX(abs(st->lo), abs(st->hi));

    text(33, 236, "NTCH", st->icons & 1 << I_NTCH, &sony_14);
    tri(52, 262, 7, 2, false);
    tri(68, 262, 7, 3, false);
    for (int i = 0; i < 12; i++) {
        int        x = 117 + i * 18, hz = (i * 2 + 1) * 4000 / 24;
        lv_point_t p[4] = {{x + 6, 218}, {x + 18, 218}, {x + 12, 258}, {x, 258}};
        poly(p, 4, hz >= lo && hz <= hi);
    }
    line(113, 264, 330, 264, 3, true);
    line(93, 280, 113, 264, 3, true);
    line(330, 264, 348, 280, 3, true);
    tri(155, 274, 5, 0, true);
    tri(286, 274, 5, 0, true);
    text(345, 236, "II", false, &sony_18);
}

static void icons(const lcd_state_t *st) {
    bool on[16];
    for (int i = 0; i < 16; i++) on[i] = st->icons & 1 << i;
    int  x;

    x = text(403, 168, "VOX", on[I_VOX], &sony_18);
    x = text(x + 10, 168, "QSK", false, &sony_18);
    x = text(x + 10, 168, "ANT", false, &sony_18);
    x = text(x, 168, "1", false, &sony_18);
    x = text(x, 168, "2", false, &sony_18);
    text(x + 10, 168, "RX", false, &sony_18);
    text(683, 168, "ATU", on[I_ATU], &sony_18);
    boxed(758, 818, 168, "RIT", on[I_RIT]);
    boxed(836, 896, 168, "XIT", on[I_XIT]);
    boxed(913, 983, 168, "SUB", false);
    boxed(1001, 1081, 168, "SPLT", on[I_SPLT]);

    /* AGC-S slow, AGC-F fast, AGC auto */
    x = text(403, 198, "AGC", st->agc != radio_agc_off, &sony_18);
    x = text(x, 198, "-", st->agc == radio_agc_slow || st->agc == radio_agc_fast, &sony_18);
    x = text(x, 198, "S", st->agc == radio_agc_slow, &sony_18);
    text(x, 198, "F", st->agc == radio_agc_fast, &sony_18);
    text(538, 198, "ATT", on[I_ATT], &sony_18);
    text(608, 198, "PRE", on[I_PRE], &sony_18);

    text(403, 236, "NB", on[I_NB], &sony_18);
    text(403, 265, "NR", on[I_NR], &sony_18);
    line(461, 236, 525, 236, 2, false);
    line(600, 236, 663, 236, 2, false);
    line(461, 236, 461, 248, 2, false);
    line(663, 236, 663, 248, 2, false);
    text_c(562, 236, "XFIL", false, &sony_14);
    for (int i = 0; i < 5; i++) {
        char s[4];
        snprintf(s, sizeof(s), "FL%d", i + 1);
        text(463 + i * 40, 265, s, false, &sony_14);
    }
}

static void vfo_icons(const lcd_state_t *st) {
    bool split = st->flags & 2, tx = st->flags & 1;

    lock_icon(1115, 75, st->flags & 4);
    frame(1101, 101, 1129, 129, 14, true);
    text_c(1115, 115, "A", true, &sony_16);
    tri(1115, 146, 8, 0, !split);
    text_c(1115, 168, "TX", true, &sony_18);
    tri(1115, 190, 8, 1, split);
    frame(1101, 209, 1129, 237, 14, true);
    text_c(1115, 223, "B", true, &sony_16);
    lock_icon(1115, 262, false);
    (void)tx;
}

static void mode_icons(const lcd_state_t *st) {
    int8_t m = st->mode;
    bool   ssb_d = m == radio_mode_lsb_dig || m == radio_mode_usb_dig;
    int    x;

    text(1148, 47, "LSB", m == radio_mode_lsb || m == radio_mode_lsb_dig, &sony_18);
    text(1148, 77, "USB", m == radio_mode_usb || m == radio_mode_usb_dig, &sony_18);
    text(1148, 107, "CW", m == radio_mode_cw || m == radio_mode_cwr, &sony_18);
    text(1148, 138, "REV", m == radio_mode_cwr, &sony_18);
    text(1148, 168, "DATA", ssb_d, &sony_18);
    x = text(1148, 198, "AM", m == radio_mode_am || m == radio_mode_sam, &sony_18);
    text(x, 198, "-S", m == radio_mode_sam, &sony_18);
    x = text(1148, 228, "FM", m == radio_mode_nfm || m == radio_mode_wfm, &sony_18);
    text(x + 10, 228, "T", false, &sony_18);
    x = text(1148, 258, "+", false, &sony_18);
    text(x + 8, 258, "-", false, &sony_18);
}

static void draw_cb(lv_event_t *e) {
    lv_area_t a;
    char      cells[9];

    ctx = lv_event_get_draw_ctx(e);
    lv_obj_get_coords(panel, &a);
    ox = a.x1;
    oy = a.y1 + (lv_area_get_height(&a) - U(ART_H)) / 2;
    bar_graph(&shown);
    filter(&shown);
    freq_cells(shown.fa, cells);
    digits(428, 1078, 37, 133, cells, false);
    icons(&shown);
    freq_cells(shown.fb, cells);
    digits(685, 1078, 203, 275, cells, true);
    vfo_icons(&shown);
    mode_icons(&shown);
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
    st->bars = s_bars(meter_get_db());
    if (tx) {
        tx_info_get(&pwr, &swr);
        st->swr_bars = LV_CLAMP(0, (int)((swr - 1.0f) * 3 + 0.5f), 7);   /* 1.0 to 3.0 on 7 bars */
        st->rf_bars  = LV_CLAMP(0, (int)(pwr * 11 / 10 + 0.5f), 11);     /* 0 to 10 W on 11 bars */
    }
    st->flags = tx | (param_i_get(cfg_band_split) ? 2 : 0) | (main_screen_freq_locked() ? 4 : 0);
    st->icons = (param_i_get(cfg_vox_on) ? 1 << I_VOX : 0) | (param_i_get(cfg_atu_enabled) ? 1 << I_ATU : 0) |
                (param_i_get(cfg_rit) ? 1 << I_RIT : 0) | (param_i_get(cfg_xit) ? 1 << I_XIT : 0) |
                (param_i_get(cfg_band_split) ? 1 << I_SPLT : 0) | (cparam_i_get(cfg_cur_att) ? 1 << I_ATT : 0) |
                (cparam_i_get(cfg_cur_pre) ? 1 << I_PRE : 0) | (param_i_get(cfg_nb) ? 1 << I_NB : 0) |
                (param_i_get(cfg_nr) ? 1 << I_NR : 0) | (param_i_get(cfg_dnf) ? 1 << I_NTCH : 0);
}

/* ponytail: polled 10 times a second and drawn again on a change, subscriptions when it matters */
static void poll_cb(lv_timer_t *t) {
    lcd_state_t st;

    read_state(&st);
    if (memcmp(&st, &shown, sizeof(st))) {
        shown = st;
        lv_obj_invalidate(panel);
    }
}

static void delete_cb(lv_event_t *e) {
    lv_timer_del(timer);
    timer = NULL;
    panel = NULL;
}

lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height) {
    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, 800, height);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, TC(bg), 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, TC(text), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(panel, delete_cb, LV_EVENT_DELETE, NULL);
    read_state(&shown);
    timer = lv_timer_create(poll_cb, 100, NULL);
    return panel;
}
