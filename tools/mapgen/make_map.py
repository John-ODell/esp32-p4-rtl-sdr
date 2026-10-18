#!/usr/bin/env python3
"""Make MAP.BIN, the ADS-B screen's base map.

    python3 make_map.py --place "Leeds, UK" --out /Volumes/SDCARD/MAP.BIN
    python3 make_map.py --lat 52.31 --lon 4.76 --name SCHIPHOL --radius 300

The map's centre is the home point on the kit, so pick a place near the antenna.

Base layers come from Natural Earth 1:10m (public domain): coastlines, lakes, rivers, country
and state borders, highways, cities and airports. They are downloaded once (about 27 MB)
and kept in ~/.cache/p4-sdr-mapgen. Close in, OpenStreetMap adds detail through the Overpass
API: the exact coastline, lakes, rivers, roads, runways and towns (map data (c) OpenStreetMap
contributors, ODbL). Overpass is often busy; if a query fails the map is made without that
layer and a rerun later fills it in (answers are cached). --no-osm skips it.

Writes the flat km-grid format main/amap.h describes. Standard library only."""
import argparse, hashlib, io, json, math, os, struct, sys, time, urllib.parse, urllib.request, zipfile

UA = "p4-sdr-mapgen/1.0 (ESP32-P4 ADS-B screen; personal use)"
CACHE = os.path.expanduser("~/.cache/p4-sdr-mapgen")
NE_URL = "https://naciscdn.org/naturalearth/10m/{}.zip"
OVERPASS = ["https://overpass-api.de/api/interpreter",
            "https://overpass.private.coffee/api/interpreter",
            "https://maps.mail.ru/osm/tools/overpass/api/interpreter"]
UNIT_M = 25                                  # i16 grid: +-819 km

COAST, WATER, RIVER, BORDER, ROAD, MOTORWAY, RUNWAY = 1, 2, 3, 4, 5, 6, 7
CITY, TOWN, AIRPORT = 16, 17, 18
# zoom bands, in 5 km steps of the screen's range (5, 10, 25, 50, 100, 200, 400 km)
ALL, FAR, MID, NEAR = (0, 80), (20, 80), (10, 80), (0, 10)

def get(url, data=None, timeout=300):
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers={"User-Agent": UA}),
                                timeout=timeout) as r:
        return r.read()

# ------------------------------------------------------------------ Natural Earth --

def ne_zip(name):
    path = os.path.join(CACHE, "ne", os.path.basename(name) + ".zip")
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"  downloading {os.path.basename(name)}")
        data = get(NE_URL.format(name))
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return zipfile.ZipFile(path)

def ne_read(name, bbox):
    """Records [(parts, attrs)] whose bounds meet bbox (s, w, n, e). parts: [[(lon, lat)...]]."""
    z = ne_zip(name)
    base = os.path.basename(name)
    shp = z.read(next(n for n in z.namelist() if n.lower() == base + ".shp"))
    dbf = z.read(next(n for n in z.namelist() if n.lower() == base + ".dbf"))
    # dbf
    nrec, hlen, rlen = struct.unpack("<IHH", dbf[4:12])
    fields, p = [], 32
    while dbf[p] != 0x0D:
        fields.append((dbf[p:p + 11].split(b"\0")[0].decode().lower(), dbf[p + 16]))
        p += 32
    def attrs(i):
        r, o, d = dbf[hlen + i * rlen:hlen + (i + 1) * rlen], 1, {}
        for fname, flen in fields:
            d[fname] = r[o:o + flen].decode("utf-8", "replace").strip()
            o += flen
        return d
    s, w, n, e = bbox
    out, pos, i = [], 100, 0
    while pos + 8 <= len(shp):
        clen = struct.unpack(">i", shp[pos + 4:pos + 8])[0] * 2
        rec = shp[pos + 8:pos + 8 + clen]
        pos += 8 + clen
        idx, i = i, i + 1
        st = struct.unpack("<i", rec[:4])[0]
        if st in (1, 11, 21):
            x, y = struct.unpack("<dd", rec[4:20])
            if s <= y <= n and w <= x <= e:
                out.append(([[(x, y)]], attrs(idx)))
        elif st in (3, 5, 13, 15, 23, 25):
            x0, y0, x1, y1 = struct.unpack("<4d", rec[4:36])
            if x1 < w or x0 > e or y1 < s or y0 > n:
                continue
            nparts, npts = struct.unpack("<ii", rec[36:44])
            parts = list(struct.unpack(f"<{nparts}i", rec[44:44 + 4 * nparts])) + [npts]
            off = 44 + 4 * nparts
            xy = struct.unpack(f"<{2 * npts}d", rec[off:off + 16 * npts])
            pts = list(zip(xy[0::2], xy[1::2]))
            out.append(([pts[parts[k]:parts[k + 1]] for k in range(nparts)], attrs(idx)))
    return out

