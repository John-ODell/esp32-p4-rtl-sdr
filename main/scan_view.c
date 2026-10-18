#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpi_lcd.h"
#include "rpi_canvas.h"
#include "pins.h"
#include "view.h"
#include "scan_view.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

#define BAR_H  48
#define ROW_H  38
#define NROWS  ((LCD_H - BAR_H - 2) / ROW_H)

typedef struct { int x, w, id; const char *label; } skey_t;
static const skey_t s_keys_wifi[] = {{208, 86, SV_MORE, "MORE"}, {298, 86, SV_TOGGLE, "CHAN"}, {388, 86, SV_EXIT, "EXIT"}};
static const skey_t s_keys_chan[] = {{298, 86, SV_TOGGLE, "LIST"}, {388, 86, SV_EXIT, "EXIT"}};
static const skey_t s_keys_ble[] = {{298, 86, SV_MORE, "MORE"}, {388, 86, SV_EXIT, "EXIT"}};

static uint16_t *s_px;
static int s_kind, s_page, s_hot = -1;
static bool s_chan;
static int s_total;                           /* rows in the list, for paging */
static view_colors_t K;
static scan_view_info_t s_last;

void scan_view_init(void)
{
    s_px = BIG_ALLOC(LCD_W * LCD_H * 2);
    if (!s_px) abort();
}

void scan_view_open(int kind)
{
    s_kind = kind;
    s_page = 0;
    s_chan = false;
    s_hot = -1;
}

static const skey_t *keys(int *n)
{
    if (s_kind == SV_BLE) { *n = 2; return s_keys_ble; }
    if (s_chan) { *n = 2; return s_keys_chan; }
    *n = 3;
    return s_keys_wifi;
}

static int pages(void) { return s_total ? (s_total + NROWS - 1) / NROWS : 1; }

static void draw_key(const rpi_canvas_t *c, const skey_t *k, bool hot)
{
    int r = K.square ? 0 : 8, y = 4, h = BAR_H - 8;
    rpi_cv_rrect(c, k->x, y, k->w, h, r, hot ? K.btn_on : K.edge);
    rpi_cv_rrect(c, k->x + 1, y + 1, k->w - 2, h - 2, r > 1 ? r - 1 : 0, hot ? rpi_mix565(K.btn, K.btn_on, 110) : K.btn);
    rpi_cv_text(c, k->x + (k->w - rpi_cv_text_w(k->label, 2)) / 2, y + (h - 32) / 2, k->label, K.key_txt, 2);
}

static void draw_bar(const rpi_canvas_t *c, const scan_view_info_t *in)
{
    rpi_cv_fill(c, 0, 0, LCD_W, BAR_H, K.btn == K.bg ? K.bg : rpi_mix565(K.bg, K.btn, 90));
    rpi_cv_fill(c, 0, BAR_H - 1, LCD_W, 1, K.edge);
    rpi_cv_text(c, 8, 8, s_kind == SV_BLE ? "BLE" : "WI-FI", K.accent, 2);
    char a[32], b[32];
    int n = s_kind == SV_BLE ? in->nble : in->nap;
    snprintf(a, sizeof a, s_kind == SV_BLE ? "%d devices" : "%d networks", n);
    if (s_kind == SV_WIFI && s_chan) snprintf(b, sizeof b, "sweep %d", in->scans);
    else snprintf(b, sizeof b, "page %d/%d", s_page + 1, pages());
    int nk;
    const skey_t *k = keys(&nk);
    int x = s_kind == SV_BLE ? 60 : 100, maxx = k[0].x - 6;
    if (x + rpi_cv_text_w(a, 1) <= maxx) rpi_cv_text(c, x, 6, a, K.txt, 1);
    if (x + rpi_cv_text_w(b, 1) <= maxx) rpi_cv_text(c, x, 24, b, K.dim, 1);
    for (int i = 0; i < nk; i++) draw_key(c, &k[i], i == s_hot);
}

/* four signal bars, 20 px wide, bottom at y */
static void bars(const rpi_canvas_t *c, int x, int y, int rssi)
{
    int lit = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = 4 + 4 * i;
        rpi_cv_fill(c, x + i * 5, y - h, 4, h, i < lit ? K.accent : K.track);
    }
}

