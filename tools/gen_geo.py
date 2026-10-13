#!/usr/bin/env python3
"""Génère src/core/geo_data.h : tracé du tram T1 (GTFS ATP, CC BY 4.0) repris
de VelohNav (src/data/tramT1.json, polyligne encodée), simplifié pour la carte.

Usage : python3 tools/gen_geo.py <velohnav/src/data/tramT1.json> <sortie.h>
"""
import json, math, sys


def decode_polyline(s, precision=5):
    coords, idx, lat, lng = [], 0, 0, 0
    f = 10 ** precision
    while idx < len(s):
        for which in (0, 1):
            shift = result = 0
            while True:
                b = ord(s[idx]) - 63
                idx += 1
                result |= (b & 0x1F) << shift
                shift += 5
                if b < 0x20:
                    break
            d = ~(result >> 1) if result & 1 else result >> 1
            if which == 0:
                lat += d
            else:
                lng += d
        coords.append((lat / f, lng / f))
    return coords


def dp(points, eps_m):
    """Douglas-Peucker en mètres (projection locale)."""
    if len(points) < 3:
        return points
    lat0 = math.radians(points[0][0])
    def xy(p):
        return (p[1] * 111320 * math.cos(lat0), p[0] * 110540)
    P = [xy(p) for p in points]
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = P[a]; bx, by = P[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy) or 1e-9
        best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = P[i]
            d = abs(dy * px - dx * py + bx * ay - by * ax) / L
            if d > best:
                best, bi = d, i
        if best > eps_m:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(points, keep) if k]


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = json.load(open(src, encoding="utf-8"))
    pts = dp(decode_polyline(d["geometry"]), 6.0)
    stops = d.get("stops", [])
    lines = [
        "// Généré par tools/gen_geo.py — ne pas éditer à la main.",
        "// Tram T1 (tracé complet, arrêts dans l’ordre) — GTFS ATP, data.public.lu, licence CC BY 4.0.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"static const int32_t VN_TRAM_T1[{len(pts)}][2] = {{  // lat, lon ×1e5",
    ]
    for i in range(0, len(pts), 4):
        lines.append("  " + " ".join(f"{{{round(a*1e5)},{round(b*1e5)}}}," for a, b in pts[i:i+4]))
    lines.append("};")
    lines.append(f"static const int VN_TRAM_T1_N = {len(pts)};")
    lines.append("")
    lines.append(f"static const int32_t VN_TRAM_STOPS[{len(stops)}][2] = {{")
    for s in stops:
        lines.append(f"  {{{round(s['lat']*1e5)},{round(s['lng']*1e5)}}},  // {s.get('name','')}")
    lines.append("};")
    lines.append(f"static const int VN_TRAM_STOPS_N = {len(stops)};")
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print(f"T1 : {len(pts)} points, {len(stops)} arrêts → {out}")


if __name__ == "__main__":
    main()
