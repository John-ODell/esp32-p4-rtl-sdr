/* Receiver loop: IQ -> spectrum -> view (drawing lives in view.c and panel.c), touch, settings.
 *
 * Tap the spectrum or waterfall to tune there. Hold a step button to repeat. Tap the frequency
 * readout for the next look (or pick one on the LOOK page). Tap MENU (top right, gain shown under it) for Controls (gain, dB range, zoom,
 * audio: filter and mode, volume, squelch; bands). Touch is sampled three times a frame; menu keys and tuning act when the finger lifts. Everything is kept in NVS namespace "sdr": freq, gaint (tenths of a dB,
 * -1 = AGC), theme, rauto, ref, rspan, zoom, bw, vol, sql, mem (blob: the channel memories); logn is the SD log session (sdlog.c). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "xpt2046.h"
#include "pins.h"
#include "sdr.h"
#include "rtlsdr_usb.h"
#include "spectrum.h"
#include "view.h"
#include "panel.h"
#include "adsb.h"
#include "adsb_view.h"
#include "amap.h"
#include "sdcard.h"
#include "sdlog.h"
#include "scan_view.h"
#include "radio_scan.h"
#include "audio.h"
#include "rds.h"
#include "ui.h"

static const char *TAG = "ui";

#define IQ_PER_FRAME (24 * 1024)            /* samples per frame (10 ms), split into FFTs */

static uint8_t *s_iq;
static float s_db[NFFT_MAX];
static ctl_t s_ctl = {.range_auto = true, .ref_db = -20, .range_db = 60, .band = -1,
                      .gain_label = sdr_gain_label, .gain_tenths = sdr_gain_tenths};
static bool s_panel;                        /* Controls panel is up */
static bool s_adsb, s_adsb_req;             /* in ADS-B mode / asked for it */
static int s_av_action;                     /* last ADS-B screen key fired */
static bool s_scan;                         /* in a Wi-Fi / BLE scanner */
static int s_scan_req = -1;                 /* SV_WIFI / SV_BLE asked for */
static int s_sv_action;                     /* last scanner key fired */
static bool s_mscan;                        /* hopping through the channel memories */
static int s_mscan_i = -1;                  /* slot tuned by the scan */
static int64_t s_hop_at, s_dwell_until;

static void draw_header(int pressed) { view_header(sdr_freq(), sdr_gain_label(sdr_gain_index()), pressed); }

/* ---------------------------------------------------------------------- settings -- */

static nvs_handle_t s_nvs;
static int64_t s_save_at;

static void settings_changed(void) { s_save_at = esp_timer_get_time() + 3000000; }

static void settings_maybe_save(void)
{
    if (!s_save_at || esp_timer_get_time() < s_save_at) return;
    s_save_at = 0;
    nvs_set_u32(s_nvs, "freq", sdr_freq());
    nvs_set_i32(s_nvs, "gaint", sdr_gain_tenths(sdr_gain_index()));
    nvs_set_i32(s_nvs, "theme", view_theme());
    nvs_set_i32(s_nvs, "rauto", s_ctl.range_auto);
    nvs_set_i32(s_nvs, "ref", s_ctl.ref_db);
    nvs_set_i32(s_nvs, "rspan", s_ctl.range_db);
    nvs_set_i32(s_nvs, "zoom", s_ctl.zoom);
    nvs_set_i32(s_nvs, "bw", s_ctl.bw);
    nvs_set_i32(s_nvs, "vol", s_ctl.vol);
    nvs_set_i32(s_nvs, "sql", s_ctl.sql);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "saved %" PRIu32 " Hz, gain %s, zoom %d, bw %d", sdr_freq(),
             sdr_gain_label(sdr_gain_index()), s_ctl.zoom, s_ctl.bw);
}

static int32_t nvs_int(const char *key, int32_t def)
{
    int32_t v = def;
    nvs_get_i32(s_nvs, key, &v);
    return v;
}

/* zoom, filter and scale from s_ctl into the FFT and the view */
static void apply_view(void)
{
    int n = 1024 << s_ctl.zoom;
    spectrum_set_size(n > NFFT_MAX ? NFFT_MAX : n);
    view_set_span(panel_zoom_hz(s_ctl.zoom));
    view_set_passband(panel_bw_hz(s_ctl.bw));
    view_set_range(s_ctl.range_auto, s_ctl.ref_db, s_ctl.range_db);
}