static void message(const rpi_canvas_t *c, const char *a, const char *b)
{
    rpi_cv_text(c, (LCD_W - rpi_cv_text_w(a, 2)) / 2, 150, a, K.accent, 2);
    if (b) rpi_cv_text(c, (LCD_W - rpi_cv_text_w(b, 1)) / 2, 192, b, K.dim, 1);
}

static void age(char *b, size_t n, int64_t ms)
{
    int s = (int)(ms / 1000) % 100000;
    if (s < 60) snprintf(b, n, "%ds", s); else snprintf(b, n, "%dm", s / 60);
}

/* ------------------------------------------------------------------------- Wi-Fi -- */

static int by_rssi_ap(const void *a, const void *b)
{
    return ((const scan_ap_t *)b)->rssi - ((const scan_ap_t *)a)->rssi;
}

static void draw_wifi_list(const rpi_canvas_t *c, const scan_view_info_t *in)
{
    for (int r = 0; r < NROWS; r++) {
        int i = s_page * NROWS + r;
        if (i >= in->nap) break;
        const scan_ap_t *a = &in->ap[i];
        int y = BAR_H + 1 + r * ROW_H;
        if (r % 2) rpi_cv_fill(c, 0, y, LCD_W, ROW_H, rpi_mix565(K.bg, K.btn, 70));
        bars(c, 8, y + 26, a->rssi);
        char t[48];
        if (a->ssid[0]) rpi_cv_text(c, 36, y + 2, a->ssid, K.txt, 1);
        else rpi_cv_text(c, 36, y + 2, "(hidden)", K.dim, 1);
        snprintf(t, sizeof t, "ch %d", a->ch);
        rpi_cv_text(c, 316, y + 2, t, K.txt, 1);
        snprintf(t, sizeof t, "%d dBm", a->rssi);
        rpi_cv_text(c, LCD_W - 8 - rpi_cv_text_w(t, 1), y + 2, t, K.accent, 1);
        snprintf(t, sizeof t, "%02X:%02X:%02X:%02X:%02X:%02X", a->bssid[0], a->bssid[1], a->bssid[2],
                 a->bssid[3], a->bssid[4], a->bssid[5]);
        rpi_cv_text(c, 36, y + 19, t, K.dim, 1);
        bool open = !strcmp(a->sec, "OPEN");
        rpi_cv_text(c, 316, y + 19, a->sec, open ? K.accent : K.dim, 1);
        age(t, sizeof t, in->now_ms - a->seen_ms);
        rpi_cv_text(c, LCD_W - 8 - rpi_cv_text_w(t, 1), y + 19, t, K.dim, 1);
    }
}

/* line clipped to rows y0..y1 */
static void seg(const rpi_canvas_t *c, int x0, int ya, int x1, int yb, int y0, int y1, uint16_t col)
{
    int dx = abs(x1 - x0), dy = -abs(yb - ya), sx = x0 < x1 ? 1 : -1, sy = ya < yb ? 1 : -1, e = dx + dy;
    for (;;) {
        if (ya >= y0 && ya <= y1 && x0 >= 0 && x0 < c->w) c->px[ya * c->w + x0] = col;
        if (x0 == x1 && ya == yb) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; ya += sy; }
    }
}

/* channels 1-13 (2.4 GHz). Each network is a hump 20 MHz wide (+-2 channels) up to its level,
 * like a Wi-Fi analyzer; the strongest few are named at their peak. */