# ------------------------------------------------------------------- OpenStreetMap --

class Busy(Exception):
    pass

def overpass(body, bbox, what, offline=False):
    s, w, n, e = (math.floor(bbox[0] * 50) / 50, math.floor(bbox[1] * 50) / 50,   # out to 0.02 deg,
                  math.ceil(bbox[2] * 50) / 50, math.ceil(bbox[3] * 50) / 50)      # so answers are reused
    q = f"[out:json][timeout:120][bbox:{s:.2f},{w:.2f},{n:.2f},{e:.2f}];{body}"
    path = os.path.join(CACHE, "osm", hashlib.sha1(q.encode()).hexdigest() + ".json")
    if os.path.exists(path):
        with open(path) as f:
            d = json.load(f)
        print(f"  OSM {what}: {len(d['elements'])} elements (cached)")
        return d["elements"]
    if offline:
        raise Busy("not asked")
    t, err = time.time(), None
    for url in OVERPASS:
        try:
            raw = get(url, urllib.parse.urlencode({"data": q}).encode(), timeout=90)
            d = json.loads(raw)
            if "error" in d.get("remark", "").lower():
                raise RuntimeError(d["remark"][:100])
            break
        except Exception as ex:
            err = f"{urllib.parse.urlparse(url).netloc}: {ex}"
    else:
        raise Busy(err)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(raw)
    print(f"  OSM {what}: {len(d['elements'])} elements, {time.time() - t:.0f} s")
    return d["elements"]

def geocode(place):
    q = urllib.parse.urlencode({"q": place, "format": "json", "limit": 1})
    path = os.path.join(CACHE, "geo", hashlib.sha1(q.encode()).hexdigest() + ".json")
    if os.path.exists(path):                 # the same answer every run, so OSM answers stay cached
        with open(path) as f:
            res = json.load(f)
    else:
        res = json.loads(get("https://nominatim.openstreetmap.org/search?" + q))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(res, f)
    if not res:
        sys.exit(f"can't find {place!r}")
    print(f"{place}: {res[0]['display_name']}")
    return float(res[0]["lat"]), float(res[0]["lon"])

# --------------------------------------------------------------------- geometry --

def bbox_for(lat, lon, r_km):
    dlat = r_km / 110.57
    dlon = r_km / (111.32 * math.cos(math.radians(lat)))
    return (lat - dlat, lon - dlon, lat + dlat, lon + dlon)

