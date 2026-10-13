// ── Fiche station (bottom sheet) : temps réel, 24 h, profil, prévision ─
#include "ui_internal.h"
#include "widgets/vn_chart.h"
#include "widgets/vn_map.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

using vn::g_model;

namespace ui {

void needText(int need, char * out, size_t n);

static const int NX = VN_RING_SLOTS + VN_FC_STEPS - 1;
static float s_real[VN_RING_SLOTS], s_prof[NX], s_fc[VN_FC_STEPS];

struct SheetW {
  int st = -1, token = -1;
  lv_obj_t * badge, * meta, * name;
  lv_obj_t * statV[4];
  lv_obj_t * meter, * capL;
  lv_obj_t * chart, * chartMeta;
  lv_obj_t * fcV[4];
  lv_obj_t * insFlash, * insIc, * insTitle, * insWhy, * insNeed, * insConf;
  lv_obj_t * lastMove, * lastRep, * addr;
};
static SheetW W;

static lv_obj_t * statCol(lv_obj_t * parent, const char * lbl, lv_obj_t ** val, bool first) {
  lv_obj_t * c = col(parent, 0);
  lv_obj_set_flex_grow(c, 1);
  if (!first) {
    lv_obj_set_style_pad_left(c, 10, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_opa(c, OPA_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
  }
  *val = label(c, &vn_inter_20b, HX_TEXT, "\xE2\x80\x94");
  eyebrow(c, lbl);
  return c;
}

static void fill() {
  if (W.st < 0 || W.st >= g_model.n) return;
  const vn::Station & s = g_model.st[W.st];
  int64_t t = now();
  char b[128], c2[32];

  badgeSet(W.badge, vn::statusShort(s.status), statusTone(s.status));
  snprintf(b, sizeof(b), "#%s \xC2\xB7 %d bornes \xC2\xB7 v\xC3\xA9los \xC3\xA9lectriques", s.sid, s.cap);
  lv_label_set_text(W.meta, b);
  lv_label_set_text(W.name, s.name);

  lv_color_t sc = statusColor(s.status);
  lv_label_set_text_fmt(W.statV[0], "%d", s.bikes);
  lv_obj_set_style_text_color(W.statV[0], sc, 0);
  lv_label_set_text_fmt(W.statV[1], "%d", s.elec);
  lv_obj_set_style_text_color(W.statV[1], s.elec ? C(HX_ELEC) : C(HX_TEXT3), 0);
  lv_label_set_text_fmt(W.statV[2], "%d", s.docks);
  lv_obj_set_style_text_color(W.statV[2], s.docks ? C(HX_TEXT) : C(HX_BAD), 0);
  lv_label_set_text_fmt(W.statV[3], "%d", s.bikesDis + s.docksDis);
  lv_obj_set_style_text_color(W.statV[3], (s.bikesDis + s.docksDis) ? C(HX_WARN) : C(HX_TEXT3), 0);
  int v[4] = { s.elec, s.bikes - s.elec, s.docks, s.bikesDis + s.docksDis };
  lv_color_t cc[4] = { C(HX_ELEC), C(HX_MECA), lv_color_make(0x34, 0x38, 0x3E), C(HX_WARN) };
  meterSet(W.meter, v, cc, 4);
  snprintf(b, sizeof(b), "CAP. %d", s.cap);
  lv_label_set_text(W.capL, b);

  // Graphique : 24 h réelles, profil habituel, prévision 2 h
  ChartData * d = chartData(W.chart);
  for (int x = 0; x < VN_RING_SLOTS; x++) s_real[x] = NAN;
  for (int bk = 0; bk < g_model.count; bk++) {
    int idx = g_model.ringIndex(bk);
    uint8_t vv = g_model.ring[(size_t)idx * VN_MAX_ST + W.st];
    if (vv != 255) s_real[VN_RING_SLOTS - 1 - bk] = vv;
  }
  if (s.live) s_real[VN_RING_SLOTS - 1] = s.bikes;
  int64_t slot0 = (t - t % VN_RING_STEP_S) - (int64_t)(VN_RING_SLOTS - 1) * VN_RING_STEP_S;
  bool anyProf = false;
  for (int x = 0; x < NX; x++) {
    float a, n;
    if (g_model.profileAt(W.st, slot0 + (int64_t)x * VN_RING_STEP_S, a, n)) { s_prof[x] = a; anyProf = true; }
    else s_prof[x] = NAN;
  }
  for (int k = 0; k < VN_FC_STEPS; k++) s_fc[k] = s.fc[k];
  float capE = s.capEff();
  d->ymin = 0;
  d->ymax = fmaxf(4.f, fmaxf(capE, (float)s.cap));
  d->hline = capE;
  snprintf(d->ylab[0], sizeof(d->ylab[0]), "%d", (int)d->ymax);
  snprintf(d->ylab[1], sizeof(d->ylab[1]), "0");
  d->s[0].color = C(HX_GOOD);
  d->nxl = 0;
  for (int x = 0; x < NX && d->nxl < 6; x++) {
    struct tm lt;
    plat::localTime(slot0 + (int64_t)x * VN_RING_STEP_S, &lt);
    if (lt.tm_min < 5 && lt.tm_hour % 6 == 0 && x > 6 && x < NX - 6) {
      d->xlPos[d->nxl] = x;
      snprintf(d->xl[d->nxl], 8, "%02dh", lt.tm_hour);
      d->nxl++;
    }
  }
  chartInvalidate(W.chart);
  lv_label_set_text(W.chartMeta, anyProf ? "#2ECC8F \xE2\x80\x94# r\xC3\xA9" "el  #808A99 \xE2\x80\x94# habituel  #F5820D \xE2\x80\xA2\xE2\x80\xA2# pr\xC3\xA9vu"
                                         : "#2ECC8F \xE2\x80\x94# r\xC3\xA9" "el  #F5820D \xE2\x80\xA2\xE2\x80\xA2# pr\xC3\xA9vu");

  // Prévisions
  static const int K[4] = { 3, 6, 12, 24 };
  for (int k = 0; k < 4; k++) {
    if (s.status == vn::ST_CLOSED) { lv_label_set_text(W.fcV[k], "\xE2\x80\x94"); continue; }
    lv_label_set_text_fmt(W.fcV[k], "%d", (int)lroundf(s.fc[K[k]]));
    lv_obj_set_style_text_color(W.fcV[k], statusColor(statusAt(W.st, K[k])), 0);
  }

  // Lecture opérationnelle
  Tone tone = T_GOOD;
  const lv_img_dsc_t * ic = &ic_check_16;
  char dur[24];
  if (s.status == vn::ST_CLOSED) {
    tone = T_CLOSED; ic = &ic_ban_16;
    snprintf(b, sizeof(b), "%s", !s.installed ? "Station non install\xC3\xA9" "e" : "Station hors service");
  } else if (s.status == vn::ST_EMPTY) {
    tone = T_BAD; ic = &ic_alert_16;
    if (s.emptySince) { fmtDur(dur, sizeof(dur), (int)((t - s.emptySince) / 60)); snprintf(b, sizeof(b), "Vide depuis %s", dur); }
    else snprintf(b, sizeof(b), "Station vide");
  } else if (s.status == vn::ST_FULL) {
    tone = T_TRANSIT; ic = &ic_dock_16;
    if (s.fullSince) { fmtDur(dur, sizeof(dur), (int)((t - s.fullSince) / 60)); snprintf(b, sizeof(b), "Pleine depuis %s", dur); }
    else snprintf(b, sizeof(b), "Station pleine");
  } else if (s.etaEmpty >= 0 && (s.etaFull < 0 || s.etaEmpty <= s.etaFull)) {
    tone = T_WARN; ic = &ic_trend_down_16;
    fmtDur(dur, sizeof(dur), s.etaEmpty);
    snprintf(b, sizeof(b), "Rupture pr\xC3\xA9vue dans ~%s", dur);
  } else if (s.etaFull >= 0) {
    tone = T_ELEC; ic = &ic_trend_up_16;
    fmtDur(dur, sizeof(dur), s.etaFull);
    snprintf(b, sizeof(b), "Saturation pr\xC3\xA9vue dans ~%s", dur);
  } else {
    snprintf(b, sizeof(b), "Situation stable sur 2 h");
  }
  lv_label_set_text(W.insTitle, b);
  lv_color_t tc = toneColor(tone);
  lv_obj_set_style_bg_color(W.insFlash, tone == T_CLOSED ? C(HX_CLOSED) : tc, 0);
  lv_img_set_src(W.insIc, ic);
  iconColor(W.insIc, C(HX_BG));
  fmtSigned(c2, sizeof(c2), s.delta30);
  float pNow, nNow;
  if (g_model.profileAt(W.st, t + 3600, pNow, nNow)) {
    char hh[8];
    fmtClock(hh, sizeof(hh), t + 3600);
    int hab = (int)lroundf(pNow);
    snprintf(b, sizeof(b), "Tendance %s v\xC3\xA9lo%s en 30 min \xC2\xB7 habituellement %d v\xC3\xA9lo%s \xC3\xA0 %s", c2,
             abs(s.delta30) > 1 ? "s" : "", hab, hab > 1 ? "s" : "", hh);
  } else {
    snprintf(b, sizeof(b), "Tendance %s v\xC3\xA9lo%s en 30 min \xC2\xB7 profil habituel en cours d'apprentissage", c2,
             abs(s.delta30) > 1 ? "s" : "");
  }
  lv_label_set_text(W.insWhy, b);
  if (s.need) {
    char nd[24];
    needText(s.need, nd, sizeof(nd));
    snprintf(b, sizeof(b), "%s %s", s.need > 0 ? "APPORTER" : "RETIRER", nd);
    badgeSet(W.insNeed, b, s.need > 0 ? T_GOOD : T_TRANSIT);
    lv_obj_clear_flag(W.insNeed, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(W.insNeed, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(W.insConf, "CONFIANCE %d %%", s.conf);

  char a1[32];
  fmtAgo(a1, sizeof(a1), s.lastChange, t);
  snprintf(b, sizeof(b), "Dernier mouvement d\xC3\xA9tect\xC3\xA9 %s", a1);
  lv_label_set_text(W.lastMove, b);
  fmtAgo(a1, sizeof(a1), s.lastReported, t);
  snprintf(b, sizeof(b), "Dernier rapport de la borne (GBFS) %s", a1);
  lv_label_set_text(W.lastRep, b);
  lv_label_set_text(W.addr, s.addr[0] ? s.addr : "\xE2\x80\x94");
}

void openStation(int st) {
  if (st < 0 || st >= g_model.n) return;
  lv_obj_t * body = sheetOpen(nullptr);
  W = SheetW();
  W.st = st;
  W.token = sheetToken();
  const lv_coord_t BW = CONTENT_W - 8 - 24;

  lv_obj_t * head = row(body, 6);
  W.badge = badge(head, "", T_NEUTRAL);
  W.meta = label(head, &vn_inter_12, HX_TEXT2, "");
  W.name = label(body, &vn_inter_15b, HX_TEXT, "");
  oneLine(W.name, BW - 30);

  lv_obj_t * grid = row(body, 0);
  lv_obj_set_width(grid, BW);
  statCol(grid, "V\xC3\x89LOS", &W.statV[0], true);
  statCol(grid, "\xC3\x89LEC", &W.statV[1], false);
  statCol(grid, "BORNES", &W.statV[2], false);
  statCol(grid, "HORS SERV.", &W.statV[3], false);

  lv_obj_t * mr = row(body, 10);
  lv_obj_set_width(mr, BW);
  W.meter = meter(mr, BW - 60, 6);
  W.capL = label(mr, &vn_mono_10, HX_TEXT3, "");

  lv_obj_t * cc = card(body, 8);
  lv_obj_set_width(cc, BW);
  lv_obj_set_style_bg_color(cc, C(HX_BG), 0);
  lv_obj_t * ch = row(cc, 6);
  lv_obj_set_width(ch, LV_PCT(100));
  eyebrow(ch, "24 H \xC2\xB7 PR\xC3\x89VISION 2 H");
  W.chartMeta = label(ch, &vn_mono_10, HX_TEXT3, "");
  lv_label_set_recolor(W.chartMeta, true);
  lv_obj_set_flex_grow(W.chartMeta, 1);
  lv_obj_set_style_text_align(W.chartMeta, LV_TEXT_ALIGN_RIGHT, 0);
  W.chart = chartCreate(cc, BW - 16, 92);
  lv_obj_set_y(W.chart, 18);
  ChartData * d = chartData(W.chart);
  d->nx = NX;
  d->ns = 3;
  d->s[0].y = s_real; d->s[0].n = VN_RING_SLOTS; d->s[0].kind = SK_AREA; d->s[0].width = 2; d->s[0].fillOpa = 90;
  d->s[1].y = s_prof; d->s[1].n = NX; d->s[1].kind = SK_LINE; d->s[1].width = 1; d->s[1].color = C(HX_TEXT3); d->s[1].opa = 150;
  d->s[2].y = s_fc; d->s[2].n = VN_FC_STEPS; d->s[2].x0 = VN_RING_SLOTS - 1; d->s[2].kind = SK_DOTS; d->s[2].width = 2;
  d->s[2].color = C(HX_ACCENT);
  d->nowX = VN_RING_SLOTS - 1;
  d->padL = 20;

  // Prévisions (.vn-fc)
  lv_obj_t * fr = row(body, 6);
  lv_obj_set_width(fr, BW);
  static const char * FL[4] = { "+15 MIN", "+30 MIN", "+1 H", "+2 H" };
  for (int k = 0; k < 4; k++) {
    lv_obj_t * f = col(fr, 1);
    lv_obj_set_flex_grow(f, 1);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(f, 5, 0);
    lv_obj_set_style_radius(f, 8, 0);
    lv_obj_set_style_bg_color(f, C(HX_S2), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(f, lv_color_white(), 0);
    lv_obj_set_style_border_opa(f, OPA_BORDER, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    eyebrow(f, FL[k]);
    W.fcV[k] = label(f, &vn_inter_15b, HX_TEXT, "\xE2\x80\x94");
  }

  // Lecture opérationnelle (.vn-dcard)
  lv_obj_t * ins = card(body, 8);
  lv_obj_set_width(ins, BW);
  lv_obj_set_flex_flow(ins, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ins, 5, 0);
  lv_obj_t * ih = row(ins, 8);
  lv_obj_set_width(ih, LV_PCT(100));
  W.insFlash = box(ih);
  lv_obj_set_size(W.insFlash, 26, 26);
  lv_obj_set_style_radius(W.insFlash, 8, 0);
  lv_obj_set_style_bg_opa(W.insFlash, LV_OPA_COVER, 0);
  W.insIc = lv_img_create(W.insFlash);
  lv_obj_center(W.insIc);
  W.insTitle = label(ih, &vn_inter_13s, HX_TEXT, "");
  lv_obj_set_flex_grow(W.insTitle, 1);
  W.insConf = label(ih, &vn_mono_10, HX_TEXT3, "");
  W.insWhy = label(ins, &vn_inter_12, HX_TEXT2, "");
  lv_label_set_long_mode(W.insWhy, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(W.insWhy, BW - 18);
  W.insNeed = badge(ins, "", T_GOOD);

  // Détails
  lv_obj_t * det = col(body, 3);
  W.lastMove = label(det, &vn_inter_12, HX_TEXT2, "");
  W.lastRep = label(det, &vn_inter_12, HX_TEXT3, "");
  W.addr = label(det, &vn_inter_11, HX_TEXT3, "");
  lv_label_set_long_mode(W.addr, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(W.addr, BW);
  lv_obj_t * btns = row(body, 8);
  lv_obj_t * bm = button(btns, "Voir sur la carte", &ic_map_16, B_SECONDARY, true);
  lv_obj_add_event_cb(bm, [](lv_event_t *) { int s = W.st; sheetClose(); mapFocus(s); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * bt = button(btns, "Transferts", &ic_truck_16, B_TONAL, true);
  lv_obj_add_event_cb(bt, [](lv_event_t *) { openTransfers(); }, LV_EVENT_CLICKED, nullptr);
  fill();
}

void stationSheetRefresh() {
  if (W.st < 0) return;
  if (sheetToken() != W.token || W.token < 0) { W.st = -1; return; }
  fill();
}

} // namespace ui
