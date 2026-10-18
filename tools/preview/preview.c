/* Render the receiver screen on a PC: synthetic IQ (a few FM stations plus noise) through the
 * real spectrum.c and view.c, into a 480x320 frame buffer written as a PNG.
 *
 * Build: make -C tools/preview */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include "rpi_lcd.h"
#include "pins.h"
#include "spectrum.h"
#include "view.h"
#include "panel.h"
#include "adsb.h"
#include "adsb_view.h"
#include "amap.h"
#include "scan_view.h"

#define RATE 2400000
#define NAVG 24
#define NFFT 1024

static uint16_t fb[LCD_H][LCD_W];

esp_err_t rpi_lcd_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < LCD_W && j >= 0 && j < LCD_H) fb[j][i] = c;
    return ESP_OK;
}

esp_err_t rpi_lcd_blit(int x, int y, int w, int h, const uint16_t *p)
{
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return ESP_ERR_INVALID_ARG;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) fb[y + j][x + i] = p[j * w + i];
    return ESP_OK;
}

/* ---- synthetic IQ ---- */
typedef struct { double off_hz, amp, dev_hz, bw_hz, phase, audio, fade; } station_t;   /* last 3: state */
static station_t st[] = {
    {-760e3, 0.30, 75e3, 6000}, {-560e3, 0.010, 60e3, 3000}, {-300e3, 0.05, 70e3, 9000},
    {0, 0.12, 70e3, 5000},      {430e3, 0.02, 50e3, 4000},   {880e3, 0.20, 75e3, 7000},
    {1050e3, 0.008, 3e3, 300},
};
#define NST (sizeof st / sizeof st[0])

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* FM stations whose "audio" is low-passed noise, plus a slow fade per frame. */
static void synth(uint8_t *iq, int n)
{
    for (int s = 0; s < (int)NST; s++) {
        station_t *p = &st[s];
        p->fade += 0.15 * (gauss() * 0.3 - p->fade);
    }
    for (int k = 0; k < n; k++) {
        double i = 0.012 * gauss(), q = 0.012 * gauss();
        for (int s = 0; s < (int)NST; s++) {
            station_t *p = &st[s];
            double a = 2 * M_PI * p->bw_hz / RATE;
            p->audio += a * (2.2 * gauss() - p->audio);
            double m = p->audio > 1 ? 1 : p->audio < -1 ? -1 : p->audio;
            p->phase += 2 * M_PI * (p->off_hz + p->dev_hz * m) / RATE;
            double amp = p->amp * (1 + p->fade);
            i += amp * cos(p->phase);
            q += amp * sin(p->phase);
        }
        int a = (int)lrint(127.4 + 128 * i), b = (int)lrint(127.4 + 128 * q);
        iq[2 * k] = a < 0 ? 0 : a > 255 ? 255 : a;
        iq[2 * k + 1] = b < 0 ? 0 : b > 255 ? 255 : b;
    }
}

