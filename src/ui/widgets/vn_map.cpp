#include "vn_map.h"
#include "../theme.h"
#include "../../core/model.h"
#include "../../core/settings.h"
#include "../../core/geo_data.h"
#include "../../platform/platform.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <new>

using vn::g_model;

namespace ui {

static const float LAT0 = 49.608f;                 // référence de projection
static const float COSLAT = 0.6475f;               // cos(49,6°)
static const float M_PER_DEG_LAT = 111200.f;

struct MapState {
  float cLat = 49.6085f, cLon = 6.1330f;          // centre
  float scale = 5600.f;                            // px par degré de latitude
  int   layer = 0;
  MapFilter filter = MF_ALL;
  int   sel = -1;
  int   moved = 0;
  uint32_t lastTapMs = 0;
  lv_point_t lastTap = { 0, 0 };
  MapTapCb cb = nullptr;
};

struct Place { const char * name; float lat, lon; uint8_t minor; };
// Repères de quartiers (positions approximatives, pour l'orientation)
static const Place PLACES[] = {
  { "CENTRE",      49.6117f, 6.1300f, 0 }, { "GARE",        49.6003f, 6.1340f, 0 },
  { "KIRCHBERG",   49.6290f, 6.1640f, 0 }, { "LIMPERTSBERG", 49.6208f, 6.1222f, 0 },
  { "BONNEVOIE",   49.5955f, 6.1430f, 0 }, { "BELAIR",      49.6112f, 6.1135f, 0 },
  { "HOLLERICH",   49.5983f, 6.1180f, 0 }, { "GASPERICH",   49.5868f, 6.1262f, 0 },
  { "CLOCHE D'OR", 49.5768f, 6.1150f, 1 }, { "CENTS",       49.6165f, 6.1660f, 1 },
  { "HAMM",        49.6080f, 6.1790f, 1 }, { "MERL",        49.6025f, 6.1010f, 1 },
  { "CESSANGE",    49.5880f, 6.1010f, 1 }, { "STRASSEN",    49.6200f, 6.0720f, 1 },
  { "BERTRANGE",   49.6105f, 6.0490f, 1 }, { "HESPERANGE",  49.5715f, 6.1585f, 1 },
  { "HOWALD",      49.5815f, 6.1440f, 1 }, { "MAMER",       49.6275f, 6.0235f, 1 },
  { "WALFERDANGE", 49.6575f, 6.1330f, 1 }, { "EICH",        49.6315f, 6.1340f, 1 },
};

static MapState * st(lv_obj_t * m) { return (MapState *)lv_obj_get_user_data(m); }

uint8_t statusAt(int i, int k) {
  const vn::Station & s = g_model.st[i];
  if (k <= 0 || s.status == vn::ST_CLOSED || !s.live) return s.status;
  if (k >= VN_FC_STEPS) k = VN_FC_STEPS - 1;
  float capE = s.capEff();
  int b = (int)lroundf(s.fc[k]);
  int d = (int)lroundf(capE - s.fc[k]);
  if (s.renting && b <= 0) return vn::ST_EMPTY;
  if (s.returning && d <= 0) return vn::ST_FULL;
  if (b <= vn::g_set.lowBikes) return vn::ST_LOW;
  if (d <= vn::g_set.lowDocks) return vn::ST_HIGH;
  return vn::ST_OK;
}

bool filterMatch(uint8_t s, MapFilter f) {
  switch (f) {
    case MF_EMPTY: return s == vn::ST_EMPTY;
    case MF_FULL: return s == vn::ST_FULL;
    case MF_TENSION: return s == vn::ST_LOW || s == vn::ST_HIGH;
    default: return true;
  }
}

struct Proj {
  float cx, cy, cLat, cLon, k;
  float x(float lon) const { return cx + (lon - cLon) * COSLAT * k; }
  float y(float lat) const { return cy - (lat - cLat) * k; }
};

static void drawText(lv_draw_ctx_t * ctx, const char * t, const lv_font_t * f, lv_color_t c, lv_opa_t opa,
                     lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_text_align_t al, lv_coord_t ls = 0) {
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.font = f; ld.color = c; ld.opa = opa; ld.align = al; ld.letter_space = ls;
  lv_area_t a = { x1, y1, x2, (lv_coord_t)(y1 + lv_font_get_line_height(f)) };
  lv_draw_label(ctx, &ld, &a, t, nullptr);
}

static bool overlaps(const lv_area_t & a, const lv_area_t * list, int n) {
  for (int i = 0; i < n; i++)
    if (a.x1 <= list[i].x2 && a.x2 >= list[i].x1 && a.y1 <= list[i].y2 && a.y2 >= list[i].y1) return true;
  return false;
}

static void drawEvent(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  MapState * m = st(obj);
  lv_draw_ctx_t * ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  Proj P{ (a.x1 + a.x2) / 2.f, (a.y1 + a.y2) / 2.f, m->cLat, m->cLon, m->scale };
  float pxPerKm = m->scale / M_PER_DEG_LAT * 1000.f;

  // ── Quadrillage métrique (500 m / 1 km / 2 km)
  float stepKm = pxPerKm >= 90 ? 0.5f : pxPerKm >= 36 ? 1.f : 2.f;
  float stepLat = stepKm * 1000.f / M_PER_DEG_LAT, stepLon = stepLat / COSLAT;
  lv_draw_line_dsc_t gl;
  lv_draw_line_dsc_init(&gl);
  gl.color = lv_color_white();
  gl.opa = 9;
  gl.width = 1;
  float lonL = m->cLon - (P.cx - a.x1) / (COSLAT * m->scale), lonR = m->cLon + (a.x2 - P.cx) / (COSLAT * m->scale);
  float latT = m->cLat + (P.cy - a.y1) / m->scale, latB = m->cLat - (a.y2 - P.cy) / m->scale;
  for (float lo = floorf((lonL - 6.13f) / stepLon) * stepLon + 6.13f; lo <= lonR; lo += stepLon) {
    lv_coord_t x = (lv_coord_t)lroundf(P.x(lo));
    lv_point_t p1 = { x, a.y1 }, p2 = { x, a.y2 };
    lv_draw_line(ctx, &gl, &p1, &p2);
  }
  for (float la = floorf((latB - LAT0) / stepLat) * stepLat + LAT0; la <= latT; la += stepLat) {
    lv_coord_t y = (lv_coord_t)lroundf(P.y(la));
    lv_point_t p1 = { a.x1, y }, p2 = { a.x2, y };
    lv_draw_line(ctx, &gl, &p1, &p2);
  }

  // ── Tram T1
  lv_draw_line_dsc_t tl;
  lv_draw_line_dsc_init(&tl);
  tl.color = C(HX_TRANSIT);
  tl.opa = 120;
  tl.width = pxPerKm >= 60 ? 3 : 2;
  tl.round_start = tl.round_end = 1;
  for (int i = 0; i + 1 < VN_TRAM_T1_N; i++) {
    lv_point_t p1 = { (lv_coord_t)lroundf(P.x(VN_TRAM_T1[i][1] / 1e5f)), (lv_coord_t)lroundf(P.y(VN_TRAM_T1[i][0] / 1e5f)) };
    lv_point_t p2 = { (lv_coord_t)lroundf(P.x(VN_TRAM_T1[i + 1][1] / 1e5f)), (lv_coord_t)lroundf(P.y(VN_TRAM_T1[i + 1][0] / 1e5f)) };
    if ((p1.x < a.x1 && p2.x < a.x1) || (p1.x > a.x2 && p2.x > a.x2) || (p1.y < a.y1 && p2.y < a.y1) || (p1.y > a.y2 && p2.y > a.y2)) continue;
    lv_draw_line(ctx, &tl, &p1, &p2);
  }
  if (pxPerKm >= 60) {
    lv_draw_rect_dsc_t sd;
    lv_draw_rect_dsc_init(&sd);
    sd.radius = LV_RADIUS_CIRCLE;
    sd.bg_color = C(HX_MAPBG);
    sd.border_color = C(HX_TRANSIT);
    sd.border_width = 1;
    sd.border_opa = 200;
    for (int i = 0; i < VN_TRAM_STOPS_N; i++) {
      lv_coord_t x = (lv_coord_t)lroundf(P.x(VN_TRAM_STOPS[i][1] / 1e5f)), y = (lv_coord_t)lroundf(P.y(VN_TRAM_STOPS[i][0] / 1e5f));
      lv_area_t r = { (lv_coord_t)(x - 2), (lv_coord_t)(y - 2), (lv_coord_t)(x + 2), (lv_coord_t)(y + 2) };
      lv_draw_rect(ctx, &sd, &r);
    }
  }

  // ── Quartiers (sans chevauchement)
  lv_area_t used[24];
  int nUsed = 0;
  for (const Place & p : PLACES) {
    if (p.minor && pxPerKm < 34) continue;
    lv_coord_t x = (lv_coord_t)lroundf(P.x(p.lon)), y = (lv_coord_t)lroundf(P.y(p.lat));
    lv_coord_t w = (lv_coord_t)(strlen(p.name) * 7 + 4);
    lv_area_t r = { (lv_coord_t)(x - w / 2), (lv_coord_t)(y - 6), (lv_coord_t)(x + w / 2), (lv_coord_t)(y + 6) };
    if (r.x2 < a.x1 || r.x1 > a.x2 || r.y2 < a.y1 || r.y1 > a.y2) continue;
    if (overlaps(r, used, nUsed)) continue;
    if (nUsed < 24) used[nUsed++] = r;
    drawText(ctx, p.name, &vn_mono_10, C(HX_TEXT3), 120, r.x1, r.y1, r.x2, LV_TEXT_ALIGN_CENTER, 1);
  }

  // ── Stations : d'abord les équilibrées, les ruptures par-dessus
  bool pills = pxPerKm >= 95;
  lv_coord_t r0 = pxPerKm < 30 ? 3 : pxPerKm < 60 ? 4 : pxPerKm < 95 ? 5 : 8;
  static const uint8_t ORDER[] = { vn::ST_CLOSED, vn::ST_OK, vn::ST_HIGH, vn::ST_LOW, vn::ST_FULL, vn::ST_EMPTY };
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.radius = LV_RADIUS_CIRCLE;
  rd.border_color = C(HX_MAPBG);
  rd.border_width = 1;
  rd.border_opa = LV_OPA_COVER;
  for (uint8_t pass : ORDER) {
    for (int i = 0; i < g_model.n; i++) {
      const vn::Station & s = g_model.st[i];
      if (!s.active || !s.live) continue;
      uint8_t sts = statusAt(i, m->layer);
      if (sts != pass) continue;
      float fx = P.x(s.lon), fy = P.y(s.lat);
      if (fx < a.x1 - 12 || fx > a.x2 + 12 || fy < a.y1 - 12 || fy > a.y2 + 12) continue;
      lv_coord_t x = (lv_coord_t)lroundf(fx), y = (lv_coord_t)lroundf(fy);
      bool match = filterMatch(sts, m->filter);
      lv_color_t c = statusColor(sts);
      rd.bg_color = c;
      rd.bg_opa = match ? LV_OPA_COVER : 55;
      rd.border_opa = match ? LV_OPA_COVER : 60;
      if (pills) {
        char num[8];
        int v = m->layer ? (int)lroundf(s.fc[m->layer]) : s.bikes;
        if (sts == vn::ST_CLOSED) snprintf(num, sizeof(num), "\xC3\x97");
        else snprintf(num, sizeof(num), "%d", v);
        lv_coord_t w = (lv_coord_t)(strlen(num) > 1 ? 22 : 16);
        lv_area_t pr = { (lv_coord_t)(x - w / 2), (lv_coord_t)(y - 8), (lv_coord_t)(x - w / 2 + w - 1), (lv_coord_t)(y + 7) };
        rd.border_width = 2;
        lv_draw_rect(ctx, &rd, &pr);
        drawText(ctx, num, &vn_mono_10, C(HX_BG), match ? LV_OPA_COVER : 140, pr.x1, (lv_coord_t)(y - 6), pr.x2, LV_TEXT_ALIGN_CENTER);
      } else {
        lv_coord_t r = r0 + (s.cap >= 25 && r0 >= 4 ? 1 : 0);
        lv_area_t cr = { (lv_coord_t)(x - r), (lv_coord_t)(y - r), (lv_coord_t)(x + r), (lv_coord_t)(y + r) };
        rd.border_width = 1;
        lv_draw_rect(ctx, &rd, &cr);
      }
      // Anneau ambre : station sans mouvement (communication suspecte)
      if (s.frozen && match) {
        lv_draw_rect_dsc_t fr;
        lv_draw_rect_dsc_init(&fr);
        fr.radius = LV_RADIUS_CIRCLE;
        fr.bg_opa = LV_OPA_TRANSP;
        fr.border_color = C(HX_WARN);
        fr.border_width = 1;
        lv_coord_t rr = (pills ? 11 : r0 + 3);
        lv_area_t a2 = { (lv_coord_t)(x - rr), (lv_coord_t)(y - rr), (lv_coord_t)(x + rr), (lv_coord_t)(y + rr) };
        lv_draw_rect(ctx, &fr, &a2);
      }
    }
  }

  // ── Station sélectionnée : halo + étiquette
  if (m->sel >= 0 && m->sel < g_model.n && g_model.st[m->sel].active) {
    const vn::Station & s = g_model.st[m->sel];
    lv_coord_t x = (lv_coord_t)lroundf(P.x(s.lon)), y = (lv_coord_t)lroundf(P.y(s.lat));
    lv_draw_rect_dsc_t hd;
    lv_draw_rect_dsc_init(&hd);
    hd.radius = LV_RADIUS_CIRCLE;
    hd.bg_opa = LV_OPA_TRANSP;
    hd.border_color = lv_color_white();
    hd.border_width = 2;
    lv_coord_t rr = pills ? 13 : r0 + 5;
    lv_area_t h = { (lv_coord_t)(x - rr), (lv_coord_t)(y - rr), (lv_coord_t)(x + rr), (lv_coord_t)(y + rr) };
    lv_draw_rect(ctx, &hd, &h);
    char txt[64];
    snprintf(txt, sizeof(txt), "%s", s.name);
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, &vn_inter_12, 0, 0, 200, LV_TEXT_FLAG_NONE);
    lv_coord_t bw = sz.x + 14, bh = 20;
    lv_coord_t bx = x - bw / 2, by = y - rr - bh - 4;
    if (by < a.y1 + 34) by = y + rr + 4;
    if (bx < a.x1 + 4) bx = a.x1 + 4;
    if (bx + bw > a.x2 - 4) bx = a.x2 - 4 - bw;
    lv_draw_rect_dsc_t bd;
    lv_draw_rect_dsc_init(&bd);
    bd.radius = 6;
    bd.bg_color = C(HX_S2);
    bd.bg_opa = 245;
    bd.border_color = lv_color_white();
    bd.border_opa = OPA_BORDER_STRONG;
    bd.border_width = 1;
    lv_area_t br = { bx, by, (lv_coord_t)(bx + bw - 1), (lv_coord_t)(by + bh - 1) };
    lv_draw_rect(ctx, &bd, &br);
    drawText(ctx, txt, &vn_inter_12, C(HX_TEXT), LV_OPA_COVER, bx, (lv_coord_t)(by + 3), (lv_coord_t)(bx + bw - 1), LV_TEXT_ALIGN_CENTER);
  }

