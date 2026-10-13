// ── Écran « Réseau » : tableau de bord temps réel ────────────────────
#include "ui_internal.h"
#include "widgets/vn_chart.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

using vn::g_model;
using vn::g_set;

namespace ui {

// Partagé avec l'écran Alertes
void alertWhy(const vn::Alert & a, char * out, size_t n);
const lv_img_dsc_t * alertIcon16(uint8_t type);
Tone alertTone(const vn::Alert & a);
void needText(int need, char * out, size_t n);

static const int NX = VN_RING_SLOTS + VN_FC_STEPS - 1;   // 24 h passées + 2 h de prévision
static float s_real[VN_RING_SLOTS], s_fc[VN_FC_STEPS], s_moves[24], s_hist[10];
static lv_color_t s_histCol[10];

struct Kpi { lv_obj_t * value, * sub; };
static Kpi s_kpi[4];
static lv_obj_t * s_chart, * s_chartMeta;
static lv_obj_t * s_arc, * s_arcPct, * s_cnt[5];
static lv_obj_t * s_wxIc, * s_wxTemp, * s_wxLbl, * s_wxScore, * s_fcVal, * s_fcDelta, * s_fcIc;
static lv_obj_t * s_actChart, * s_actMeta, * s_histChart, * s_histMeta;
static lv_obj_t * s_prio, * s_prioRows[5], * s_modelLbl;

static lv_obj_t * kpiTile(lv_obj_t * parent, const lv_img_dsc_t * ic, const char * title, uint32_t col, Kpi & k) {
  lv_obj_t * c = card(parent, 7);
  lv_obj_set_flex_grow(c, 1);
  lv_obj_set_height(c, LV_PCT(100));
  lv_obj_t * head = row(c, 4);
  icon(head, ic, HX_TEXT3);
  lv_obj_t * eb = eyebrow(head, title);
  lv_obj_set_style_text_letter_space(eb, 0, 0);
  k.value = label(c, &vn_inter_26x, col, "\xE2\x80\x94");
  lv_obj_set_pos(k.value, 0, 12);
  k.sub = label(c, &vn_inter_11, HX_TEXT2, "");
  oneLine(k.sub, LV_PCT(100));
  lv_obj_align(k.sub, LV_ALIGN_BOTTOM_LEFT, 0, 1);
  return c;
}

static void openPrio(lv_event_t * e) {
  lv_obj_t * t = lv_event_get_current_target(e);
  int s = (int)(intptr_t)lv_obj_get_user_data(t);
  if (s >= 0) openStation(s);
}

static void build(lv_obj_t * root) {
  lv_obj_t * sc = col(root, 6);
  lv_obj_set_size(sc, CONTENT_W, CONTENT_H);
  lv_obj_set_style_pad_all(sc, 8, 0);
  lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_OFF);
  const lv_coord_t W = CONTENT_W - 16;

  // ── Indicateurs clés
  lv_obj_t * ra = row(sc, 6);
  lv_obj_set_size(ra, W, 74);
  kpiTile(ra, &ic_bike_12, "V\xC3\x89LOS DISPO", HX_GOOD, s_kpi[0]);
  kpiTile(ra, &ic_activity_12, "EN TRAJET", HX_ACCENT, s_kpi[1]);
  kpiTile(ra, &ic_dock_12, "BORNES LIB.", HX_TEXT, s_kpi[2]);
  kpiTile(ra, &ic_wrench_12, "V\xC3\x89LOS HS", HX_WARN, s_kpi[3]);