/* ---- PNG ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t crc = crc32(crc32(0, (const uint8_t *)type, 4), d, n);
    put32(h, crc);
    fwrite(h, 1, 4, f);
}

static void write_png(const char *path, const uint16_t *px, int W, int H)
{
    size_t raw_n = (size_t)H * (1 + W * 3);
    uint8_t *raw = malloc(raw_n), *z = malloc(compressBound(raw_n));
    uint8_t *o = raw;
    for (int y = 0; y < H; y++) {
        *o++ = 0;
        for (int x = 0; x < W; x++) {
            uint16_t c = px[y * W + x];
            int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
            *o++ = r << 3 | r >> 2; *o++ = g << 2 | g >> 4; *o++ = b << 3 | b >> 2;
        }
    }
    uLongf zn = compressBound(raw_n);
    compress2(z, &zn, raw, raw_n, 9);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13];
    put32(ih, W); put32(ih + 4, H);
    ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

static const int s_g[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
static int gain_tenths(int i) { return i <= 0 ? -1 : s_g[(i - 1) % 29]; }

static const char *gain_label(int i)
{
    static char b[16];
    if (i <= 0) return "AGC";
    snprintf(b, sizeof b, "%d.%d dB", gain_tenths(i) / 10, gain_tenths(i) % 10);
    return b;
}

/* usage: preview OUT_DIR [freq_hz] [pressed_button]
 * Writes OUT_DIR/<theme>.png and <theme>-controls.png for every theme, and OUT_DIR/themes.png. */
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    uint32_t freq = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 100000000;
    int pressed = argc > 3 ? atoi(argv[3]) : -1;
    static uint8_t iq[NAVG * 1024 * 2];
    static float db[NFFT_MAX];
    spectrum_init();
    view_init(RATE);
    panel_init();
    int n = view_themes(), cols = 2, rows = n, gap = 8;   /* left: receiver, right: Controls */
    int SW = cols * LCD_W + (cols + 1) * gap, SH = rows * LCD_H + (rows + 1) * gap;
    uint16_t *sheet = calloc((size_t)SW * SH, 2);
    for (int i = 0; i < SW * SH; i++) sheet[i] = 0x4208;
    for (int t = 0; t < n; t++) {
        srand(1);
        view_set_theme(t);
        view_reset();
        view_header(freq, "AGC", pressed);
        view_axis(freq);
        for (int frame = 0; frame < LCD_H; frame++) {
            synth(iq, NAVG * NFFT);
            spectrum_compute(iq, NAVG, db);
            view_render(db, NFFT, freq);
        }
        view_blit();
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        int ox = gap, oy = gap + t * (LCD_H + gap);
        for (int y = 0; y < LCD_H; y++) memcpy(&sheet[(oy + y) * SW + ox], fb[y], LCD_W * 2);
        ctl_t ct = {.gain_idx = 17, .gain_n = 30, .gain_label = gain_label, .gain_tenths = gain_tenths,
                    .range_auto = false, .ref_db = -20, .range_db = 60, .zoom = 1, .bw = 2, .band = 1};
        panel_open(&ct);
        snprintf(path, sizeof path, "%s/%s-controls.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        ox += LCD_W + gap;
        for (int y = 0; y < LCD_H; y++) memcpy(&sheet[(oy + y) * SW + ox], fb[y], LCD_W * 2);
        /* every sub page: tap its menu tile, save, tap BACK */
        static const char *pg[] = {"gain", "range", "zoom", "filter", "band", "scan", "look"};
        int top = view_tune_y(), ph = LCD_H - top, cw = (LCD_W - 12 - 18) / 4, chh = (ph - 12 - 6) / 2;
        for (int i = 0; i < 7; i++) {
            int tx = 6 + cw / 2 + (i % 4) * (cw + 6), ty = top + 6 + chh / 2 + (i / 4) * (chh + 6);
            panel_touch(&ct, PANEL_PRESS, tx, ty, 0);
            panel_touch(&ct, PANEL_RELEASE, tx, ty, 0);
            snprintf(path, sizeof path, "%s/%s-%s.png", dir, view_theme_name(t), pg[i]);
            write_png(path, &fb[0][0], LCD_W, LCD_H);
            panel_touch(&ct, PANEL_PRESS, 60, top + 28, 0);           /* BACK */
            panel_touch(&ct, PANEL_RELEASE, 60, top + 28, 0);
        }
        /* a key lit under the finger (pressed, not yet released) */
        panel_touch(&ct, PANEL_PRESS, 240, top + 6 + chh / 2, 0);
        snprintf(path, sizeof path, "%s/%s-pressed.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        panel_touch(&ct, PANEL_MOVE, 240, 10, 0);                    /* slide off: nothing fires */
        panel_touch(&ct, PANEL_RELEASE, 240, 10, 0);
        printf("%s\n", path);
    }
    /* ADS-B list and map with made-up traffic around a home point */
    adsb_init();
    adsb_view_init();
    static const struct { uint32_t icao; const char *fl; int alt, spd, trk, vr; double dlat, dlon; } tr[] = {
        {0x4840D6, "KLM1023", 38000, 452, 95, 0, 0.18, -0.30},  {0x40621D, "BAW9KX", 24500, 388, 250, -1600, -0.10, 0.22},
        {0x3C6585, "DLH4AB", 36000, 470, 300, 0, 0.35, 0.40},    {0x485020, "", 7800, 210, 183, -832, -0.06, -0.05},
        {0x4CA2B1, "RYR81QP", 31000, 430, 40, 1200, -0.32, -0.42}, {0xA1B2C3, "", -2147483647 - 1, -1, 0, 0, 0, 0},
        {0x478F41, "SAS1405", 12000, 280, 140, -2200, 0.02, 0.12},
    };
    /* AMAP=path/MAP.BIN draws that base map under the traffic (tools/mapgen makes one) */
    const char *mp = getenv("AMAP");
    if (mp) {
        FILE *f = fopen(mp, "rb");
        static uint8_t mapbuf[16 << 20];
        size_t len = f ? fread(mapbuf, 1, sizeof mapbuf, f) : 0;
        if (f) fclose(f);
        if (!amap_load(mapbuf, len)) { fprintf(stderr, "%s: not a map\n", mp); return 1; }
        printf("map %s: %d features\n", amap_info()->name, amap_info()->nfeat);
    }
    const double hlat = mp ? amap_info()->lat : 52.31, hlon = mp ? amap_info()->lon : 4.76;
    for (int i = 0; i < (int)(sizeof tr / sizeof tr[0]); i++) {
        adsb_ac_t a = {0};
        a.icao = tr[i].icao;
        snprintf(a.flight, sizeof a.flight, "%s", tr[i].fl);
        a.alt_ft = tr[i].alt; a.speed_kt = tr[i].spd; a.track_deg = tr[i].trk; a.vrate_fpm = tr[i].vr;
        a.has_pos = tr[i].dlat != 0 || tr[i].dlon != 0;
        a.lat = hlat + tr[i].dlat; a.lon = hlon + tr[i].dlon;
        double h = tr[i].trk * M_PI / 180;
        for (int k = 0; k < 8 && a.has_pos; k++) {             /* trail behind it */
            a.trail_lat[k] = (float)(a.lat - (8 - k) * 0.006 * cos(h));
            a.trail_lon[k] = (float)(a.lon - (8 - k) * 0.010 * sin(h));
        }
        a.ntrail = a.has_pos ? 8 : 0;
        a.msgs = 40 + 37 * i;
        a.seen_ms = -1000 * i;
        adsb_test_add(&a);
    }
    adsb_view_info_t info = {.msgs_per_s = 212, .gain_tenths = 496, .have_home = true, .home_lat = hlat, .home_lon = hlon};
    for (int t = 0; t < n; t++) {
        view_set_theme(t);
        char path[512];
        adsb_view_open();
        adsb_view_draw(&info);
        snprintf(path, sizeof path, "%s/%s-adsb-list.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        adsb_view_touch(AV_PRESS, 340, 20);                    /* MAP */
        adsb_view_touch(AV_RELEASE, 340, 20);
        adsb_view_draw(&info);
        snprintf(path, sizeof path, "%s/%s-adsb-map.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        if (mp) {                                              /* zoomed out twice, then in */
            static const struct { int x, n; const char *tag; } z[] = {{200, 2, "200"}, {264, 4, "10"}};
            for (int k = 0; k < 2; k++) {
                for (int j = 0; j < z[k].n; j++) {
                    adsb_view_touch(AV_PRESS, z[k].x, 20);
                    adsb_view_touch(AV_RELEASE, z[k].x, 20);
                }
                adsb_view_draw(&info);
                snprintf(path, sizeof path, "%s/%s-adsb-map%s.png", dir, view_theme_name(t), z[k].tag);
                write_png(path, &fb[0][0], LCD_W, LCD_H);
            }
            for (int j = 0; j < 2; j++) { adsb_view_touch(AV_PRESS, 200, 20); adsb_view_touch(AV_RELEASE, 200, 20); }
        }
    }
    /* Wi-Fi and BLE scanners with made-up neighbours */
    static scan_ap_t ap[] = {
        {"Cafe-Guest", {0x3C, 0x84, 0x6A, 0x11, 0x20, 0x01}, -48, 6, "OPEN", -1000},
        {"NETGEAR52", {0xA0, 0x40, 0xA0, 0x7E, 0x22, 0x13}, -57, 1, "WPA2", -2000},
        {"ATT8kq2Pw", {0x88, 0x96, 0x4E, 0x01, 0x9A, 0xC2}, -63, 11, "WPA2/3", 0},
        {"", {0x8A, 0x96, 0x4E, 0x01, 0x9A, 0xC3}, -64, 11, "WPA2", 0},
        {"Spectrum-5G-Lobby", {0x10, 0x56, 0x11, 0x88, 0x01, 0x0F}, -71, 6, "WPA2", -3000},
        {"HP-Print-4B-OfficeJet", {0xDA, 0x12, 0x65, 0x4B, 0x00, 0x01}, -74, 6, "WPA2", -5000},
        {"Xfinity", {0x92, 0x1A, 0x22, 0x33, 0x44, 0x55}, -78, 3, "OPEN", -2000},
        {"MyRing-7731", {0x34, 0x3E, 0xA4, 0x77, 0x31, 0x02}, -81, 9, "WPA2", -9000},
        {"TP-Link_E2C4", {0x50, 0xC7, 0xBF, 0xE2, 0xC4, 0x10}, -84, 1, "WPA2", -4000},
        {"Tesla-Model-Y", {0x4C, 0xFC, 0xAA, 0x01, 0x02, 0x03}, -88, 13, "WPA2", -12000},
    };
    static scan_ble_t ble[] = {
        {{0x5A, 0x11, 0x2B, 0x9C, 0x00, 0x41}, true, true, -52, "", "Apple", 312, 0},
        {{0xD4, 0x3A, 0x2C, 0x18, 0x77, 0x01}, false, true, -60, "LE-Bose QC45", "", 88, -1000},
        {{0x71, 0x02, 0xC3, 0x55, 0xE1, 0x9D}, true, false, -66, "", "Microsoft", 140, 0},
        {{0xE8, 0x9F, 0x6D, 0x01, 0x22, 0x33}, false, true, -70, "Galaxy Watch5", "Samsung", 64, -2000},
        {{0x43, 0x9A, 0x10, 0x01, 0x02, 0x03}, true, false, -77, "", "Fast Pair", 22, -5000},
        {{0xC1, 0x77, 0x23, 0x90, 0x41, 0x0A}, true, false, -83, "", "Tile", 9, -14000},
        {{0x08, 0x3A, 0xF2, 0x10, 0x20, 0x30}, false, true, -89, "ESP32-C6 test", "Espressif", 5, -31000},
        {{0x66, 0x01, 0x02, 0x03, 0x04, 0x05}, true, false, -94, "", "", 3, -40000},
    };
    scan_view_init();
    for (int t = 0; t < n; t++) {
        view_set_theme(t);
        char path[512];
        scan_view_info_t wi = {.kind = SV_WIFI, .ap = ap, .nap = sizeof ap / sizeof ap[0], .now_ms = 0, .scans = 14};
        scan_view_open(SV_WIFI);
        scan_view_draw(&wi);
        snprintf(path, sizeof path, "%s/%s-wifi-list.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        scan_view_touch(SV_PRESS, 340, 20);                    /* CHAN */
        scan_view_touch(SV_RELEASE, 340, 20);
        scan_view_draw(&wi);
        snprintf(path, sizeof path, "%s/%s-wifi-chan.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
        scan_view_info_t bi = {.kind = SV_BLE, .ble = ble, .nble = sizeof ble / sizeof ble[0], .now_ms = 0};
        scan_view_open(SV_BLE);
        scan_view_draw(&bi);
        snprintf(path, sizeof path, "%s/%s-ble.png", dir, view_theme_name(t));
        write_png(path, &fb[0][0], LCD_W, LCD_H);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/themes.png", dir);
    write_png(path, sheet, SW, SH);
    printf("%s\n", path);
    return 0;
}