  // ── Échelle
  {
    lv_coord_t len = (lv_coord_t)lroundf(pxPerKm * stepKm);
    lv_coord_t x2 = a.x2 - 12, x1 = x2 - len, y = a.y1 + 58;
    lv_draw_line_dsc_t sl;
    lv_draw_line_dsc_init(&sl);
    sl.color = C(HX_TEXT3);
    sl.width = 1;
    lv_point_t p1 = { x1, y }, p2 = { x2, y }, p3 = { x1, (lv_coord_t)(y - 3) }, p4 = { x2, (lv_coord_t)(y - 3) };
    lv_draw_line(ctx, &sl, &p1, &p2);
    lv_draw_line(ctx, &sl, &p1, &p3);
    lv_draw_line(ctx, &sl, &p2, &p4);
    char t[12];
    if (stepKm < 1) snprintf(t, sizeof(t), "500 m"); else snprintf(t, sizeof(t), "%d km", (int)stepKm);
    drawText(ctx, t, &vn_mono_10, C(HX_TEXT3), LV_OPA_COVER, x1 - 10, (lv_coord_t)(y - 15), x2 + 10, LV_TEXT_ALIGN_CENTER);
  }
}

static int hitTest(lv_obj_t * obj, lv_point_t p) {
  MapState * m = st(obj);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  Proj P{ (a.x1 + a.x2) / 2.f, (a.y1 + a.y2) / 2.f, m->cLat, m->cLon, m->scale };
  int best = -1;
  float bestD = 18 * 18;
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (!s.active || !s.live) continue;
    float dx = P.x(s.lon) - p.x, dy = P.y(s.lat) - p.y, d = dx * dx + dy * dy;
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

static void zoomAt(lv_obj_t * obj, float f, lv_point_t p) {
  MapState * m = st(obj);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  float cx = (a.x1 + a.x2) / 2.f, cy = (a.y1 + a.y2) / 2.f;
  // point géographique sous le doigt
  float lon = m->cLon + (p.x - cx) / (COSLAT * m->scale);
  float lat = m->cLat - (p.y - cy) / m->scale;
  float ns = m->scale * f;
  if (ns < 1800) ns = 1800;
  if (ns > 40000) ns = 40000;
  m->scale = ns;
  m->cLon = lon - (p.x - cx) / (COSLAT * ns);
  m->cLat = lat + (p.y - cy) / ns;
  lv_obj_invalidate(obj);
}

static void inputEvent(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  MapState * m = st(obj);
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t * indev = lv_indev_get_act();
  if (!indev) return;
  if (code == LV_EVENT_PRESSED) {
    m->moved = 0;
  } else if (code == LV_EVENT_PRESSING) {
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x || v.y) {
      m->moved += LV_ABS(v.x) + LV_ABS(v.y);
      if (m->moved > 6) {
        m->cLon -= v.x / (COSLAT * m->scale);
        m->cLat += v.y / m->scale;
        lv_obj_invalidate(obj);
      }
    }
  } else if (code == LV_EVENT_RELEASED) {
    if (m->moved > 6) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    uint32_t t = lv_tick_get();
    bool dbl = (t - m->lastTapMs) < 350 && LV_ABS(p.x - m->lastTap.x) < 24 && LV_ABS(p.y - m->lastTap.y) < 24;
    m->lastTapMs = t;
    m->lastTap = p;
    if (dbl) { zoomAt(obj, 1.8f, p); m->lastTapMs = 0; return; }
    int s = hitTest(obj, p);
    if (s >= 0) {
      m->sel = s;
      lv_obj_invalidate(obj);
      if (m->cb) m->cb(s);
    }
  }
}