/* the filter picks the demodulator: 200k WFM, 25k and 12.5k NFM, 8k AM */
static void apply_audio(void)
{
    demod_mode_t m = s_ctl.bw == 0 ? DEMOD_WFM : s_ctl.bw == PANEL_BWS - 1 ? DEMOD_AM : DEMOD_NFM;
    audio_set_mode(m, panel_bw_hz(s_ctl.bw));
    audio_set_volume(s_ctl.vol);
    audio_set_squelch(s_ctl.sql);
}

/* ----------------------------------------------------------------------- memories -- */

static void save_memories(void)
{
    struct { uint32_t hz[PANEL_MEMS]; int8_t bw[PANEL_MEMS]; } m;
    memcpy(m.hz, s_ctl.mem_hz, sizeof m.hz);
    memcpy(m.bw, s_ctl.mem_bw, sizeof m.bw);
    nvs_set_blob(s_nvs, "mem", &m, sizeof m);
    nvs_commit(s_nvs);
}

static void load_memories(void)
{
    struct { uint32_t hz[PANEL_MEMS]; int8_t bw[PANEL_MEMS]; } m;
    size_t len = sizeof m;
    if (nvs_get_blob(s_nvs, "mem", &m, &len) != ESP_OK || len != sizeof m) return;
    for (int i = 0; i < PANEL_MEMS; i++) {
        s_ctl.mem_hz[i] = m.hz[i];
        s_ctl.mem_bw[i] = m.bw[i] >= 0 && m.bw[i] < PANEL_BWS ? m.bw[i] : 0;
    }
}

/* a memory's frequency and filter (so also its mode) */
static void tune_memory(int i)
{
    s_ctl.bw = s_ctl.mem_bw[i];
    s_ctl.band = -1;
    sdr_set_freq(s_ctl.mem_hz[i]);
    audio_retuned();
    view_set_passband(panel_bw_hz(s_ctl.bw));   /* not apply_view: that clears the waterfall */
    apply_audio();
    s_ctl.freq = sdr_freq();
    if (!s_panel) view_axis(sdr_freq());
    draw_header(-1);
    settings_changed();
}

static void mscan_stop(void)
{
    if (!s_mscan) return;
    s_mscan = false;
    ESP_LOGI(TAG, "memory scan stopped on M%d", s_mscan_i + 1);
}

/* Once a frame while scanning: stay while the squelch is open and 2 s after, otherwise hop to
 * the next stored channel every 300 ms. The squelch is not believed for the first 80 ms after a
 * hop, while IQ from the last channel is still coming through. */
static void mscan_step(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_hop_at > 80000 && audio_squelch_open()) {
        if (s_dwell_until < now + 2000000) s_dwell_until = now + 2000000;
        return;
    }
    if (now < s_dwell_until) return;
    for (int k = 1; k <= PANEL_MEMS; k++) {
        int i = (s_mscan_i + k) % PANEL_MEMS;
        if (!s_ctl.mem_hz[i]) continue;
        s_mscan_i = i;
        if (s_ctl.mem_hz[i] != sdr_freq() || s_ctl.mem_bw[i] != s_ctl.bw) tune_memory(i);
        s_hop_at = now;
        s_dwell_until = now + 300000;
        return;
    }
    mscan_stop();                            /* nothing stored any more */
}

static void mscan_start(void)
{
    if (!s_ctl.sql) {                        /* an open squelch would never let it move on */
        s_ctl.sql = 3;
        apply_audio();
        settings_changed();
        ESP_LOGI(TAG, "memory scan: squelch was open, set to 3");
    }
    s_mscan = true;
    s_mscan_i = -1;
    s_dwell_until = 0;
    ESP_LOGI(TAG, "memory scan");
}

/* RDS names and texts go to the log when they change */
static void log_rds(const rds_info_t *r)
{
    static char ps[9], rt[65];
    if (r->has_ps && strcmp(ps, r->ps)) {
        strcpy(ps, r->ps);
        ESP_LOGI(TAG, "RDS PI %04X, station \"%s\"", r->pi, ps);
    }
    if (r->has_rt && strcmp(rt, r->rt)) {
        strcpy(rt, r->rt);
        ESP_LOGI(TAG, "RDS text \"%s\"", rt);
    }
}

/* the plate on the spectrum: mode and volume, the memory being scanned, or on broadcast FM the
 * RDS station name once it is in; lit while sound is coming out */
