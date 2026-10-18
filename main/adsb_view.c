#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "rpi_canvas.h"
#include "pins.h"
#include "view.h"
#include "adsb_view.h"
#include "amap.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

#define BAR_H  48
#define ROW_H  44
#define NROWS  ((LCD_H - BAR_H - 4) / ROW_H)

typedef struct { int x, w, id; const char *label; } akey_t;
static const akey_t s_keys_list[] = {{298, 86, AV_TOGGLE, "MAP"}, {388, 86, AV_EXIT, "EXIT"}};
static const akey_t s_keys_map[] = {{170, 60, AV_ZOOM_OUT, "-"}, {234, 60, AV_ZOOM_IN, "+"},
                                   {298, 86, AV_TOGGLE, "LIST"}, {388, 86, AV_EXIT, "EXIT"}};
static const int s_ranges[] = {5, 10, 25, 50, 100, 200, 400};

static uint16_t *s_px, *s_bg;              /* frame; base map cache (rows BAR_H..) */
static struct { int range, theme; double lat, lon; bool valid; } s_bgkey;
static bool s_map;
static int s_hot = -1, s_range = 3;
static view_colors_t K;
static adsb_view_info_t s_last;

void adsb_view_init(void)
{
    s_px = BIG_ALLOC(LCD_W * LCD_H * 2);
    s_bg = BIG_ALLOC(LCD_W * LCD_H * 2);
    if (!s_px || !s_bg) abort();
}

bool adsb_view_is_map(void) { return s_map; }
int adsb_view_range_km(void) { return s_ranges[s_range]; }

static const akey_t *keys(int *n)
{
    *n = s_map ? 4 : 2;
    return s_map ? s_keys_map : s_keys_list;
}

static void draw_key(const rpi_canvas_t *c, const akey_t *k, bool hot)
{
    int r = K.square ? 0 : 8, y = 4, h = BAR_H - 8;
    rpi_cv_rrect(c, k->x, y, k->w, h, r, hot ? K.btn_on : K.edge);
    rpi_cv_rrect(c, k->x + 1, y + 1, k->w - 2, h - 2, r > 1 ? r - 1 : 0, hot ? rpi_mix565(K.btn, K.btn_on, 110) : K.btn);
    rpi_cv_text(c, k->x + (k->w - rpi_cv_text_w(k->label, 2)) / 2, y + (h - 32) / 2, k->label, K.key_txt, 2);
}

static void draw_bar(const rpi_canvas_t *c, const adsb_view_info_t *in)
{
    rpi_cv_fill(c, 0, 0, LCD_W, BAR_H, K.btn == K.bg ? K.bg : rpi_mix565(K.bg, K.btn, 90));
    rpi_cv_fill(c, 0, BAR_H - 1, LCD_W, 1, K.edge);
    rpi_cv_text(c, 8, 8, "ADS-B", K.accent, 2);
    char a[48], b[32];
    int n = adsb_count();
    if (!s_map && n > NROWS) snprintf(a, sizeof a, "%d aircraft, %d shown", n, NROWS);
    else snprintf(a, sizeof a, "%d aircraft", n);
    snprintf(b, sizeof b, "%.0f msg/s", in->msgs_per_s);
    int x = 100, maxx = (s_map ? s_keys_map[0].x : s_keys_list[0].x) - 6;
    if (x + rpi_cv_text_w(a, 1) <= maxx) rpi_cv_text(c, x, 6, a, K.txt, 1);
    if (x + rpi_cv_text_w(b, 1) <= maxx) rpi_cv_text(c, x, 24, b, K.dim, 1);
    int nk;
    const akey_t *k = keys(&nk);
    for (int i = 0; i < nk; i++) draw_key(c, &k[i], i == s_hot);
}

/* newest first */
static int by_seen(const void *a, const void *b)
{
    const adsb_ac_t *x = *(const adsb_ac_t *const *)a, *y = *(const adsb_ac_t *const *)b;
    return x->seen_ms < y->seen_ms ? 1 : x->seen_ms > y->seen_ms ? -1 : 0;
}