static void deleteEvent(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  MapState * m = st(obj);
  if (m) { m->~MapState(); lv_mem_free(m); }
  lv_obj_set_user_data(obj, nullptr);
}

lv_obj_t * mapCreate(lv_obj_t * parent, lv_coord_t w, lv_coord_t h, MapTapCb cb) {
  lv_obj_t * o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(o, C(HX_MAPBG), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  void * mem = lv_mem_alloc(sizeof(MapState));
  MapState * m = new (mem) MapState();
  m->cb = cb;
  lv_obj_set_user_data(o, m);
  lv_obj_add_event_cb(o, drawEvent, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(o, inputEvent, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(o, inputEvent, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(o, inputEvent, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(o, deleteEvent, LV_EVENT_DELETE, nullptr);
  return o;
}

void mapSetLayer(lv_obj_t * map, int k) { st(map)->layer = k; lv_obj_invalidate(map); }
void mapSetFilter(lv_obj_t * map, MapFilter f) { st(map)->filter = f; lv_obj_invalidate(map); }
void mapSelect(lv_obj_t * map, int s) { st(map)->sel = s; lv_obj_invalidate(map); }
void mapRefresh(lv_obj_t * map) { lv_obj_invalidate(map); }
void mapCenter(lv_obj_t * map, float lat, float lon, float scale) {
  MapState * m = st(map);
  m->cLat = lat; m->cLon = lon;
  if (scale > 0) m->scale = scale;
  lv_obj_invalidate(map);
}

void mapZoom(lv_obj_t * map, float f) {
  lv_area_t a;
  lv_obj_get_coords(map, &a);
  lv_point_t c = { (lv_coord_t)((a.x1 + a.x2) / 2), (lv_coord_t)((a.y1 + a.y2) / 2) };
  zoomAt(map, f, c);
}

void mapFit(lv_obj_t * map, bool whole) {
  MapState * m = st(map);
  if (!whole) { m->cLat = 49.6085f; m->cLon = 6.1330f; m->scale = 5600.f; lv_obj_invalidate(map); return; }
  float la1 = 90, la2 = -90, lo1 = 180, lo2 = -180;
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (!s.active) continue;
    la1 = fminf(la1, s.lat); la2 = fmaxf(la2, s.lat); lo1 = fminf(lo1, s.lon); lo2 = fmaxf(lo2, s.lon);
  }
  if (la1 > la2) return;
  lv_coord_t w = lv_obj_get_width(map) - 40, h = lv_obj_get_height(map) - 70;
  m->cLat = (la1 + la2) / 2; m->cLon = (lo1 + lo2) / 2;
  float sx = w / fmaxf(1e-4f, (lo2 - lo1) * COSLAT), sy = h / fmaxf(1e-4f, la2 - la1);
  m->scale = fminf(sx, sy);
  m->cLat -= 8 / m->scale;      // décalage : laisser de la place aux filtres en haut
  lv_obj_invalidate(map);
}

} // namespace ui