static void show_audio(void)
{
    if (!audio_ok()) { view_set_audio(NULL, false); return; }
    char l[24];
    rds_info_t r;
    rds_get(&r);
    if (s_mscan) {
        snprintf(l, sizeof l, "SCAN M%d %s", s_mscan_i + 1, panel_bw_mode(s_ctl.bw));
    } else if (s_ctl.bw == 0 && r.has_ps) {
        log_rds(&r);
        int n = 8;
        while (n > 0 && r.ps[n - 1] == ' ') n--;
        snprintf(l, sizeof l, "WFM %.*s", n, r.ps);
    } else if (!s_ctl.vol) snprintf(l, sizeof l, "%s off", panel_bw_mode(s_ctl.bw));
    else if (s_ctl.sql && !audio_squelch_open()) snprintf(l, sizeof l, "%s %d  SQL", panel_bw_mode(s_ctl.bw), s_ctl.vol);
    else snprintf(l, sizeof l, "%s %d", panel_bw_mode(s_ctl.bw), s_ctl.vol);
    view_set_audio(l, s_ctl.vol && audio_squelch_open());
}

/* -------------------------------------------------------------------------- touch -- */

static void retune(uint32_t hz)
{
    sdr_set_freq(hz);
    audio_retuned();
    view_axis(sdr_freq());
    settings_changed();
}

static void panel_show(bool on)
{
    s_panel = on;
    if (on) {
        s_ctl.freq = sdr_freq();
        s_ctl.gain_idx = sdr_gain_index();
        s_ctl.gain_n = sdr_gain_presets();
        panel_open(&s_ctl);
    } else {
        view_axis(sdr_freq());                 /* spectrum and waterfall come back next frame */
    }
}

static void set_look(int i)
{
    view_set_theme(i);                         /* clears the screen */
    apply_view();
    ESP_LOGI(TAG, "look: %s", view_theme_name(view_theme()));
    if (s_panel) panel_redraw(&s_ctl);
    else view_axis(sdr_freq());
    settings_changed();
}

/* landscape <-> portrait: saved at once (main.c reads it before the splash), then every
 * screen is laid out again */
static void turn(void)
{
    bool p = !LCD_PORTRAIT;
    rpi_lcd_set_portrait(p);
    nvs_set_i32(s_nvs, "port", p);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "%s", p ? "portrait" : "landscape");
    set_look(view_theme());
    draw_header(-1);
}

static void do_action(int act)
{
    uint32_t f = sdr_freq();
    switch (act) {
    case VIEW_DN1M:   retune(f > 1000000 ? f - 1000000 : 0); break;
    case VIEW_DN100K: retune(f > 100000 ? f - 100000 : 0); break;
    case VIEW_UP100K: retune(f + 100000); break;
    case VIEW_UP1M:   retune(f + 1000000); break;
    case VIEW_GAIN:   panel_show(!s_panel); break;
    case VIEW_THEME:  set_look((view_theme() + 1) % view_themes()); break;
    }
    draw_header(act);                       /* keep the key lit while held */
}

static void panel_event(int ev, int x, int y, int64_t now)
{
    int ch = panel_touch(&s_ctl, ev, x, y, now);
    if (ch & CTL_CLOSE) { panel_show(false); return; }
    if (ch & CTL_TURN) {
        turn();
        return;
    }
    if (ch & CTL_LOOK) {
        set_look(s_ctl.look);
        draw_header(-1);
        return;
    }
    if (ch & (CTL_WIFI | CTL_BLE)) {         /* scanners on the C6: their own mode */
        panel_show(false);
        s_scan_req = ch & CTL_WIFI ? SV_WIFI : SV_BLE;
        return;
    }
    if (ch & CTL_GAIN) {
        sdr_set_gain_index(s_ctl.gain_idx);
        draw_header(-1);
    }
    if (ch & CTL_BAND) {
        uint32_t f;
        int bw, zoom;
        const char *name = panel_band(s_ctl.band, &f, &bw, &zoom);
        ESP_LOGI(TAG, "band %s", name);
        if (!strcmp(name, "ADS-B")) {        /* its own mode: decoder, list and map */
            panel_show(false);
            s_adsb_req = true;
            return;
        }
        sdr_set_freq(f);
        audio_retuned();
        draw_header(-1);
    }
    if (ch & (CTL_ZOOM | CTL_BW | CTL_RANGE)) ESP_LOGI(TAG, "zoom %d, filter %d, range %s %d/%d", s_ctl.zoom,
                                                       s_ctl.bw, s_ctl.range_auto ? "auto" : "manual",
                                                       s_ctl.ref_db, s_ctl.range_db);
    if (ch & CTL_MEM_SAVE) {
        save_memories();
        ESP_LOGI(TAG, "M%d: %" PRIu32 " Hz, %s", s_ctl.mem + 1, s_ctl.mem_hz[s_ctl.mem], panel_bw_mode(s_ctl.mem_bw[s_ctl.mem]));
    }
    if (ch & CTL_MEM_TUNE) {                 /* the panel stays up: tap through and listen */
        tune_memory(s_ctl.mem);
        panel_redraw(&s_ctl);
        return;
    }
    if (ch & CTL_MEM_SCAN) {
        panel_show(false);
        mscan_start();
        return;
    }
    if (ch & (CTL_RANGE | CTL_ZOOM | CTL_BW | CTL_BAND)) apply_view();
    if (ch & (CTL_BW | CTL_BAND | CTL_AUDIO)) apply_audio();
    if (ch) settings_changed();
}

