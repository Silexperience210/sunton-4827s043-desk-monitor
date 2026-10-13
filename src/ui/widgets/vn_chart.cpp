#include "vn_chart.h"
#include "../theme.h"
#include <new>
#include <string.h>

namespace ui {

static lv_point_t s_pts[640];

struct Ctx {
  lv_coord_t px1, px2, py1, py2;
  float W, H;
  const ChartData * d;
  float X(float xi) const;
  float Y(float v) const;
};

// Hors ligne (noinline) : réduit la pression sur les registres flottants de drawEvent,
// ce qui évite l'ICE de GCC 8.4 xtensa en postreload.
__attribute__((noinline)) float Ctx::X(float xi) const {
  if (d->nx <= 1) return (float)px1;
  return (float)px1 + xi * W / (float)(d->nx - 1);
}
__attribute__((noinline)) float Ctx::Y(float v) const {
  float span = d->ymax - d->ymin;
  if (span < 1e-6f) return (float)py2;
  float t = (v - d->ymin) / span;
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  return (float)py2 - t * H;
}

// Construit la polyligne (en pixels) d'un tronçon continu ; renvoie le nombre de points
// et l'index où reprendre. Fusionne les points distants de < 2 px (moyenne).
__attribute__((noinline)) static int buildRun(const Ctx & c, const Series & s, int & i) {
  int np = 0;
  while (i < s.n && isnan(s.y[i])) i++;
  float accX = 0, accY = 0; int accN = 0; lv_coord_t lastPx = -32768;
  for (; i < s.n && !isnan(s.y[i]); i++) {
    float x = c.X((float)(s.x0 + i)), y = c.Y(s.y[i]);
    if (accN && x - lastPx >= 2.f) {
      if (np < (int)(sizeof(s_pts) / sizeof(s_pts[0]))) {
        s_pts[np].x = (lv_coord_t)lroundf(accX / accN);
        s_pts[np].y = (lv_coord_t)lroundf(accY / accN);
        np++;
      }
      accX = accY = 0; accN = 0;
    }
    if (!accN) lastPx = (lv_coord_t)x;
    accX += x; accY += y; accN++;
  }
  if (accN && np < (int)(sizeof(s_pts) / sizeof(s_pts[0]))) {
    s_pts[np].x = (lv_coord_t)lroundf(accX / accN);
    s_pts[np].y = (lv_coord_t)lroundf(accY / accN);
    np++;
  }
  return np;
}

__attribute__((noinline)) static void drawArea(lv_draw_ctx_t * ctx, const Ctx & c, const Series & s, int np) {
  lv_coord_t top = c.py2;
  for (int k = 0; k < np; k++) if (s_pts[k].y < top) top = s_pts[k].y;
  lv_area_t fa = { c.px1, top, c.px2, c.py2 };
  lv_draw_mask_fade_param_t fade;
  lv_draw_mask_fade_init(&fade, &fa, LV_OPA_COVER, top, LV_OPA_TRANSP, c.py2);
  int16_t fadeId = lv_draw_mask_add(&fade, nullptr);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_color = s.color;
  rd.bg_opa = s.fillOpa;
  for (int k = 0; k + 1 < np; k++) {
    lv_point_t p1 = s_pts[k], p2 = s_pts[k + 1];
    if (p2.x <= p1.x) continue;
    lv_draw_mask_line_param_t lm;
    lv_draw_mask_line_points_init(&lm, p1.x, p1.y, p2.x, p2.y, LV_DRAW_MASK_LINE_SIDE_BOTTOM);
    int16_t lid = lv_draw_mask_add(&lm, nullptr);
    lv_area_t ra = { p1.x, (lv_coord_t)LV_MIN(p1.y, p2.y), (lv_coord_t)(p2.x - 1), c.py2 };
    lv_draw_rect(ctx, &rd, &ra);
    lv_draw_mask_free_param(&lm);
    lv_draw_mask_remove_id(lid);
  }
  lv_draw_mask_free_param(&fade);
  lv_draw_mask_remove_id(fadeId);
}

__attribute__((noinline)) static void drawPolyline(lv_draw_ctx_t * ctx, const Series & s, int np) {
  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = s.color;
  ld.width = s.width;
  ld.opa = s.opa;
  ld.round_start = 1;
  ld.round_end = 1;
  if (np == 1) { lv_draw_line(ctx, &ld, &s_pts[0], &s_pts[0]); return; }
  for (int k = 0; k + 1 < np; k++) lv_draw_line(ctx, &ld, &s_pts[k], &s_pts[k + 1]);
}

__attribute__((noinline)) static void drawDots(lv_draw_ctx_t * ctx, const Series & s, int np) {
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_color = s.color;
  rd.bg_opa = s.opa;
  rd.radius = LV_RADIUS_CIRCLE;
  const float step = s.width + 2.6f;
  float carry = 0;
  lv_coord_t r = s.width > 1 ? 1 : 0;
  for (int k = 0; k + 1 < np; k++) {
    float x1 = s_pts[k].x, y1 = s_pts[k].y, x2 = s_pts[k + 1].x, y2 = s_pts[k + 1].y;
    float L = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    float t = carry;
    while (t <= L) {
      float f = L > 0 ? t / L : 0;
      lv_coord_t x = (lv_coord_t)lroundf(x1 + (x2 - x1) * f), y = (lv_coord_t)lroundf(y1 + (y2 - y1) * f);
      lv_area_t a = { (lv_coord_t)(x - r), (lv_coord_t)(y - r), (lv_coord_t)(x + r), (lv_coord_t)(y + r) };
      lv_draw_rect(ctx, &rd, &a);
      t += step;
    }
    carry = t - L;
  }
}

__attribute__((noinline)) static void drawBars(lv_draw_ctx_t * ctx, const Ctx & c, const Series & s) {
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = s.opa;
  rd.radius = 1;
  float stepPx = c.d->nx > 1 ? c.W / (c.d->nx - 1) : c.W;
  lv_coord_t bw = (lv_coord_t)LV_MAX(1, lroundf(stepPx * 0.62f));
  for (int i = 0; i < s.n; i++) {
    if (isnan(s.y[i])) continue;
    lv_coord_t cx = (lv_coord_t)lroundf(c.X((float)(s.x0 + i)));
    lv_coord_t y = (lv_coord_t)lroundf(c.Y(s.y[i]));
    if (y > c.py2 - 1) y = c.py2 - 1;
    lv_area_t a = { (lv_coord_t)(cx - bw / 2), y, (lv_coord_t)(cx - bw / 2 + bw - 1), c.py2 };
    rd.bg_color = s.barColors ? s.barColors[i] : s.color;
    lv_draw_rect(ctx, &rd, &a);
  }
}

__attribute__((noinline)) static void drawText(lv_draw_ctx_t * ctx, const char * txt, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_text_align_t al,
                     lv_color_t col) {
  if (!txt || !*txt) return;
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.font = &vn_mono_10;
  ld.color = col;
  ld.align = al;
  lv_area_t a = { x1, y1, x2, (lv_coord_t)(y1 + 12) };
  lv_draw_label(ctx, &ld, &a, txt, nullptr);
}

static void drawEvent(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  ChartData * d = (ChartData *)lv_obj_get_user_data(obj);
  if (!d) return;
  lv_draw_ctx_t * ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  Ctx c;
  c.d = d;
  c.px1 = a.x1 + d->padL; c.px2 = a.x2 - d->padR;
  c.py1 = a.y1 + d->padT; c.py2 = a.y2 - d->padB;
  c.W = (float)(c.px2 - c.px1); c.H = (float)(c.py2 - c.py1);
  if (c.W < 4 || c.H < 4) return;

  // Grille horizontale pointillée (0 / 50 / 100 %)
  lv_draw_line_dsc_t gl;
  lv_draw_line_dsc_init(&gl);
  gl.color = lv_color_white();
  gl.opa = 16;
  gl.width = 1;
  gl.dash_width = 2;
  gl.dash_gap = 3;
  if (d->gridY) {
    for (int k = 0; k <= 2; k++) {
      lv_coord_t y = (lv_coord_t)(c.py1 + k * c.H / 2);
      lv_point_t p1 = { c.px1, y }, p2 = { c.px2, y };
      lv_draw_line(ctx, &gl, &p1, &p2);
    }
  } else {
    lv_point_t p1 = { c.px1, c.py2 }, p2 = { c.px2, c.py2 };
    lv_draw_line(ctx, &gl, &p1, &p2);
  }
  if (!isnan(d->hline)) {
    lv_draw_line_dsc_t hl = gl;
    hl.opa = 70;
    hl.dash_width = 4;
    hl.dash_gap = 3;
    lv_coord_t y = (lv_coord_t)lroundf(c.Y(d->hline));
    lv_point_t p1 = { c.px1, y }, p2 = { c.px2, y };
    lv_draw_line(ctx, &hl, &p1, &p2);
  }
  // Étiquettes
  lv_color_t t3 = C(HX_TEXT3);
  drawText(ctx, d->ylab[0], a.x1, c.py1 - 5, c.px1 - 3, LV_TEXT_ALIGN_RIGHT, t3);
  drawText(ctx, d->ylab[1], a.x1, c.py2 - 6, c.px1 - 3, LV_TEXT_ALIGN_RIGHT, t3);
  for (int k = 0; k < d->nxl && k < 8; k++) {
    lv_coord_t x = (lv_coord_t)lroundf(c.X((float)d->xlPos[k]));
    lv_coord_t x1 = x - 22, x2 = x + 22;
    lv_text_align_t al = LV_TEXT_ALIGN_CENTER;
    if (x1 < a.x1) { x1 = a.x1; x2 = x1 + 44; al = LV_TEXT_ALIGN_LEFT; }
    if (x2 > a.x2) { x2 = a.x2; x1 = x2 - 44; al = LV_TEXT_ALIGN_RIGHT; }
    drawText(ctx, d->xl[k], x1, c.py2 + 1, x2, al, t3);
  }

  // Repère « maintenant »
  if (d->nowX >= 0 && d->nowX < d->nx) {
    lv_draw_line_dsc_t nl = gl;
    nl.color = C(HX_ACCENT);
    nl.opa = 150;
    nl.dash_width = 2;
    nl.dash_gap = 2;
    lv_coord_t x = (lv_coord_t)lroundf(c.X((float)d->nowX));
    lv_point_t p1 = { x, c.py1 }, p2 = { x, c.py2 };
    lv_draw_line(ctx, &nl, &p1, &p2);
  }

  // Séries
  for (int k = 0; k < d->ns; k++) {
    const Series & s = d->s[k];
    if (!s.y || s.n <= 0) continue;
    if (s.kind == SK_BARS) { drawBars(ctx, c, s); continue; }
    int i = 0;
    while (i < s.n) {
      int np = buildRun(c, s, i);
      if (np <= 0) continue;
      if (s.kind == SK_AREA) { drawArea(ctx, c, s, np); drawPolyline(ctx, s, np); }
      else if (s.kind == SK_LINE) drawPolyline(ctx, s, np);
      else drawDots(ctx, s, np);
    }
  }

  // Point lumineux en bout de la série 0
  if (d->endDot && d->ns > 0 && d->s[0].kind != SK_BARS) {
    const Series & s = d->s[0];
    for (int i = s.n - 1; i >= 0; i--) {
      if (isnan(s.y[i])) continue;
      lv_coord_t x = (lv_coord_t)lroundf(c.X((float)(s.x0 + i))), y = (lv_coord_t)lroundf(c.Y(s.y[i]));
      lv_draw_rect_dsc_t rd;
      lv_draw_rect_dsc_init(&rd);
      rd.radius = LV_RADIUS_CIRCLE;
      rd.bg_color = s.color;
      rd.bg_opa = 60;
      lv_area_t g = { (lv_coord_t)(x - 6), (lv_coord_t)(y - 6), (lv_coord_t)(x + 6), (lv_coord_t)(y + 6) };
      lv_draw_rect(ctx, &rd, &g);
      rd.bg_opa = LV_OPA_COVER;
      rd.border_color = C(HX_BG);
      rd.border_width = 1;
      rd.border_opa = LV_OPA_COVER;
      lv_area_t p = { (lv_coord_t)(x - 3), (lv_coord_t)(y - 3), (lv_coord_t)(x + 3), (lv_coord_t)(y + 3) };
      lv_draw_rect(ctx, &rd, &p);
      break;
    }
  }
}

static void deleteEvent(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  ChartData * d = (ChartData *)lv_obj_get_user_data(obj);
  if (d) { d->~ChartData(); lv_mem_free(d); }
  lv_obj_set_user_data(obj, nullptr);
}

lv_obj_t * chartCreate(lv_obj_t * parent, lv_coord_t w, lv_coord_t h) {
  lv_obj_t * o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, w, h);
  void * mem = lv_mem_alloc(sizeof(ChartData));
  ChartData * d = new (mem) ChartData();
  lv_obj_set_user_data(o, d);
  lv_obj_add_event_cb(o, drawEvent, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(o, deleteEvent, LV_EVENT_DELETE, nullptr);
  return o;
}

ChartData * chartData(lv_obj_t * chart) { return (ChartData *)lv_obj_get_user_data(chart); }
void chartInvalidate(lv_obj_t * chart) { lv_obj_invalidate(chart); }

} // namespace ui