static void draw_wifi_chan(const rpi_canvas_t *c, const scan_view_info_t *in)
{
    const int x0 = 44, x1 = LCD_W - 10, y0 = BAR_H + 14, y1 = LCD_H - 34;
    const int lo = -100, hi = -20;
    /* domain: channel -1 .. 15 so the edge humps fit */
    #define CX(ch10) (x0 + (int)((long)((ch10) + 10) * (x1 - x0) / 160))
    #define LY(db) (y1 - (int)((long)((db) - lo) * (y1 - y0) / (hi - lo)))
    char t[24];
    for (int db = -90; db <= hi; db += 10) {
        int y = LY(db);
        for (int x = x0; x < x1; x += 3) c->px[y * LCD_W + x] = K.track;
        snprintf(t, sizeof t, "%d", db);
        rpi_cv_text(c, x0 - 6 - rpi_cv_text_w(t, 1), y - 8, t, K.dim, 1);
    }
    rpi_cv_fill(c, x0, y1, x1 - x0, 1, K.edge);
    int count[15] = {0};
    for (int i = 0; i < in->nap; i++) if (in->ap[i].ch >= 1 && in->ap[i].ch <= 14) count[in->ap[i].ch]++;
    for (int ch = 1; ch <= 13; ch++) {
        int x = CX(ch * 10);
        snprintf(t, sizeof t, "%d", ch);
        rpi_cv_text(c, x - rpi_cv_text_w(t, 1) / 2, y1 + 2, t, ch == 1 || ch == 6 || ch == 11 ? K.txt : K.dim, 1);
        if (count[ch]) {
            snprintf(t, sizeof t, "%d", count[ch]);
            rpi_cv_text(c, x - rpi_cv_text_w(t, 1) / 2, y1 + 17, t, K.accent, 1);
        }
    }
    rpi_cv_text(c, 4, y1 + 17, "APs", K.dim, 1);
    /* weakest first so the strong ones end up on top; in->ap is sorted strongest first */
    const uint16_t pal[4] = {K.accent, K.txt, rpi_mix565(K.accent, K.txt, 128), rpi_mix565(K.bg, K.accent, 170)};
    for (int i = in->nap - 1; i >= 0; i--) {
        const scan_ap_t *a = &in->ap[i];
        if (a->ch < 1 || a->ch > 14) continue;
        uint16_t col = i < 6 ? pal[i % 4] : rpi_mix565(K.bg, K.txt, 110);
        int top = a->rssi < lo ? lo : a->rssi > hi ? hi : a->rssi;
        int px = -1, py = -1;
        for (int d = -20; d <= 20; d++) {        /* tenths of a channel */
            double u = d / 20.0;
            int y = LY(lo + (top - lo) * (1 - u * u * u * u));
            int x = CX(a->ch * 10 + d);
            if (px >= 0) seg(c, px, py, x, y, y0, y1, col);
            px = x;
            py = y;
        }
    }
    /* names of the strongest few at their peaks, if they fit */
    int used[6][4], nu = 0;
    for (int i = 0; i < in->nap && nu < 6; i++) {
        const scan_ap_t *a = &in->ap[i];
        if (a->ch < 1 || a->ch > 14) continue;
        const char *nm = a->ssid[0] ? a->ssid : "(hidden)";
        char s[14];
        size_t l = strlen(nm) < sizeof s - 1 ? strlen(nm) : sizeof s - 1;   /* first 13 characters */
        memcpy(s, nm, l);
        s[l] = 0;
        int w = rpi_cv_text_w(s, 1), x = CX(a->ch * 10) - w / 2, y = LY(a->rssi < hi ? a->rssi : hi) - 18;
        if (x < x0) x = x0;
        if (x + w > x1) x = x1 - w;
        if (y < y0) y = y0;
        bool clash = false;
        for (int k = 0; k < nu; k++)
            if (x < used[k][2] && used[k][0] < x + w && y < used[k][3] && used[k][1] < y + 16) clash = true;
        if (clash) continue;
        used[nu][0] = x; used[nu][1] = y; used[nu][2] = x + w; used[nu][3] = y + 16; nu++;
        rpi_cv_text(c, x, y, s, i < 6 ? pal[i % 4] : K.txt, 1);
    }
    #undef CX
    #undef LY
}

/* --------------------------------------------------------------------------- BLE -- */

static int by_rssi_ble(const void *a, const void *b)
{
    return ((const scan_ble_t *)b)->rssi - ((const scan_ble_t *)a)->rssi;
}