/* Touch tracker. The XPT2046 reads noisily as the finger lands and lifts, so the first
 * sample after pen-down is dropped, later ones are smoothed, and the lift-off position is
 * the last smoothed one. Events: press (first good sample), move, release. */
static void poll_touch(const touch_cal_t *cal)
{
    static bool down;
    static int n, x, y;
    static int where;                       /* 0 header, 1 panel, 2 spectrum, 3 ADS-B, 4 scanner, 5 none */
    static int act = -1;
    static int64_t next_repeat;
    uint16_t rx, ry, rz;
    int64_t now = esp_timer_get_time();

    if (!xpt2046_read_raw(&rx, &ry, &rz)) {
        if (down && n > 0) {                /* release */
            if (where == 3) s_av_action = adsb_view_touch(AV_RELEASE, x, y);
            else if (where == 4) s_sv_action = scan_view_touch(SV_RELEASE, x, y);
            else if (where == 0 && act >= 0) draw_header(-1);
            else if (where == 1) panel_event(PANEL_RELEASE, x, y, now);
            else if (where == 2 && !s_panel) {
                int step = s_ctl.zoom == 0 ? 10000 : s_ctl.zoom == 1 ? 5000 : 1000;
                int64_t f = (int64_t)sdr_freq() + (int64_t)(x - LCD_W / 2) * view_hz_per_px();
                f = (f + step / 2) / step * step;
                retune(f > 0 ? (uint32_t)f : 0);
                draw_header(-1);
            }
        }
        down = false;
        n = 0;
        act = -1;
        return;
    }
    int px, py;
    touch_cal_map(cal, rx, ry, &px, &py);
    if (!down) { down = true; n = 0; return; }       /* landing sample: drop it */
    if (n++ == 0) {                                   /* press */
        x = px; y = py;
        if (s_mscan) {                                /* the first touch only stops a scan */
            mscan_stop();
            where = 5;
        } else if (s_adsb) {
            where = 3;
            adsb_view_touch(AV_PRESS, x, y);
        } else if (s_scan) {
            where = 4;
            scan_view_touch(SV_PRESS, x, y);
        } else if (y < view_tune_y()) {
            where = 0;
            act = view_button_at(x, y);
            if (act >= 0) { do_action(act); next_repeat = now + 450000; }
        } else if (s_panel) {
            where = 1;
            panel_event(PANEL_PRESS, x, y, now);
        } else {
            where = 2;
        }
        return;
    }
    x = (x + px) / 2;                                 /* move */
    y = (y + py) / 2;
    if (where == 3) adsb_view_touch(AV_MOVE, x, y);
    else if (where == 4) scan_view_touch(SV_MOVE, x, y);
    else if (where == 1) panel_event(PANEL_MOVE, x, y, now);
    else if (where == 0 && act >= 0 && act <= VIEW_UP1M && now >= next_repeat) {
        do_action(act);
        next_repeat = now + 150000;
    }
}

/* -------------------------------------------------------------------------- ADS-B -- */

/* free internal RAM, its low-water mark, and the largest internal DMA block, in KB: the numbers
 * the USB transfers and the C6 link compete for */
static void ram_kb(unsigned *free_kb, unsigned *min_kb, unsigned *dma_kb)
{
    *free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10;
    *min_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10;
    *dma_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10;
}

