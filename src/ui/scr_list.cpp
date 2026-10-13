// ── Écran « Stations » : toutes les stations, triables ───────────────
#include "ui_internal.h"
#include "widgets/vn_map.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

using vn::g_model;

namespace ui {

struct Row { lv_obj_t * obj, * bar, * name, * sub, * nums, * meter, * fcv, * fcl; };
static Row s_rows[VN_MAX_ST];
static int s_nRows = 0;
static lv_obj_t * s_list, * s_seg, * s_count;
static int s_sort = 0;
static int16_t s_order[VN_MAX_ST];

static int cmpRisk(const void * a, const void * b) {
  const vn::Station & x = g_model.st[*(const int16_t *)a], & y = g_model.st[*(const int16_t *)b];
  if (x.risk != y.risk) return y.risk - x.risk;
  return strcasecmp(x.name, y.name);
}
static int cmpName(const void * a, const void * b) {
  return strcasecmp(g_model.st[*(const int16_t *)a].name, g_model.st[*(const int16_t *)b].name);
}
static int cmpFill(const void * a, const void * b) {
  const vn::Station & x = g_model.st[*(const int16_t *)a], & y = g_model.st[*(const int16_t *)b];
  bool cx = x.status == vn::ST_CLOSED, cy = y.status == vn::ST_CLOSED;
  if (cx != cy) return cx ? 1 : -1;
  float fx = x.fill(), fy = y.fill();
  return fx < fy ? -1 : fx > fy ? 1 : 0;
}
static int etaOf(const vn::Station & s) {
  int e = 9999;
  if (s.etaEmpty >= 0) e = s.etaEmpty;
  if (s.etaFull >= 0 && s.etaFull < e) e = s.etaFull;
  return e;
}
static int cmpEta(const void * a, const void * b) {
  const vn::Station & x = g_model.st[*(const int16_t *)a], & y = g_model.st[*(const int16_t *)b];
  int ex = etaOf(x), ey = etaOf(y);
  if (ex != ey) return ex - ey;
  return y.risk - x.risk;
}

static void rowClick(lv_event_t * e) {
  int st = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (st >= 0) openStation(st);
}

static Row & makeRow(int i) {
  Row & r = s_rows[i];
  r.obj = row(s_list, 8);
  lv_obj_set_size(r.obj, LV_PCT(100), 40);
  lv_obj_set_style_pad_hor(r.obj, 10, 0);
  lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r.obj, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_STATE_PRESSED);
  if (i > 0) {
    lv_obj_set_style_border_side(r.obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(r.obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(r.obj, OPA_BORDER, 0);
    lv_obj_set_style_border_width(r.obj, 1, 0);
  }
  lv_obj_add_event_cb(r.obj, rowClick, LV_EVENT_CLICKED, nullptr);
  r.bar = box(r.obj);
  lv_obj_set_size(r.bar, 3, 24);
  lv_obj_set_style_radius(r.bar, 2, 0);
  lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
  lv_obj_t * c = col(r.obj, 0);
  lv_obj_set_flex_grow(c, 1);
  r.name = label(c, &vn_inter_13s, HX_TEXT, "");
  oneLine(r.name, 196);
  r.sub = label(c, &vn_inter_11, HX_TEXT3, "");
  oneLine(r.sub, 196);
  lv_obj_t * n = col(r.obj, 3);
  lv_obj_set_width(n, 66);
  r.nums = label(n, &vn_inter_13s, HX_TEXT, "");
  lv_label_set_recolor(r.nums, true);
  r.meter = meter(n, 64, 4);
  lv_obj_t * f = col(r.obj, 0);
  lv_obj_set_width(f, 50);
  r.fcl = label(f, &vn_mono_10, HX_TEXT3, "+1 H");
  r.fcv = label(f, &vn_inter_13s, HX_TEXT2, "");
  icon(r.obj, &ic_chevron_right_12, HX_TEXT3);
  return r;
}

static void fillRow(Row & r, int si) {
  const vn::Station & s = g_model.st[si];
  lv_obj_set_user_data(r.obj, (void *)(intptr_t)si);
  lv_obj_set_style_bg_color(r.bar, statusColor(s.status), 0);
  lv_label_set_text(r.name, s.name);
  char b[96], e[32];
  int eta = etaOf(s);
  if (s.status == vn::ST_CLOSED) snprintf(b, sizeof(b), "#%s \xC2\xB7 ferm\xC3\xA9" "e \xC2\xB7 %d bornes", s.sid, s.cap);
  else if (s.status == vn::ST_EMPTY && s.emptySince) { fmtDur(e, sizeof(e), (int)((now() - s.emptySince) / 60)); snprintf(b, sizeof(b), "#%s \xC2\xB7 vide depuis %s", s.sid, e); }
  else if (s.status == vn::ST_FULL && s.fullSince) { fmtDur(e, sizeof(e), (int)((now() - s.fullSince) / 60)); snprintf(b, sizeof(b), "#%s \xC2\xB7 pleine depuis %s", s.sid, e); }
  else if (eta <= 120) { fmtDur(e, sizeof(e), eta); snprintf(b, sizeof(b), "#%s \xC2\xB7 %s dans ~%s", s.sid, s.etaEmpty == eta ? "rupture" : "saturation", e); }
  else if (s.bikesDis) snprintf(b, sizeof(b), "#%s \xC2\xB7 %d bornes \xC2\xB7 %d v\xC3\xA9lo%s HS", s.sid, s.cap, s.bikesDis, s.bikesDis > 1 ? "s" : "");
  else snprintf(b, sizeof(b), "#%s \xC2\xB7 %d bornes", s.sid, s.cap);
  lv_label_set_text(r.sub, b);
  lv_color_t sc = statusColor(s.status);
  snprintf(b, sizeof(b), "#%06X %d# #808A99 / %d#", (unsigned)(lv_color_to32(sc) & 0xFFFFFF), s.bikes, s.docks);
  lv_label_set_text(r.nums, b);
  int v[3] = { s.bikes, s.docks, s.bikesDis + s.docksDis };
  lv_color_t cc[3] = { sc, lv_color_make(0x3A, 0x40, 0x48), C(HX_WARN) };
  meterSet(r.meter, v, cc, 3);
  if (s.status == vn::ST_CLOSED) { lv_label_set_text(r.fcv, "\xE2\x80\x94"); lv_obj_set_style_text_color(r.fcv, C(HX_TEXT3), 0); }
  else {
    int f = (int)lroundf(s.fc[12]);
    snprintf(b, sizeof(b), "\xE2\x86\x92 %d", f);
    lv_label_set_text(r.fcv, b);
    lv_obj_set_style_text_color(r.fcv, statusColor(statusAt(si, 12)), 0);
  }
}

static void refresh() {
  int n = 0;
  for (int i = 0; i < g_model.n; i++) if (g_model.st[i].active && g_model.st[i].live) s_order[n++] = (int16_t)i;
  int (*cmp)(const void *, const void *) = s_sort == 0 ? cmpRisk : s_sort == 1 ? cmpName : s_sort == 2 ? cmpFill : cmpEta;
  qsort(s_order, n, sizeof(int16_t), cmp);
  while (s_nRows < n) makeRow(s_nRows++);
  for (int i = 0; i < s_nRows; i++) {
    if (i < n) { lv_obj_clear_flag(s_rows[i].obj, LV_OBJ_FLAG_HIDDEN); fillRow(s_rows[i], s_order[i]); }
    else lv_obj_add_flag(s_rows[i].obj, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text_fmt(s_count, "%d stations", n);
}

void listSetSort(int mode) {
  s_sort = mode;
  if (s_seg) segSet(s_seg, mode, true);
  refresh();
  if (s_list) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

static void build(lv_obj_t * root) {
  static const char * L[4] = { "Priorit\xC3\xA9", "Nom", "Remplissage", "\xC3\x89" "ch\xC3\xA9" "ance" };
  s_seg = segmented(root, L, 4, s_sort, [](int i) { listSetSort(i); }, 32);
  lv_obj_set_width(s_seg, 300);
  lv_obj_set_pos(s_seg, 8, 6);
  segSet(s_seg, s_sort, false);
  s_count = label(root, &vn_mono_10, HX_TEXT3, "");
  lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, -10, 16);
  lv_obj_t * holder = box(root);
  lv_obj_set_pos(holder, 8, 44);
  lv_obj_set_size(holder, CONTENT_W - 16, CONTENT_H - 44);   // liste sous la barre de tri
  s_list = card(holder, 0);
  lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_border_side(s_list, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_radius(s_list, 10, 0);
  lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(s_list, C(HX_TEXT3), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(s_list, 120, LV_PART_SCROLLBAR);
}

const ScreenDef SCR_LIST = { "Stations", "STATIONS", &ic_list_20, build, refresh, nullptr };

} // namespace ui