static int sorted(const adsb_ac_t **out)
{
    int n = adsb_count();
    for (int i = 0; i < n; i++) out[i] = adsb_get(i);
    qsort(out, n, sizeof out[0], by_seen);
    return n;
}

static void draw_list(const rpi_canvas_t *c)
{
    const adsb_ac_t *ac[ADSB_MAX_AC];
    int n = sorted(ac);
    int64_t now = adsb_now_ms();
    if (!n) {
        const char *a = "listening on 1090 MHz", *b = "aircraft show up here; an antenna at a window helps";
        rpi_cv_text(c, (LCD_W - rpi_cv_text_w(a, 2)) / 2, 150, a, K.accent, 2);
        rpi_cv_text(c, (LCD_W - rpi_cv_text_w(b, 1)) / 2, 192, b, K.dim, 1);
        return;
    }
    for (int r = 0; r < n && r < NROWS; r++) {
        const adsb_ac_t *a = ac[r];
        int y = BAR_H + 2 + r * ROW_H;
        if (r % 2) rpi_cv_fill(c, 0, y, LCD_W, ROW_H, rpi_mix565(K.bg, K.btn, 70));
        char id[16], l1[96], l2[96], alt[24], spd[24], hdg[24], vr[24];
        if (a->flight[0]) snprintf(id, sizeof id, "%s", a->flight);
        else snprintf(id, sizeof id, "%06X", (unsigned)a->icao);
        rpi_cv_text(c, 8, y + 6, id, a->flight[0] ? K.accent : K.dim, 2);
        if (a->alt_ft != INT32_MIN) snprintf(alt, sizeof alt, "%d ft", a->alt_ft); else snprintf(alt, sizeof alt, "--- ft");
        if (a->speed_kt >= 0) {
            snprintf(spd, sizeof spd, "%d kt", a->speed_kt);
            snprintf(hdg, sizeof hdg, "hdg %03d", a->track_deg);
            snprintf(vr, sizeof vr, "%+d fpm", a->vrate_fpm);
        } else {
            snprintf(spd, sizeof spd, "-- kt");
            snprintf(hdg, sizeof hdg, "hdg ---");
            vr[0] = 0;
        }
        snprintf(l1, sizeof l1, "%-9s %-7s %s", alt, spd, hdg);
        int age = (int)((now - a->seen_ms) / 1000);
        snprintf(l2, sizeof l2, "%06X %-10s %s %3ds %4u msg", (unsigned)a->icao, vr, a->has_pos ? "pos" : "   ",
                 age, (unsigned)a->msgs);
        rpi_cv_text(c, 152, y + 4, l1, K.txt, 1);
        rpi_cv_text(c, 152, y + 22, l2, K.dim, 1);
    }
}

/* ---------------------------------------------------------------------------- map -- */

static void line(const rpi_canvas_t *c, int x0, int y0, int x1, int y1, uint16_t col)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < c->w && y0 >= 0 && y0 < c->h) c->px[y0 * c->w + x0] = col;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void circle(const rpi_canvas_t *c, int cx, int cy, int r, uint16_t col)
{
    for (int a = 0; a < 360; a += 2) {
        double t = a * M_PI / 180;
        int x = cx + (int)lrint(r * sin(t)), y = cy - (int)lrint(r * cos(t));
        if (x >= 0 && x < c->w && y >= 0 && y < c->h) c->px[y * c->w + x] = col;
    }
}

/* filled triangle by scanlines */
static void tri(const rpi_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col)
{
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny; y <= maxy; y++) {
        int xs[3], n = 0;
        int px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2};
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y))
                xs[n++] = px[i] + (y - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
        }
        if (n == 2) {
            int a = xs[0] < xs[1] ? xs[0] : xs[1], b = xs[0] < xs[1] ? xs[1] : xs[0];
            rpi_cv_fill(c, a, y, b - a + 1, 1, col);
        }
    }
}