static double uptime_s(void) { return esp_timer_get_time() / 1e6; }

/* One row per aircraft heard since its last row, at most every 5 s each. */
enum { LOG_EVERY_MS = 5000, LOG_SLOTS = 2 * ADSB_MAX_AC };
typedef struct { uint32_t icao; int64_t at; } logged_t;
static logged_t s_logged[LOG_SLOTS];          /* last row per aircraft, adsb clock; cleared per entry */

static void log_aircraft(void)
{
    logged_t *done = s_logged;
    const int64_t now = adsb_now_ms();
    for (int i = 0; i < adsb_count(); i++) {
        const adsb_ac_t *a = adsb_get(i);
        int k = -1, old = 0;
        for (int j = 0; j < LOG_SLOTS; j++) {
            if (done[j].icao == a->icao && done[j].at) { k = j; break; }
            if (done[j].at < done[old].at) old = j;
        }
        if (k >= 0 && (a->seen_ms <= done[k].at || now - done[k].at < LOG_EVERY_MS)) continue;
        if (k < 0) { k = old; done[k].icao = a->icao; }
        done[k].at = now;
        char fl[24] = "", alt[12] = "", spd[12] = "", trk[12] = "", vr[12] = "", lat[16] = "", lon[16] = "";
        if (a->flight[0]) sdlog_str(fl, sizeof fl, a->flight);
        if (a->alt_ft != INT32_MIN) snprintf(alt, sizeof alt, "%d", a->alt_ft);
        if (a->speed_kt >= 0) {
            snprintf(spd, sizeof spd, "%d", a->speed_kt);
            snprintf(trk, sizeof trk, "%d", a->track_deg);
            snprintf(vr, sizeof vr, "%d", a->vrate_fpm);
        }
        if (a->has_pos) {
            snprintf(lat, sizeof lat, "%.5f", a->lat);
            snprintf(lon, sizeof lon, "%.5f", a->lon);
        }
        sdlog_row("%.1f,%06" PRIX32 ",%s,%s,%s,%s,%s,%s,%s,%" PRIu32, uptime_s(), a->icao, fl, alt, spd, trk,
                  vr, lat, lon, a->msgs);
    }
}

/* 1090 MHz at 2 MS/s, max gain, every sample through the decoder. Returns when EXIT is
 * tapped or the stream dies; the receiver settings are restored either way. */