def simplify(pts, tol):
    """Douglas-Peucker, iterative."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        (ax, ay), (bx, by) = pts[a], pts[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy) or 1e-9
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            d = abs(dy * (pts[i][0] - ax) - dx * (pts[i][1] - ay)) / L
            if d > best:
                best, bi = d, i
        if best > tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]

class Map:
    def __init__(self, lat, lon):
        self.lat, self.lon = lat, lon
        self.kx, self.ky = 111.32 * math.cos(math.radians(lat)), 110.57
        self.lines, self.labels = [], []

    def xy(self, lon, lat):                  # degrees -> km east/north of the centre
        return ((lon - self.lon) * self.kx, (lat - self.lat) * self.ky)

    def line(self, kind, band, lonlat, tol_km, lim):
        """Clip to the square of half-size lim km (one point beyond either edge kept), simplify, add."""
        pts = [self.xy(x, y) for x, y in lonlat]
        ins = [abs(x) <= lim and abs(y) <= lim for x, y in pts]
        runs, cur = [], []
        for i, p in enumerate(pts):
            near = ins[i] or (i + 1 < len(pts) and ins[i + 1]) or (i > 0 and ins[i - 1])
            if near:
                cur.append(p)
            elif cur:
                runs.append(cur); cur = []
        if cur:
            runs.append(cur)
        for run in runs:
            q = []
            for x, y in simplify(run, tol_km):
                p = (round(x * 1000 / UNIT_M), round(y * 1000 / UNIT_M))
                if max(abs(p[0]), abs(p[1])) < 32767 and (not q or q[-1] != p):
                    q.append(p)
            for k in range(0, len(q) - 1, 65000):
                part = q[k:k + 65001]
                if len(part) >= 2:
                    self.lines.append((kind, band, part))

    def label(self, kind, band, lon, lat, text, rank):
        x, y = self.xy(lon, lat)
        self.labels.append((kind, band, (x, y), text, rank))

    def write(self, path, radius, name):
        self.labels.sort(key=lambda l: -l[4])  # most important first: the screen places them in order
        out = bytearray(struct.pack("<4sHHiiII16s", b"AMAP", 1, UNIT_M, round(self.lat * 1e6),
                                    round(self.lon * 1e6), len(self.lines) + len(self.labels), radius,
                                    name.encode("ascii", "replace")))
        npts = 0
        for kind, (lo, hi), q in self.lines:
            out += struct.pack("<BBBBH", kind, lo, hi, 0, len(q))
            out += b"".join(struct.pack("<hh", x, y) for x, y in q)
            npts += len(q)
        for kind, (lo, hi), (x, y), text, _ in self.labels:
            b = text.encode("ascii", "replace")[:20]
            out += struct.pack("<BBBBHhh", kind, lo, hi, 0, len(b), round(x * 1000 / UNIT_M),
                               round(y * 1000 / UNIT_M)) + b
        with open(path, "wb") as f:
            f.write(out)
        print(f"{path}: {len(self.lines)} lines ({npts} points), {len(self.labels)} names, "
              f"{len(out) / 1024:.0f} KB")

# ------------------------------------------------------------------------- main --

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--place", help="town, airport or address to centre on (looked up on Nominatim)")
    ap.add_argument("--lat", type=float)
    ap.add_argument("--lon", type=float)
    ap.add_argument("--radius", type=int, default=400, help="km around the centre (default 400, max 800)")
    ap.add_argument("--name", help="shown on the map (15 chars; default: first part of --place)")
    ap.add_argument("--no-osm", action="store_true", help="Natural Earth only, no Overpass queries")
    ap.add_argument("--out", default="MAP.BIN")
    a = ap.parse_args()
    if a.place:
        lat, lon = geocode(a.place)
    elif a.lat is not None and a.lon is not None:
        lat, lon = a.lat, a.lon
    else:
        ap.error("give --place or --lat and --lon")
    R = max(25, min(a.radius, 800))
    name = (a.name or (a.place or f"{lat:.3f} {lon:.3f}").split(",")[0]).upper()[:15]
    m = Map(lat, lon)
    bb = lambda r: bbox_for(lat, lon, min(R, r) * 1.1)
    lim = lambda r: min(R, r) * 1.05
    print(f"centre {lat:.5f} {lon:.5f}, radius {R} km -> {a.out}")

    # OpenStreetMap detail first: what it covers decides where the coarse layers start
    have = set()
    osm_names, osm_codes = [], {}
    # most useful first; after two busy answers in a row, don't wait on the rest
    busy, fails = [a.no_osm], [0]
    def osm(what, body, r):
        try:
            els = overpass(body, bb(r), what, offline=busy[0])   # cached answers are still used
            have.add(what)
            fails[0] = 0
            return els
        except Busy as e:
            if not busy[0]:
                print(f"  OSM {what}: Overpass busy ({e})", file=sys.stderr)
                fails[0] += 1
                if fails[0] == 2:
                    print("  skipping the other OSM layers", file=sys.stderr)
                    busy[0] = True
            return []
    def ways(els, kind, band, tol, r):
        for e in els:
            if e["type"] == "way" and "geometry" in e:
                m.line(kind, band, [(g["lon"], g["lat"]) for g in e["geometry"] if g], tol, lim(r))
    ways(osm("runways", 'way["aeroway"="runway"];out geom;', 150), RUNWAY, (0, 20), 0.01, 150)
    for e in osm("towns", 'node["place"~"^(city|town)$"];out;', 80):
        t = e.get("tags", {})
        nm = t.get("name:en") or t.get("name")
        if not nm:
            continue
        try:
            pop = int(str(t.get("population", "0")).replace(",", "").replace(" ", "").split(";")[0] or 0)
        except ValueError:
            pop = 0
        city = t["place"] == "city"
        osm_names.append((nm.lower(), len(m.labels)))
        m.label(CITY if city else TOWN, (0, 40 if city else 10 if pop >= 20000 else 5), e["lon"], e["lat"], nm, pop)
    for e in osm("airfields", '(node["aeroway"="aerodrome"]["icao"];way["aeroway"="aerodrome"]["icao"];);out center;', R):
        t = e.get("tags", {})
        c = e if "lat" in e else e.get("center")
        if c and t["icao"].upper() not in osm_codes:     # an airfield can be both a node and a way
            osm_codes[t["icao"].upper()] = len(m.labels)
            m.label(AIRPORT, (0, 20 if "iata" in t else 10), c["lon"], c["lat"], t["icao"].upper(), 2 if "iata" in t else 1)

    ways(osm("coastline", 'way["natural"="coastline"];out geom;', 100), COAST, NEAR, 0.04, 100)
    ways(osm("lakes", '(way["natural"="water"]["water"~"^(lake|reservoir)$"];'
             'rel["natural"="water"]["water"~"^(lake|reservoir)$"];>;);way._;out geom;', 60),
         WATER, (0, 5), 0.03, 60)
    ways(osm("rivers", 'way["waterway"="river"];out geom;', 60), RIVER, (0, 5), 0.04, 60)
    ways(osm("motorways", 'way["highway"="motorway"];out geom;', 100), MOTORWAY, NEAR, 0.04, 100)
    ways(osm("main roads", 'way["highway"~"^(trunk|primary)$"];out geom;', 50), ROAD, (0, 5), 0.03, 50)

    # Natural Earth everywhere, handing over to OSM where OSM has the layer
    box, tol = bb(R), 0.15
    hand = lambda what: FAR if what in have else ALL
    for parts, _ in ne_read("physical/ne_10m_coastline", box):
        for p in parts:
            m.line(COAST, hand("coastline"), p, tol, lim(R))
    for parts, _ in ne_read("physical/ne_10m_lakes", box):
        for p in parts:
            m.line(WATER, (10, 80) if "lakes" in have else ALL, p, tol, lim(R))
    for parts, _ in ne_read("physical/ne_10m_rivers_lake_centerlines", box):
        for p in parts:
            m.line(RIVER, (10, 40) if "rivers" in have else (0, 40), p, tol, lim(R))
    for parts, _ in ne_read("cultural/ne_10m_admin_0_boundary_lines_land", box):
        for p in parts:
            m.line(BORDER, ALL, p, tol, lim(R))
    for parts, _ in ne_read("cultural/ne_10m_admin_1_states_provinces_lines", box):
        for p in parts:
            m.line(BORDER, (0, 40), p, tol, lim(R))
    for parts, at in ne_read("cultural/ne_10m_roads", box):
        ty = at.get("type", "")
        if ty == "Major Highway" or at.get("expressway") == "1":
            band, kind = (FAR if "motorways" in have else ALL), MOTORWAY
        elif ty == "Secondary Highway":
            band, kind = ((10, 20) if "main roads" in have else (0, 20)), ROAD
        else:
            continue
        for p in parts:
            m.line(kind, band, p, tol, lim(R))
    for parts, at in ne_read("cultural/ne_10m_populated_places", box):
        (x0, y0), = parts[0]
        nm = at.get("nameascii") or at.get("name")
        try:
            pop = int(float(at.get("pop_max") or 0))
        except ValueError:
            pop = 0
        x, y = m.xy(x0, y0)
        if not nm or max(abs(x), abs(y)) > lim(R):
            continue
        hi = 80 if pop >= 1000000 else 40 if pop >= 250000 else 20 if pop >= 100000 else 10 if pop >= 20000 else 5
        kind = CITY if pop >= 100000 else TOWN
        same = [i for n, i in osm_names if n == nm.lower() and math.hypot(x - m.labels[i][2][0], y - m.labels[i][2][1]) < 8]
        if same:                              # OSM has the exact spot; Natural Earth knows how big it is
            k, (lo, ohi), xy, text, opop = m.labels[same[0]]
            if hi > ohi:
                m.labels[same[0]] = (min(k, kind), (lo, hi), xy, text, max(opop, pop))
            continue
        m.label(kind, (0, hi), x0, y0, nm, pop)
    for parts, at in ne_read("cultural/ne_10m_airports", box):
        (x0, y0), = parts[0]
        code = (at.get("gps_code") or at.get("abbrev") or "").upper()
        x, y = m.xy(x0, y0)
        if not code or max(abs(x), abs(y)) > lim(R):
            continue
        ty = at.get("type", "")
        hi, rank = (80 if "major" in ty else 20 if "mid" in ty else 10), (3 if "major" in ty else 2)
        if code in osm_codes:                 # OSM has the spot; Natural Earth knows how big it is
            i = osm_codes[code]
            k, (lo, ohi), xy, text, orank = m.labels[i]
            m.labels[i] = (k, (lo, max(hi, ohi)), xy, text, max(rank, orank))
            continue
        m.label(AIRPORT, (0, hi), x0, y0, code, rank)
    m.write(a.out, R, name)
    if not a.no_osm and len(have) < 8:
        print(f"OSM layers missing: {', '.join(sorted({'coastline','lakes','rivers','motorways','main roads','runways','towns','airfields'} - have))}"
              " (Overpass busy); the map works without them, rerun later to add them")

if __name__ == "__main__":
    main()
