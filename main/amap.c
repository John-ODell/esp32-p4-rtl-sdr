#include <stdlib.h>
#include <string.h>
#include "amap.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

#define HDR 40

typedef struct {
    const uint8_t *p;                        /* points (or the label position and text) */
    uint8_t kind, lo, hi;
    uint16_t n;
    int16_t x0, y0, x1, y1;                  /* bounding box, units */
} feat_t;

static amap_info_t s_info;
static bool s_ok;
static feat_t *s_f;
static int s_nf;
static double s_unit_km;

static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int16_t i16(const uint8_t *p) { return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool amap_load(const uint8_t *buf, size_t len)
{
    s_ok = false;
    free(s_f);
    s_f = NULL;
    s_nf = 0;
    if (len < HDR || memcmp(buf, "AMAP", 4) || u16(buf + 4) != 1) return false;
    int unit = u16(buf + 6);
    uint32_t nf = u32(buf + 16);
    if (!unit || nf > 1000000) return false;
    s_f = BIG_ALLOC((nf ? nf : 1) * sizeof *s_f);
    if (!s_f) return false;
    const uint8_t *p = buf + HDR, *end = buf + len;
    for (uint32_t i = 0; i < nf; i++) {
        if (end - p < 6) return false;
        feat_t *f = &s_f[s_nf];
        f->kind = p[0];
        f->lo = p[1];
        f->hi = p[2];
        f->n = u16(p + 4);
        p += 6;
        size_t need = f->kind >= AMAP_CITY ? 4u + f->n : 4u * f->n;
        if ((size_t)(end - p) < need) return false;
        f->p = p;
        int np = f->kind >= AMAP_CITY ? 1 : f->n;
        f->x0 = f->y0 = INT16_MAX;
        f->x1 = f->y1 = INT16_MIN;
        for (int k = 0; k < np; k++) {
            int16_t x = i16(p + 4 * k), y = i16(p + 4 * k + 2);
            if (x < f->x0) f->x0 = x;
            if (x > f->x1) f->x1 = x;
            if (y < f->y0) f->y0 = y;
            if (y > f->y1) f->y1 = y;
        }
        p += need;
        if (f->n) s_nf++;
    }
    s_info.lat = (int32_t)u32(buf + 8) / 1e6;
    s_info.lon = (int32_t)u32(buf + 12) / 1e6;
    s_info.radius_km = (int)u32(buf + 20);
    s_info.nfeat = s_nf;
    memcpy(s_info.name, buf + 24, 16);
    s_info.name[16] = 0;
    s_unit_km = unit / 1000.0;
    s_ok = true;
    return true;
}

const amap_info_t *amap_info(void) { return s_ok ? &s_info : NULL; }

/* ------------------------------------------------------------------------- drawing -- */

typedef struct { const rpi_canvas_t *c; int ytop; } clip_t;

static inline void dot(const clip_t *k, int x, int y, uint16_t col)
{
    if (x >= 0 && x < k->c->w && y >= k->ytop && y < k->c->h) k->c->px[y * k->c->w + x] = col;
}

/* Bresenham; dash > 0 draws dash px on, dash px off; wide doubles the line */
static void seg(const clip_t *k, int x0, int y0, int x1, int y1, uint16_t col, int dash, bool wide, int *phase)
{
    if ((x0 < 0 && x1 < 0) || (y0 < k->ytop && y1 < k->ytop) || (x0 >= k->c->w && x1 >= k->c->w) ||
        (y0 >= k->c->h && y1 >= k->c->h)) return;
    if (abs(x1 - x0) > 4 * k->c->w || abs(y1 - y0) > 4 * k->c->h) return;   /* far off screen */
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    bool steep = -dy > dx;
    for (;;) {
        if (!dash || ((*phase / dash) & 1) == 0) {
            dot(k, x0, y0, col);
            if (wide) dot(k, x0 + steep, y0 + !steep, col);
        }
        (*phase)++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

typedef struct { int x0, y0, x1, y1; } rect_t;
#define MAXLBL 48

static bool free_spot(const rect_t *r, const rect_t *used, int n)
{
    for (int i = 0; i < n; i++)
        if (r->x0 < used[i].x1 && used[i].x0 < r->x1 && r->y0 < used[i].y1 && used[i].y0 < r->y1) return false;
    return true;
}

void amap_draw(const rpi_canvas_t *c, int ytop, int cx, int cy, double px_per_km, int range_km,
               const amap_colors_t *col)
{
    if (!s_ok) return;
    const clip_t k = {c, ytop};
    const double s = px_per_km * s_unit_km;  /* px per unit */
    /* screen box in units, for culling */
    const double ux0 = (0 - cx) / s, ux1 = (c->w - cx) / s, uy0 = (cy - c->h) / s, uy1 = (cy - ytop) / s;
    static const uint8_t order[] = {AMAP_WATER, AMAP_RIVER, AMAP_BORDER, AMAP_ROAD, AMAP_MOTORWAY,
                                    AMAP_COAST, AMAP_RUNWAY};
    for (size_t o = 0; o < sizeof order; o++) {
        int kind = order[o];
        uint16_t cl = kind == AMAP_COAST ? col->coast : kind == AMAP_WATER ? col->water :
                      kind == AMAP_RIVER ? col->river : kind == AMAP_BORDER ? col->border :
                      kind == AMAP_ROAD ? col->road : kind == AMAP_MOTORWAY ? col->motorway : col->runway;
        bool wide = (kind == AMAP_RUNWAY && range_km <= 25) || (kind == AMAP_COAST && range_km <= 50) ||
                    (kind == AMAP_MOTORWAY && range_km <= 25);
        int dash = kind == AMAP_BORDER ? 3 : 0;
        for (int i = 0; i < s_nf; i++) {
            const feat_t *f = &s_f[i];
            if (f->kind != kind || range_km > 5 * f->hi || range_km < 5 * f->lo) continue;
            if (f->x1 < ux0 || f->x0 > ux1 || f->y1 < uy0 || f->y0 > uy1) continue;
            int phase = 0;
            int px = cx + (int)(i16(f->p) * s), py = cy - (int)(i16(f->p + 2) * s);
            if (f->n == 1) { dot(&k, px, py, cl); continue; }
            for (int j = 1; j < f->n; j++) {
                int x = cx + (int)(i16(f->p + 4 * j) * s), y = cy - (int)(i16(f->p + 4 * j + 2) * s);
                if (x != px || y != py) seg(&k, px, py, x, y, cl, dash, wide, &phase);
                px = x;
                py = y;
            }
        }
    }
    /* names: cities, then airports, then towns; file order is most important first. Each tries
     * right of its point, then left, above and below, and is left out if all four are taken. */
    rect_t used[MAXLBL];
    int nu = 0;
    static const uint8_t lorder[] = {AMAP_CITY, AMAP_AIRPORT, AMAP_TOWN};
    for (size_t o = 0; o < sizeof lorder && nu < MAXLBL; o++)
        for (int i = 0; i < s_nf && nu < MAXLBL; i++) {
            const feat_t *f = &s_f[i];
            if (f->kind != lorder[o] || range_km > 5 * f->hi || range_km < 5 * f->lo) continue;
            int x = cx + (int)(f->x0 * s), y = cy - (int)(f->y0 * s);
            char t[24];
            int n = f->n < (int)sizeof t - 1 ? f->n : (int)sizeof t - 1;
            memcpy(t, f->p + 4, n);
            t[n] = 0;
            int tw = rpi_cv_text_w(t, 1);
            bool apt = f->kind == AMAP_AIRPORT;
            const rect_t spots[4] = {{x + 4, y - 8, x + 6 + tw, y + 8}, {x - 6 - tw, y - 8, x - 4, y + 8},
                                     {x - tw / 2 - 1, y - 20, x + tw / 2 + 1, y - 4},
                                     {x - tw / 2 - 1, y + 4, x + tw / 2 + 1, y + 20}};
            rect_t r;
            int sp = 0;
            for (; sp < 4; sp++) {
                r = spots[sp];
                if (r.x0 >= 0 && r.x1 <= c->w && r.y0 >= ytop + 2 && r.y1 <= c->h - 18 && free_spot(&r, used, nu)) break;
            }
            if (sp == 4) continue;                  /* (the bottom line is the map's name) */
            used[nu++] = r;
            uint16_t lc = apt ? col->airport : col->place;
            if (apt) {
                for (int d = -2; d <= 2; d++) { dot(&k, x + d, y, lc); dot(&k, x, y + d, lc); }
            } else {
                rpi_cv_fill(c, x - 1, y - 1, 3, 3, lc);
            }
            rpi_cv_text(c, r.x0 + 1, r.y0, t, lc, 1);
        }
}