static void adsb_mode(const touch_cal_t *cal)
{
    enum { BUF = 32 * 1024 };
    static uint8_t *buf;                       /* PSRAM: internal RAM is for USB DMA */
    if (!buf) buf = heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM);
    assert(buf);
    const uint32_t freq = sdr_freq();
    const int gain = sdr_gain_index();
    ESP_LOGI(TAG, "ADS-B mode");
    audio_pause(true);
    sdr_stop();
    memset(s_logged, 0, sizeof s_logged);   /* adsb_init restarts the decoder's clock */
    sdlog_begin("ADSB", "t_s,icao,callsign,alt_ft,speed_kt,track_deg,vrate_fpm,lat,lon,msgs");
    static uint8_t *map;                       /* MAP.BIN, kept in PSRAM once read */
    if (!map) {                                /* card read while the USB transfers are freed */
        size_t len;
        if (sdcard_read("MAP.BIN", &map, &len, 16 << 20)) {
            if (amap_load(map, len)) {
                const amap_info_t *m = amap_info();
                ESP_LOGI(TAG, "map \"%s\": %.4f %.4f, %d km, %d features", m->name, m->lat, m->lon,
                         m->radius_km, m->nfeat);
            } else {
                ESP_LOGW(TAG, "MAP.BIN is not a map");
                free(map);
                map = NULL;
            }
        }
    }
    sdr_set_sample_rate(ADSB_RATE);
    sdr_set_freq(1090000000);
    sdr_set_gain_index(sdr_gain_presets() - 1);
    sdr_start();
    adsb_init();
    adsb_view_open();
    s_adsb = true;
    s_av_action = AV_NONE;

    adsb_view_info_t info = {.gain_tenths = sdr_gain_tenths(sdr_gain_index()), .rec = sdlog_state()};
    if (amap_info()) {                         /* the map's centre is home */
        info.have_home = true;
        info.home_lat = amap_info()->lat;
        info.home_lon = amap_info()->lon;
    }
    uint64_t pos = sdr_total_bytes(), dropped = 0, ok_last = 0, pre_last = 0;
    int64_t t_draw = 0, t_touch = 0, t_yield = esp_timer_get_time(), t_log = t_yield, busy = 0;
    adsb_view_draw(&info);
    while (sdr_streaming() && s_av_action != AV_EXIT) {
        int64_t t0 = esp_timer_get_time();
        size_t n = sdr_ring_read(buf, &pos, BUF, &dropped);
        if (n) {
            adsb_process(buf, n / 2);
            busy += esp_timer_get_time() - t0;
        }
        int64_t now = esp_timer_get_time();
        if (n < BUF || now - t_yield > 50000) { vTaskDelay(1); t_yield = now; }
        if (now - t_touch > 20000) {
            t_touch = now;
            s_av_action = AV_NONE;
            poll_touch(cal);
            if (s_av_action == AV_TOGGLE || s_av_action == AV_ZOOM_IN || s_av_action == AV_ZOOM_OUT) t_draw = 0;
        }
        if (now - t_draw > 1000000) {
            t_draw = now;
            adsb_expire(60000);
            if (!info.have_home)                 /* centre the map on the first fix */
                for (int i = 0; i < adsb_count(); i++)
                    if (adsb_get(i)->has_pos) {
                        info.have_home = true;
                        info.home_lat = adsb_get(i)->lat;
                        info.home_lon = adsb_get(i)->lon;
                        break;
                    }
            log_aircraft();
            info.rec = sdlog_state();
            adsb_view_draw(&info);
        }
        if (now - t_log > 5000000) {
            adsb_stats_t st;
            adsb_get_stats(&st);
            double dt = (now - t_log) / 1e6;
            info.msgs_per_s = (st.msgs_ok - ok_last) / dt;
            unsigned fk, mk, dk;
            ram_kb(&fk, &mk, &dk);
            ESP_LOGI(TAG, "adsb: %.1f msg/s, %.0f preambles/s, ok %llu fixed %llu bad %llu, %d aircraft, "
                     "decoder load %.0f%%, dropped %llu B, ram %u KB (min %u, dma %u)", info.msgs_per_s,
                     (st.preambles - pre_last) / dt, (unsigned long long)st.msgs_ok, (unsigned long long)st.msgs_fixed,
                     (unsigned long long)st.msgs_bad, adsb_count(), 100.0 * busy / (now - t_log),
                     (unsigned long long)dropped, fk, mk, dk);
            ok_last = st.msgs_ok;
            pre_last = st.preambles;
            busy = 0;
            t_log = now;
        }
    }
    s_adsb = false;
    ESP_LOGI(TAG, "leaving ADS-B mode");
    sdr_stop();
    sdlog_end();
    sdr_set_sample_rate(SDR_SAMPLE_RATE);
    sdr_set_freq(freq);
    sdr_set_gain_index(gain);
    sdr_start();
    audio_pause(false);
    view_set_theme(view_theme());              /* clears the screen */
    apply_view();
    draw_header(-1);
    view_axis(sdr_freq());
}

/* ----------------------------------------------------------------- Wi-Fi / BLE -- */