/* the SD-card base map in this look's colours, redrawn only when range, look or centre move */
static void base_map(const rpi_canvas_t *c, const adsb_view_info_t *in, int cx, int cy, double km_px)
{
    const amap_info_t *m = amap_info();
    if (!m || !in->have_home) return;
    int theme = view_theme();
    if (!s_bgkey.valid || s_bgkey.range != s_range || s_bgkey.theme != theme || s_bgkey.lat != in->home_lat ||
        s_bgkey.lon != in->home_lon) {
        rpi_canvas_t b = {s_bg, LCD_W, LCD_H};
        rpi_cv_fill(&b, 0, BAR_H, LCD_W, LCD_H - BAR_H, K.bg);
        /* the map is drawn around its own centre; shift if the home point is elsewhere */
        const double kx = 111.32 * cos(m->lat * M_PI / 180), ky = 110.57;
        int mx = cx + (int)lrint((m->lon - in->home_lon) * kx / km_px);
        int my = cy - (int)lrint((m->lat - in->home_lat) * ky / km_px);
        amap_colors_t col = {
            .coast = rpi_mix565(K.bg, K.accent, 150), .water = rpi_mix565(K.bg, K.accent, 80),
            .river = rpi_mix565(K.bg, K.accent, 60), .border = rpi_mix565(K.bg, K.txt, 90),
            .road = rpi_mix565(K.bg, K.txt, 45), .motorway = rpi_mix565(K.bg, K.txt, 85),
            .runway = rpi_mix565(K.bg, K.txt, 200), .place = rpi_mix565(K.bg, K.txt, 130),
            .airport = rpi_mix565(K.bg, K.accent, 190),
        };
        amap_draw(&b, BAR_H + 1, mx, my, 1.0 / km_px, s_ranges[s_range], &col);
        s_bgkey.valid = true;
        s_bgkey.range = s_range;
        s_bgkey.theme = theme;
        s_bgkey.lat = in->home_lat;
        s_bgkey.lon = in->home_lon;
    }
    memcpy(c->px + BAR_H * LCD_W, s_bg + BAR_H * LCD_W, (size_t)LCD_W * (LCD_H - BAR_H) * 2);
}