  // ── Disponibilité 24 h + prévision / équilibre
  lv_obj_t * rb = row(sc, 6);
  lv_obj_set_size(rb, W, 98);
  lv_obj_t * cc = card(rb, 7);
  lv_obj_set_size(cc, 240, 98);
  lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);              // → carte du réseau
  lv_obj_set_style_bg_color(cc, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_add_event_cb(cc, [](lv_event_t *) { showTab(TAB_MAP); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t * ch = row(cc, 6);
  lv_obj_set_width(ch, LV_PCT(100));
  eyebrow(ch, "V\xC3\x89LOS DISPONIBLES \xC2\xB7 24 H");
  s_chartMeta = label(ch, &vn_mono_10, HX_ACCENT, "");
  lv_obj_set_flex_grow(s_chartMeta, 1);
  lv_obj_set_style_text_align(s_chartMeta, LV_TEXT_ALIGN_RIGHT, 0);
  s_chart = chartCreate(cc, 224, 68);
  lv_obj_align(s_chart, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  ChartData * d = chartData(s_chart);
  d->nx = NX;
  d->ns = 2;
  d->s[0].y = s_real; d->s[0].n = VN_RING_SLOTS; d->s[0].x0 = 0;
  d->s[0].kind = SK_AREA; d->s[0].color = C(HX_GOOD); d->s[0].width = 2; d->s[0].fillOpa = 95;
  d->s[1].y = s_fc; d->s[1].n = VN_FC_STEPS; d->s[1].x0 = VN_RING_SLOTS - 1;
  d->s[1].kind = SK_DOTS; d->s[1].color = C(HX_ACCENT); d->s[1].width = 2;
  d->nowX = VN_RING_SLOTS - 1;
  d->padL = 30;

  lv_obj_t * hc = card(rb, 7);
  lv_obj_set_flex_grow(hc, 1);
  lv_obj_set_height(hc, 98);
  lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);              // → stations triées par remplissage
  lv_obj_set_style_bg_color(hc, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_add_event_cb(hc, [](lv_event_t *) { openStationsFiltered(2); }, LV_EVENT_CLICKED, nullptr);
  eyebrow(hc, "\xC3\x89QUILIBRE");
  s_arc = lv_arc_create(hc);
  lv_obj_set_size(s_arc, 56, 56);
  lv_obj_align(s_arc, LV_ALIGN_BOTTOM_LEFT, -2, 2);
  lv_arc_set_bg_angles(s_arc, 135, 45);
  lv_arc_set_range(s_arc, 0, 100);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_arc, 22, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, C(HX_GOOD), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(s_arc, 0, 0);
  s_arcPct = label(s_arc, &vn_inter_15b, HX_TEXT, "\xE2\x80\x94");
  lv_obj_align(s_arcPct, LV_ALIGN_CENTER, 0, -2);
  lv_obj_t * okl = label(s_arc, &vn_mono_10, HX_TEXT3, "OK");
  lv_obj_align(okl, LV_ALIGN_CENTER, 0, 13);
  lv_obj_t * lst = col(hc, 0);
  lv_obj_align(lst, LV_ALIGN_TOP_RIGHT, 0, 14);
  static const uint8_t ORDER[5] = { vn::ST_EMPTY, vn::ST_LOW, vn::ST_OK, vn::ST_HIGH, vn::ST_FULL };
  static const char * NAMES[5] = { "vides", "faibles", "\xC3\xA9quil.", "charg.", "pleines" };
  for (int i = 0; i < 5; i++) {
    lv_obj_t * r = row(lst, 4);
    dot(r, 6, statusColor(ORDER[i]));
    s_cnt[i] = label(r, &vn_mono_10, HX_TEXT, "0");
    lv_obj_set_width(s_cnt[i], 18);
    lv_obj_set_style_text_align(s_cnt[i], LV_TEXT_ALIGN_RIGHT, 0);
    label(r, &vn_mono_10, HX_TEXT3, NAMES[i]);
  }

  // ── Bandeau météo + prévision +1 h
  lv_obj_t * rc = card(sc, 0);
  lv_obj_set_size(rc, W, 36);
  lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(rc, 10, 0);
  lv_obj_set_style_pad_column(rc, 6, 0);
  s_wxIc = icon(rc, &ic_cloud_16, HX_TEXT2);
  s_wxTemp = label(rc, &vn_inter_13s, HX_TEXT, "--\xC2\xB0");
  s_wxLbl = label(rc, &vn_inter_12, HX_TEXT2, "M\xC3\xA9t\xC3\xA9o");
  label(rc, &vn_inter_12, HX_TEXT3, "\xC2\xB7");
  label(rc, &vn_inter_12, HX_TEXT2, "Indice v\xC3\xA9lo");
  s_wxScore = label(rc, &vn_inter_13s, HX_GOOD, "\xE2\x80\x94");
  lv_obj_t * sp = box(rc);
  lv_obj_set_flex_grow(sp, 1);
  eyebrow(rc, "PR\xC3\x89VISION +1 H");
  s_fcVal = label(rc, &vn_inter_13s, HX_TEXT, "\xE2\x80\x94");
  s_fcIc = icon(rc, &ic_trend_down_16, HX_TEXT2);
  s_fcDelta = label(rc, &vn_mono_10, HX_TEXT2, "");

  // ── Sous la ligne de flottaison : activité, remplissage, priorités
  lv_obj_t * ac = card(sc, 8);
  lv_obj_set_size(ac, W, 104);
  lv_obj_t * ah = row(ac, 6);
  lv_obj_set_width(ah, LV_PCT(100));
  eyebrow(ah, "ACTIVIT\xC3\x89 \xC2\xB7 MOUVEMENTS / HEURE");
  s_actMeta = label(ah, &vn_inter_11, HX_TEXT2, "");
  lv_obj_set_flex_grow(s_actMeta, 1);
  lv_obj_set_style_text_align(s_actMeta, LV_TEXT_ALIGN_RIGHT, 0);
  s_actChart = chartCreate(ac, W - 16, 74);
  lv_obj_align(s_actChart, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  d = chartData(s_actChart);
  d->nx = 24; d->ns = 1; d->endDot = false; d->padL = 26; d->padR = 8;
  d->s[0].y = s_moves; d->s[0].n = 24; d->s[0].kind = SK_BARS; d->s[0].color = C(HX_ACCENT); d->s[0].opa = 220;

  lv_obj_t * hc2 = card(sc, 8);
  lv_obj_set_size(hc2, W, 96);
  lv_obj_t * hh = row(hc2, 6);
  lv_obj_set_width(hh, LV_PCT(100));
  eyebrow(hh, "REMPLISSAGE DES STATIONS");
  s_histMeta = label(hh, &vn_inter_11, HX_TEXT2, "");
  lv_obj_set_flex_grow(s_histMeta, 1);
  lv_obj_set_style_text_align(s_histMeta, LV_TEXT_ALIGN_RIGHT, 0);
  s_histChart = chartCreate(hc2, W - 16, 66);
  lv_obj_align(s_histChart, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  d = chartData(s_histChart);
  d->nx = 10; d->ns = 1; d->endDot = false; d->padL = 26; d->padR = 14; d->gridY = false;
  d->s[0].y = s_hist; d->s[0].n = 10; d->s[0].kind = SK_BARS; d->s[0].barColors = s_histCol; d->s[0].opa = 230;
  static const char * XL[] = { "0 %", "50 %", "100 %" };
  d->nxl = 3; d->xlPos[0] = 0; d->xlPos[1] = 5; d->xlPos[2] = 9;
  for (int i = 0; i < 3; i++) snprintf(d->xl[i], sizeof(d->xl[i]), "%s", XL[i]);
  for (int i = 0; i < 10; i++) {
    float f = (i + 0.5f) / 10;
    s_histCol[i] = f < 0.1f ? C(HX_BAD) : f < 0.25f ? C(HX_WARN) : f < 0.75f ? C(HX_GOOD) : f < 0.9f ? C(HX_ELEC) : C(HX_TRANSIT);
  }

  s_prio = card(sc, 0);
  lv_obj_set_width(s_prio, W);
  lv_obj_set_flex_flow(s_prio, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_ver(s_prio, 6, 0);
  lv_obj_t * ph = row(s_prio, 6);
  lv_obj_set_style_pad_hor(ph, 10, 0);
  lv_obj_set_style_pad_bottom(ph, 2, 0);
  eyebrow(ph, "PRIORIT\xC3\x89S D'INTERVENTION");
  for (int i = 0; i < 5; i++) {
    lv_obj_t * r = row(s_prio, 8);
    lv_obj_set_size(r, LV_PCT(100), 30);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, C(HX_S2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(r, lv_color_white(), 0);
    lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_t * ic = lv_img_create(r);
    lv_img_set_src(ic, &ic_alert_16);
    lv_obj_t * nm = label(r, &vn_inter_13s, HX_TEXT, "");
    oneLine(nm, 130);
    lv_obj_t * why = label(r, &vn_inter_12, HX_TEXT2, "");
    oneLine(why, 10);
    lv_obj_set_flex_grow(why, 1);
    badge(r, "", T_ACCENT);
    lv_obj_set_user_data(r, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(r, openPrio, LV_EVENT_CLICKED, nullptr);
    s_prioRows[i] = r;
  }
  lv_obj_t * more = button(sc, "Toutes les alertes", &ic_bell_16, B_SECONDARY, true);
  lv_obj_add_event_cb(more, [](lv_event_t *) { showTab(TAB_ALERTS); }, LV_EVENT_CLICKED, nullptr);
  s_modelLbl = label(sc, &vn_inter_11, HX_TEXT3, "");
  lv_obj_set_width(s_modelLbl, W);
  lv_label_set_long_mode(s_modelLbl, LV_LABEL_LONG_WRAP);
}

static void refresh() {
  const vn::Totals & t = g_model.tot;
  int64_t nowT = now();
  char b[192], c2[32];

  // KPI
  fmtInt(b, sizeof(b), t.bikes); lv_label_set_text(s_kpi[0].value, b);
  // Sans une heure de relevés, « +0 sur 1 h » se lit « stable » alors que rien
  // n'est connu : on affiche l'absence de mesure plutôt qu'un zéro trompeur.
  if (t.deltaValid) { fmtSigned(c2, sizeof(c2), t.delta1h); snprintf(b, sizeof(b), "%s sur 1 h", c2); }
  else snprintf(b, sizeof(b), "recul de 1 h requis");
  lv_label_set_text(s_kpi[0].sub, b);
  // Idem pour les vélos en circulation : c'est une estimation tirée du parc, qui
  // ne se lit qu'après un creux de nuit. Tant qu'il n'a pas été observé, la
  // valeur vaut 0 par construction — l'afficher en gros serait un mensonge.
  if (t.inUseValid) fmtInt(b, sizeof(b), t.inUse);
  else snprintf(b, sizeof(b), "\xE2\x80\x94");
  lv_label_set_text(s_kpi[1].value, b);
  if (t.inUseValid) { fmtInt(c2, sizeof(c2), t.fleet); snprintf(b, sizeof(b), "parc \xE2\x89\x88 %s", c2); }
  else snprintf(b, sizeof(b), "estimation apr\xC3\xA8s une nuit");
  lv_label_set_text(s_kpi[1].sub, b);
  fmtInt(b, sizeof(b), t.docks); lv_label_set_text(s_kpi[2].value, b);
  snprintf(b, sizeof(b), "rempli \xC3\xA0 %.0f %%", t.fillPct); lv_label_set_text(s_kpi[2].sub, b);
  fmtInt(b, sizeof(b), t.bikesDis); lv_label_set_text(s_kpi[3].value, b);
  snprintf(b, sizeof(b), "%d borne%s HS", t.docksDis, t.docksDis > 1 ? "s" : ""); lv_label_set_text(s_kpi[3].sub, b);

  // Courbe 24 h
  ChartData * d = chartData(s_chart);
  float mn = 1e9f, mx = -1e9f;
  for (int x = 0; x < VN_RING_SLOTS; x++) s_real[x] = NAN;
  for (int bk = 0; bk < g_model.count; bk++) {
    int idx = g_model.ringIndex(bk);
    const vn::NetSample & ns = g_model.net[idx];
    if (ns.avail == 0xFFFF) continue;
    s_real[VN_RING_SLOTS - 1 - bk] = ns.avail;
  }
  if (g_model.lastUpdate) s_real[VN_RING_SLOTS - 1] = (float)t.bikes;
  for (int k = 0; k < VN_FC_STEPS; k++) s_fc[k] = g_model.netFc[k];
  for (float v : s_real) if (!isnan(v)) { mn = fminf(mn, v); mx = fmaxf(mx, v); }
  for (float v : s_fc) { mn = fminf(mn, v); mx = fmaxf(mx, v); }
  if (mn > mx) { mn = 0; mx = 1; }
  float pad = fmaxf(10.f, (mx - mn) * 0.12f);
  d->ymin = fmaxf(0, floorf((mn - pad) / 10) * 10);
  d->ymax = ceilf((mx + pad) / 10) * 10;
  snprintf(d->ylab[0], sizeof(d->ylab[0]), "%d", (int)d->ymax);
  snprintf(d->ylab[1], sizeof(d->ylab[1]), "%d", (int)d->ymin);
  // Étiquettes horaires (00 h / 06 h / 12 h / 18 h)
  d->nxl = 0;
  int64_t slot0 = (nowT - nowT % VN_RING_STEP_S) - (int64_t)(VN_RING_SLOTS - 1) * VN_RING_STEP_S;
  for (int x = 0; x < NX && d->nxl < 6; x++) {
    int64_t tt = slot0 + (int64_t)x * VN_RING_STEP_S;
    struct tm lt;
    plat::localTime(tt, &lt);
    if (lt.tm_min < 5 && lt.tm_hour % 6 == 0 && x > 6 && x < NX - 6) {
      d->xlPos[d->nxl] = x;
      snprintf(d->xl[d->nxl], sizeof(d->xl[0]), "%02dh", lt.tm_hour);
      d->nxl++;
    }
  }
  chartInvalidate(s_chart);
  snprintf(b, sizeof(b), "\xE2\x80\xA2\xE2\x80\xA2 +2 h");
  lv_label_set_text(s_chartMeta, b);

  // Équilibre
  lv_arc_set_value(s_arc, (int16_t)lroundf(t.balancePct));
  lv_obj_set_style_arc_color(s_arc, t.balancePct >= 70 ? C(HX_GOOD) : t.balancePct >= 50 ? C(HX_WARN) : C(HX_BAD), LV_PART_INDICATOR);
  snprintf(b, sizeof(b), "%.0f%%", t.balancePct);
  lv_label_set_text(s_arcPct, b);
  static const uint8_t ORDER[5] = { vn::ST_EMPTY, vn::ST_LOW, vn::ST_OK, vn::ST_HIGH, vn::ST_FULL };
  for (int i = 0; i < 5; i++) lv_label_set_text_fmt(s_cnt[i], "%d", t.byStatus[ORDER[i]]);

  // Météo + prévision
  const vn::Weather & w = g_model.wx;
  if (w.valid) {
    lv_img_set_src(s_wxIc, wxIcon16(w.code));
    snprintf(b, sizeof(b), "%.0f\xC2\xB0", w.temp); lv_label_set_text(s_wxTemp, b);
    lv_label_set_text(s_wxLbl, vn::wxLabel(w.code));
    fmtFloat1(b, sizeof(b), w.score);
    lv_label_set_text(s_wxScore, b);
    lv_obj_set_style_text_color(s_wxScore, w.score >= 7 ? C(HX_GOOD) : w.score >= 4 ? C(HX_WARN) : C(HX_BAD), 0);
  }
  int f60 = (int)lroundf(g_model.netFc[12]);
  int dlt = f60 - t.bikes;
  fmtInt(b, sizeof(b), f60); lv_label_set_text(s_fcVal, b);
  fmtSigned(c2, sizeof(c2), dlt); lv_label_set_text(s_fcDelta, c2);
  lv_img_set_src(s_fcIc, dlt < 0 ? &ic_trend_down_16 : &ic_trend_up_16);
  lv_color_t dc = dlt <= -15 ? C(HX_WARN) : dlt >= 15 ? C(HX_ELEC) : C(HX_TEXT2);
  iconColor(s_fcIc, dc);
  lv_obj_set_style_text_color(s_fcDelta, dc, 0);

  // Activité par heure (24 dernières heures)
  for (int h = 0; h < 24; h++) s_moves[h] = NAN;
  float mvMax = 10;
  for (int h = 0; h < 24; h++) {
    float sum = 0; int n = 0;
    for (int k = 0; k < 12; k++) {
      int idx = g_model.ringIndex(h * 12 + k);
      if (idx < 0) break;
      const vn::NetSample & ns = g_model.net[idx];
      if (ns.avail == 0xFFFF) continue;
      sum += ns.dep + ns.ret; n++;
    }
    if (n) { s_moves[23 - h] = sum * 12.f / n; mvMax = fmaxf(mvMax, s_moves[23 - h]); }
  }
  d = chartData(s_actChart);
  d->ymin = 0; d->ymax = ceilf(mvMax / 50) * 50;
  snprintf(d->ylab[0], sizeof(d->ylab[0]), "%d", (int)d->ymax);
  snprintf(d->ylab[1], sizeof(d->ylab[1]), "0");
  d->nxl = 0;
  for (int h = 0; h < 24; h++) {
    struct tm lt;
    plat::localTime(nowT - (int64_t)(23 - h) * 3600, &lt);
    if (lt.tm_hour % 6 == 0 && d->nxl < 6) { d->xlPos[d->nxl] = h; snprintf(d->xl[d->nxl], 8, "%02dh", lt.tm_hour); d->nxl++; }
  }
  chartInvalidate(s_actChart);
  char a1[16], a2[16];
  if (t.actValid) {
    fmtInt(a1, sizeof(a1), t.dep1h); fmtInt(a2, sizeof(a2), t.ret1h);
    snprintf(b, sizeof(b), "1 h : %s retraits \xC2\xB7 %s retours", a1, a2);
  } else snprintf(b, sizeof(b), "activit\xC3\xA9 : recul de 1 h requis");
  lv_label_set_text(s_actMeta, b);

  // Histogramme de remplissage
  for (int i = 0; i < 10; i++) s_hist[i] = 0;
  float hMax = 1;
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (!s.active || s.status == vn::ST_CLOSED || s.capEff() == 0) continue;
    int bin = (int)(s.fill() * 10);
    if (bin > 9) bin = 9;
    s_hist[bin]++;
  }
  for (float v : s_hist) hMax = fmaxf(hMax, v);
  d = chartData(s_histChart);
  d->ymin = 0; d->ymax = ceilf(hMax / 5) * 5;
  snprintf(d->ylab[0], sizeof(d->ylab[0]), "%d", (int)d->ymax);
  d->ylab[1][0] = 0;
  chartInvalidate(s_histChart);
  snprintf(b, sizeof(b), "%d stations ouvertes \xC2\xB7 %d ferm\xC3\xA9" "es", t.open, t.closed);
  lv_label_set_text(s_histMeta, b);

  // Priorités
  int shown = 0;
  for (int k = 0; k < g_model.nAlerts && shown < 5; k++) {
    const vn::Alert & a = g_model.alerts[k];
    if (a.sev < 1) continue;
    bool dup = false;   // une ligne par station
    for (int j = 0; j < shown; j++) if ((intptr_t)lv_obj_get_user_data(s_prioRows[j]) == a.st) dup = true;
    if (dup) continue;
    lv_obj_t * r = s_prioRows[shown++];
    lv_obj_clear_flag(r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(r, (void *)(intptr_t)a.st);
    lv_obj_t * ic = lv_obj_get_child(r, 0);
    lv_img_set_src(ic, alertIcon16(a.type));
    iconColor(ic, toneColor(alertTone(a)));
    lv_label_set_text(lv_obj_get_child(r, 1), g_model.st[a.st].name);
    alertWhy(a, b, sizeof(b));
    lv_label_set_text(lv_obj_get_child(r, 2), b);
    int nd = g_model.st[a.st].need;
    lv_obj_t * bd = lv_obj_get_child(r, 3);
    if (nd) { needText(nd, c2, sizeof(c2)); badgeSet(bd, c2, nd > 0 ? T_GOOD : T_TRANSIT); lv_obj_clear_flag(bd, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(bd, LV_OBJ_FLAG_HIDDEN);
  }
  for (int j = shown; j < 5; j++) lv_obj_add_flag(s_prioRows[j], LV_OBJ_FLAG_HIDDEN);
  if (!shown) {
    lv_obj_clear_flag(s_prioRows[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(s_prioRows[0], (void *)(intptr_t)-1);
    lv_img_set_src(lv_obj_get_child(s_prioRows[0], 0), &ic_check_16);
    iconColor(lv_obj_get_child(s_prioRows[0], 0), C(HX_GOOD));
    lv_label_set_text(lv_obj_get_child(s_prioRows[0], 1), "R\xC3\xA9seau \xC3\xA9quilibr\xC3\xA9");
    lv_label_set_text(lv_obj_get_child(s_prioRows[0], 2), "Aucune intervention prioritaire");
    lv_obj_add_flag(lv_obj_get_child(s_prioRows[0], 3), LV_OBJ_FLAG_HIDDEN);
  }

  // Modèle
  int days = g_model.learnSince ? (int)((nowT - g_model.learnSince) / 86400) : 0;
  char ag[32];
  fmtAgo(ag, sizeof(ag), g_model.lastUpdate, nowT);
  snprintf(b, sizeof(b), "Mise \xC3\xA0 jour %s \xC2\xB7 confiance pr\xC3\xA9vision %d %% \xC2\xB7 profil hebdo appris \xC3\xA0 %.0f %% (%d j)",
           ag, g_model.netConf, g_model.learnPct(), days);
  lv_label_set_text(s_modelLbl, b);
}

const ScreenDef SCR_DASH = { "R\xC3\xA9seau", "TABLEAU DE BORD", &ic_dashboard_20, build, refresh, nullptr };

} // namespace ui