static void mac_str(char *out, const uint8_t *m)
{
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Wi-Fi: every network heard in the sweep that just finished. BLE: every device heard in the
 * last 5 s. since_ms is the scanner clock (ms since boot) of the previous batch. */
static void log_scan(int kind, const scan_view_info_t *in, int64_t since_ms)
{
    char mac[18], a[72], b[40];
    if (kind == SV_WIFI)
        for (int i = 0; i < in->nap; i++) {
            const scan_ap_t *p = &in->ap[i];
            if (p->seen_ms <= since_ms) continue;
            mac_str(mac, p->bssid);
            sdlog_row("%.1f,%s,%s,%d,%d,%s", uptime_s(), mac, sdlog_str(a, sizeof a, p->ssid), p->ch, p->rssi, p->sec);
        }
    else
        for (int i = 0; i < in->nble; i++) {
            const scan_ble_t *p = &in->ble[i];
            if (p->seen_ms <= since_ms) continue;
            mac_str(mac, p->addr);
            sdlog_row("%.1f,%s,%s,%d,%s,%s,%d,%" PRIu32, uptime_s(), mac, p->random ? "random" : "public", p->rssi,
                      sdlog_str(a, sizeof a, p->name), sdlog_str(b, sizeof b, p->kind), p->connectable, p->count);
        }
}

/* A scanner on the module's ESP32-C6. The RTL-SDR stream is stopped first: that frees the
 * internal RAM of the USB transfers, which the Wi-Fi buffers need; the scanner frees its own
 * again on EXIT before the stream restarts. */
static void scan_mode(const touch_cal_t *cal, int kind)
{
    static scan_ap_t *ap;                      /* PSRAM snapshots for drawing */
    static scan_ble_t *ble;
    if (!ap) ap = heap_caps_calloc(SCAN_MAX_AP, sizeof *ap, MALLOC_CAP_SPIRAM);
    if (!ble) ble = heap_caps_calloc(SCAN_MAX_BLE, sizeof *ble, MALLOC_CAP_SPIRAM);
    assert(ap && ble);
    const char *name = kind == SV_WIFI ? "Wi-Fi" : "BLE";
    ESP_LOGI(TAG, "%s scanner", name);
    audio_pause(true);
    sdr_stop();
    scan_view_open(kind);
    s_scan = true;
    s_sv_action = SV_NONE;
    scan_view_info_t info = {.kind = kind, .ap = ap, .ble = ble,
                             .status = kind == SV_WIFI ? "starting Wi-Fi..." : "starting Bluetooth..."};
    scan_view_draw(&info);
    const char *why = NULL;
    bool ok = radio_scan_start(kind, &why);
    info.status = ok ? NULL : why;
    if (ok) sdlog_begin(kind == SV_WIFI ? "WIFI" : "BLE",
                        kind == SV_WIFI ? "t_s,bssid,ssid,channel,rssi_dbm,security"
                                        : "t_s,address,address_type,rssi_dbm,name,maker_or_service,connectable,adverts");
    info.rec = sdlog_state();
    int64_t t_draw = 0, t_log = esp_timer_get_time(), logged_ms = 0;
    int logged_sweeps = 0;
    while (s_sv_action != SV_EXIT) {
        int64_t now = esp_timer_get_time();
        if (ok) radio_scan_poll();
        s_sv_action = SV_NONE;
        poll_touch(cal);
        if (s_sv_action == SV_TOGGLE || s_sv_action == SV_MORE) t_draw = 0;
        if (now - t_draw > 1000000) {
            t_draw = now;
            info.now_ms = now / 1000;
            if (ok && kind == SV_WIFI) info.nap = radio_scan_get_ap(ap, SCAN_MAX_AP, &info.scans);
            if (ok && kind == SV_BLE) info.nble = radio_scan_get_ble(ble, SCAN_MAX_BLE);
            if (kind == SV_WIFI ? info.scans != logged_sweeps : info.now_ms - logged_ms >= 5000) {
                log_scan(kind, &info, logged_ms);
                logged_sweeps = info.scans;
                logged_ms = info.now_ms;
            }
            info.rec = sdlog_state();
            scan_view_draw(&info);
        }
        if (now - t_log > 10000000) {
            t_log = now;
            unsigned fk, mk, dk;
            ram_kb(&fk, &mk, &dk);
            if (kind == SV_WIFI) ESP_LOGI(TAG, "wifi: %d networks, %d sweeps, ram %u KB (min %u, dma %u)", info.nap,
                                          info.scans, fk, mk, dk);
            else ESP_LOGI(TAG, "ble: %d devices, ram %u KB (min %u, dma %u)", info.nble, fk, mk, dk);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    sdlog_end();
    if (ok) radio_scan_stop();
    s_scan = false;
    ESP_LOGI(TAG, "leaving the %s scanner", name);
    sdr_start();
    audio_pause(false);
    view_set_theme(view_theme());              /* clears the screen */
    apply_view();
    draw_header(-1);
    view_axis(sdr_freq());
}

/* ---------------------------------------------------------------------------- loop -- */

void ui_run(const touch_cal_t *cal)
{
    s_iq = heap_caps_malloc(IQ_PER_FRAME * 2, MALLOC_CAP_SPIRAM);
    assert(s_iq);
    spectrum_init();
    view_init(SDR_SAMPLE_RATE);
    panel_init();
    adsb_view_init();
    scan_view_init();
    nvs_open("sdr", NVS_READWRITE, &s_nvs);
    view_set_theme(nvs_int("theme", 0));
    s_ctl.range_auto = nvs_int("rauto", 1);
    s_ctl.ref_db = nvs_int("ref", -20);
    s_ctl.range_db = nvs_int("rspan", 60);
    s_ctl.zoom = nvs_int("zoom", 0) % PANEL_ZOOMS;
    s_ctl.bw = nvs_int("bw", 0) % PANEL_BWS;
    s_ctl.vol = nvs_int("vol", 4);
    s_ctl.sql = nvs_int("sql", 0);
    load_memories();
    apply_view();
    if (audio_init()) apply_audio();

    draw_header(-1);
    view_axis(sdr_freq());
    sdr_start();
    audio_pause(false);

    int frames = 0;
    int64_t t_log = esp_timer_get_time(), t_dsp = 0, t_lcd = 0;
    uint64_t b_log = sdr_total_bytes();
    for (;;) {
        if (s_adsb_req) {
            s_adsb_req = false;
            adsb_mode(cal);
        }
        if (s_scan_req >= 0) {
            int k = s_scan_req;
            s_scan_req = -1;
            scan_mode(cal, k);
        }
        if (!sdr_streaming()) {
            static int restarts;
            if (!rtlsdr_usb_take_gone() && restarts < 3) {
                /* Dongle still attached: restart the stream, don't reopen the device. */
                restarts++;
                ESP_LOGW(TAG, "stream stopped, restart %d", restarts);
                vTaskDelay(pdMS_TO_TICKS(500));
                sdr_start();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGW(TAG, "stream lost: reopening the dongle");
            s_panel = false;
            view_message("RTL-SDR not streaming", "unplug and replug it if this stays");
            sdr_close();
            vTaskDelay(pdMS_TO_TICKS(2000));
            while (!sdr_open(5000, NULL, 0)) vTaskDelay(pdMS_TO_TICKS(1000));
            restarts = 0;
            view_reset();
            draw_header(-1);
            view_axis(sdr_freq());
            sdr_start();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        int n = spectrum_size();
        sdr_ring_latest(s_iq, IQ_PER_FRAME * 2);
        spectrum_compute(s_iq, IQ_PER_FRAME / n, s_db);
        show_audio();
        view_render(s_db, n, sdr_freq());
        int64_t t1 = esp_timer_get_time();
        poll_touch(cal);
        if (!s_panel) view_blit_spectrum();    /* the waterfall keeps filling behind the panel */
        poll_touch(cal);
        if (!s_panel) view_blit_waterfall();
        int64_t t2 = esp_timer_get_time();
        t_dsp += t1 - t0;
        t_lcd += t2 - t1;
        poll_touch(cal);
        if (s_mscan) mscan_step();
        settings_maybe_save();
        vTaskDelay(1);                          /* let IDLE1 run (task watchdog) */
        frames++;
        if (t2 - t_log > 5000000) {
            int nf = frames;
            uint64_t b = sdr_total_bytes();
            rtlsdr_usb_stats_t us;
            rtlsdr_usb_get_stats(&us);
            unsigned fk, mk, dk;
            ram_kb(&fk, &mk, &dk);
            ESP_LOGI(TAG, "%.1f fps (dsp %.1f ms, lcd %.1f ms), fft %d, floor %.1f dBFS, IQ %.3f MB/s, "
                     "usb err %" PRIu32 " backlog max %" PRIu32 ", ram %u KB (min %u, dma %u)",
                     nf * 1e6 / (t2 - t_log), t_dsp / 1e3 / nf, t_lcd / 1e3 / nf, n, view_floor(),
                     (b - b_log) / ((t2 - t_log) / 1e6) / 1e6, us.bulk_errors, us.max_backlog, fk, mk, dk);
            if (audio_ok()) {
                int load, lag;
                audio_stats(&load, &lag);
                ESP_LOGI(TAG, "audio %s vol %d sql %d: demod %d%% of real time, lag max %d ms, channel %.1f dBFS "
                         "(floor %.1f), squelch %.1f dB, %s", panel_bw_mode(s_ctl.bw), s_ctl.vol, s_ctl.sql, load, lag,
                         demod_level_db(), demod_floor_db(), demod_quality_db(), audio_squelch_open() ? "open" : "shut");
                if (s_ctl.bw == 0) {
                    rds_info_t r;
                    rds_get(&r);
                    ESP_LOGI(TAG, "rds: %s, %" PRIu32 " blocks, %" PRIu32 " bad", r.sync ? "in sync" : "searching",
                             r.blocks, r.bad_blocks);
                }
            }
            b_log = b;
            frames = 0;
            t_dsp = t_lcd = 0;
            t_log = t2;
        }
    }
}