static void draw_map(const rpi_canvas_t *c, const adsb_view_info_t *in)
{
    const int cx = LCD_W / 2, cy = BAR_H + (LCD_H - BAR_H) / 2, R = (LCD_H - BAR_H) / 2 - 6;
    const double km_px = s_ranges[s_range] / (double)R;
    base_map(c, in, cx, cy, km_px);
    uint16_t ring = K.track, lbl = K.dim;
    for (int k = 1; k <= 4; k++) circle(c, cx, cy, R * k / 4, ring);
    line(c, cx - 6, cy, cx + 6, cy, lbl);
    line(c, cx, cy - 6, cx, cy + 6, lbl);
    char t[32];
    for (int k = 2; k <= 4; k += 2) {
        snprintf(t, sizeof t, "%d km", s_ranges[s_range] * k / 4);
        rpi_cv_text(c, cx + 4, cy - R * k / 4 + 2, t, lbl, 1);
    }
    rpi_cv_text(c, cx - 4 - 12, cy - R - 6, "N", K.accent, 1);
    if (!in->have_home) {
        const char *m = "waiting for a first position";
        rpi_cv_text(c, (LCD_W - rpi_cv_text_w(m, 1)) / 2, cy + 20, m, K.dim, 1);
        return;
    }
    const amap_info_t *m = amap_info();
    if (m && m->name[0]) snprintf(t, sizeof t, "%s", m->name);
    else snprintf(t, sizeof t, "%.3f %.3f", in->home_lat, in->home_lon);
    rpi_cv_text(c, 6, LCD_H - 18, t, lbl, 1);
    const double kx = 111.32 * cos(in->home_lat * M_PI / 180), ky = 110.57;
    for (int i = 0; i < adsb_count(); i++) {
        const adsb_ac_t *a = adsb_get(i);
        if (!a->has_pos) continue;
        int nt = a->ntrail < ADSB_TRAIL ? a->ntrail : ADSB_TRAIL;
        for (int k = 0; k < nt; k++) {
            int x = cx + (int)lrint((a->trail_lon[k] - in->home_lon) * kx / km_px);
            int y = cy - (int)lrint((a->trail_lat[k] - in->home_lat) * ky / km_px);
            if (x >= 1 && x < LCD_W - 1 && y > BAR_H && y < LCD_H - 1) rpi_cv_fill(c, x, y, 2, 2, K.dim);
        }
        int x = cx + (int)lrint((a->lon - in->home_lon) * kx / km_px);
        int y = cy - (int)lrint((a->lat - in->home_lat) * ky / km_px);
        if (x < 0 || x >= LCD_W || y <= BAR_H || y >= LCD_H) continue;
        double h = (a->speed_kt >= 0 ? a->track_deg : 0) * M_PI / 180, s = sin(h), co = cos(h);
        /* arrow: nose 9 px ahead, wings 6 px back and to the sides */
        tri(c, x + (int)lrint(9 * s), y - (int)lrint(9 * co),
            x + (int)lrint(-6 * s + 5 * co), y - (int)lrint(-6 * co - 5 * s),
            x + (int)lrint(-6 * s - 5 * co), y - (int)lrint(-6 * co + 5 * s), K.accent);
        char id[12];
        if (a->flight[0]) snprintf(id, sizeof id, "%s", a->flight); else snprintf(id, sizeof id, "%06X", (unsigned)a->icao);
        rpi_cv_text(c, x + 10, y - 8, id, K.txt, 1);
        if (a->alt_ft != INT32_MIN) {
            snprintf(t, sizeof t, "%d", a->alt_ft / 100);
            rpi_cv_text(c, x + 10, y + 6, t, K.dim, 1);
        }
    }
}

/* ------------------------------------------------------------------------ public -- */

void adsb_view_draw(const adsb_view_info_t *in)
{
    s_last = *in;
    view_colors(&K);
    rpi_canvas_t c = {s_px, LCD_W, LCD_H};
    rpi_cv_fill(&c, 0, 0, LCD_W, LCD_H, K.bg);
    draw_bar(&c, in);
    if (s_map) draw_map(&c, in);
    else draw_list(&c);
    rpi_lcd_blit(0, 0, LCD_W, LCD_H, s_px);
}

void adsb_view_open(void)
{
    s_map = false;
    s_bgkey.valid = false;
    s_hot = -1;
}

static void redraw_bar(void)
{
    rpi_canvas_t c = {s_px, LCD_W, LCD_H};
    draw_bar(&c, &s_last);
    rpi_lcd_blit(0, 0, LCD_W, BAR_H, s_px);
}

int adsb_view_touch(int ev, int x, int y)
{
    int nk, hit = -1;
    const akey_t *k = keys(&nk);
    if (y < BAR_H + 6)
        for (int i = 0; i < nk; i++) if (x >= k[i].x - 3 && x < k[i].x + k[i].w + 3) hit = i;
    int fired = AV_NONE, old = s_hot;
    if (ev == AV_RELEASE) {
        if (s_hot >= 0 && s_hot == hit) fired = k[hit].id;
        s_hot = -1;
    } else {
        s_hot = hit;                             /* follow the finger */
    }
    if (fired == AV_TOGGLE) { s_map = !s_map; return fired; }
    if (fired == AV_ZOOM_IN && s_range > 0) s_range--;
    if (fired == AV_ZOOM_OUT && s_range < (int)(sizeof s_ranges / sizeof s_ranges[0]) - 1) s_range++;
    if (old != s_hot) redraw_bar();
    return fired;
}