static void draw_ble_list(const rpi_canvas_t *c, const scan_view_info_t *in)
{
    for (int r = 0; r < NROWS; r++) {
        int i = s_page * NROWS + r;
        if (i >= in->nble) break;
        const scan_ble_t *d = &in->ble[i];
        int y = BAR_H + 1 + r * ROW_H;
        if (r % 2) rpi_cv_fill(c, 0, y, LCD_W, ROW_H, rpi_mix565(K.bg, K.btn, 70));
        bars(c, 8, y + 26, d->rssi);
        char t[48];
        if (d->name[0]) rpi_cv_text(c, 36, y + 2, d->name, K.txt, 1);
        else rpi_cv_text(c, 36, y + 2, d->kind[0] ? d->kind : "(no name)", d->kind[0] ? K.txt : K.dim, 1);
        if (d->name[0] && d->kind[0]) rpi_cv_text(c, 260, y + 2, d->kind, K.dim, 1);
        snprintf(t, sizeof t, "%d dBm", d->rssi);
        rpi_cv_text(c, LCD_W - 8 - rpi_cv_text_w(t, 1), y + 2, t, K.accent, 1);
        snprintf(t, sizeof t, "%02X:%02X:%02X:%02X:%02X:%02X%s", d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5], d->random ? " rnd" : "");
        rpi_cv_text(c, 36, y + 19, t, K.dim, 1);
        char ag[16];
        age(ag, sizeof ag, in->now_ms - d->seen_ms);
        snprintf(t, sizeof t, "%s%5u adv %4s", d->connectable ? "conn " : "     ", (unsigned)d->count, ag);
        rpi_cv_text(c, LCD_W - 8 - rpi_cv_text_w(t, 1), y + 19, t, K.dim, 1);
    }
}

/* ------------------------------------------------------------------------ public -- */

void scan_view_draw(const scan_view_info_t *in)
{
    s_last = *in;
    view_colors(&K);
    if (in->kind == SV_WIFI) qsort((void *)in->ap, in->nap, sizeof *in->ap, by_rssi_ap);
    else qsort((void *)in->ble, in->nble, sizeof *in->ble, by_rssi_ble);
    s_total = in->kind == SV_WIFI ? in->nap : in->nble;
    if (s_page >= pages()) s_page = 0;
    rpi_canvas_t c = {s_px, LCD_W, LCD_H};
    rpi_cv_fill(&c, 0, 0, LCD_W, LCD_H, K.bg);
    draw_bar(&c, in);
    if (in->status) message(&c, in->status, NULL);
    else if (in->kind == SV_WIFI && !in->nap)
        message(&c, "scanning 2.4 GHz", "channels 1-13, about 2 s a sweep");
    else if (in->kind == SV_BLE && !in->nble)
        message(&c, "listening for BLE", "phones, watches, tags and beacons show up here");
    else if (in->kind == SV_WIFI && s_chan) draw_wifi_chan(&c, in);
    else if (in->kind == SV_WIFI) draw_wifi_list(&c, in);
    else draw_ble_list(&c, in);
    rpi_lcd_blit(0, 0, LCD_W, LCD_H, s_px);
}

static void redraw_bar(void)
{
    rpi_canvas_t c = {s_px, LCD_W, LCD_H};
    draw_bar(&c, &s_last);
    rpi_lcd_blit(0, 0, LCD_W, BAR_H, s_px);
}

int scan_view_touch(int ev, int x, int y)
{
    int nk, hit = -1;
    const skey_t *k = keys(&nk);
    if (y < BAR_H + 6)
        for (int i = 0; i < nk; i++) if (x >= k[i].x - 3 && x < k[i].x + k[i].w + 3) hit = i;
    int fired = SV_NONE, old = s_hot;
    if (ev == SV_RELEASE) {
        if (s_hot >= 0 && s_hot == hit) fired = k[hit].id;
        s_hot = -1;
    } else {
        s_hot = hit;                             /* follow the finger */
    }
    if (fired == SV_TOGGLE) { s_chan = !s_chan; return fired; }
    if (fired == SV_MORE) { s_page = (s_page + 1) % pages(); return fired; }
    if (old != s_hot) redraw_bar();
    return fired;
}
