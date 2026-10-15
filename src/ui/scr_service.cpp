// ── Écran « Service » : ce que regarde un directeur de service ───────
// Disponibilité (jour, 30 jours, parts nommées), ruptures et saturations par jour, comparaisons d'une
// semaine à l'autre, problèmes récurrents, pires et meilleures stations, état de la carte. Les règles
// sont dans core/service (testées par sim/test_core) ; cet écran ne fait que les dire.
#include "ui_internal.h"
#include "widgets/vn_chart.h"
#include "../core/service.h"
#include "../core/ops.h"
#include "../net/net.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>

using vn::g_model;
using vn::g_set;

namespace ui {

static const int NPANE = 4;
static const char * PANES[NPANE] = { "Synth\xC3\xA8se", "R\xC3\xA9" "currents", "Classement", "La carte" };
static const lv_coord_t PANE_Y = 42, PANE_W = CONTENT_W - 16;
static const int NDAYS_CHART = 30, NRANK = 5, MAX_REC = 24;

static lv_obj_t * s_seg, * s_pane[NPANE];
static int s_paneIdx = 0;
// Synthèse
static lv_obj_t * s_todayPct, * s_todaySub, * s_todayEb, * s_monthPct, * s_monthSub, * s_monthEb;
static lv_obj_t * s_meter, * s_parts[4], * s_chart, * s_chartSub, * s_cmp[3];
static float s_evE[NDAYS_CHART], s_evF[NDAYS_CHART];
// Récurrents
static lv_obj_t * s_recRule, * s_recScope, * s_recList, * s_recEmpty, * s_recEmptySub;
struct RRow { lv_obj_t * obj, * flash, * ic, * name, * why; };
static RRow s_rrows[MAX_REC];
static int s_nRRows = 0;
static vn::Recur s_rec[MAX_REC];
static int s_nRec = 0;
// Classement
static lv_obj_t * s_rankRule, * s_rankWorst, * s_rankBest;
struct KRow { lv_obj_t * obj, * name, * why, * val; };
static KRow s_worst[NRANK], s_best[NRANK];
// La carte
static lv_obj_t * s_cardSub[6], * s_cardBadge[6];
static uint32_t s_seenDays = 0xFFFFFFFF;
static int32_t s_seenToday = 0;
static int s_seenDone = -1;

// ── Petits blocs ─────────────────────────────────────────────────────
static lv_obj_t * paneCol(lv_obj_t * root) {
  lv_obj_t * p = col(root, 8);
  lv_obj_set_pos(p, 8, PANE_Y);
  lv_obj_set_size(p, PANE_W, CONTENT_H - PANE_Y);
  lv_obj_set_style_pad_bottom(p, 8, 0);
  lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(p, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_width(p, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(p, C(HX_TEXT3), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(p, 120, LV_PART_SCROLLBAR);
  return p;
}
static lv_obj_t * wrapLabel(lv_obj_t * parent, const lv_font_t * f, uint32_t hex, lv_coord_t w) {
  lv_obj_t * l = label(parent, f, hex, "");
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w);
  return l;
}
static lv_obj_t * cardCol(lv_obj_t * parent, const char * eb) {
  lv_obj_t * c = card(parent, 8);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 3, 0);
  if (eb) eyebrow(c, eb);
  return c;
}
static void tile(lv_obj_t * parent, lv_obj_t ** eb, lv_obj_t ** pct, lv_obj_t ** sub) {
  lv_obj_t * c = card(parent, 8);
  lv_obj_set_size(c, (PANE_W - 8) / 2, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 1, 0);
  *eb = eyebrow(c, "");
  *pct = label(c, &vn_inter_20b, HX_TEXT, "\xE2\x80\x94");
  *sub = wrapLabel(c, &vn_inter_11, HX_TEXT2, (PANE_W - 8) / 2 - 16);
}

// Taux en dixièmes de point (‑1 si dénominateur nul)
static int rate10(uint32_t num, uint32_t den) { return den ? (int)(((uint64_t)num * 1000 + den / 2) / den) : -1; }
static void ptDiff(char * out, size_t n, int a, int b) {     // « +0,8 pt » / « −1,2 pt » / « = »
  if (a < 0 || b < 0) { snprintf(out, n, "\xE2\x80\x94"); return; }
  int d = a - b;
  if (!d) { snprintf(out, n, "stable"); return; }
  snprintf(out, n, "%s%d,%d\xC2\xA0pt", d > 0 ? "+" : "\xE2\x88\x92", (d < 0 ? -d : d) / 10, (d < 0 ? -d : d) % 10);
}

// ── Synthèse ─────────────────────────────────────────────────────────
static void buildSynth(lv_obj_t * p) {
  lv_obj_t * tiles = row(p, 8);
  lv_obj_set_width(tiles, LV_PCT(100));
  lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  tile(tiles, &s_todayEb, &s_todayPct, &s_todaySub);
  tile(tiles, &s_monthEb, &s_monthPct, &s_monthSub);

  lv_obj_t * c = cardCol(p, "R\xC3\x89PARTITION DU TEMPS OBSERV\xC3\x89 \xC2\xB7 30 JOURS \xC2\xB7 7 H\xE2\x80\x93" "22 H");
  // Barre empilée à largeurs exactes (le meter du thème répartit par flex_grow, arrondi grossier aux petites parts)
  s_meter = box(c);
  lv_obj_set_size(s_meter, PANE_W - 16, 6);
  lv_obj_set_style_radius(s_meter, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(s_meter, true, 0);
  lv_obj_set_style_bg_color(s_meter, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_meter, 15, 0);
  for (int k = 0; k < 4; k++) {
    lv_obj_t * seg = box(s_meter);
    lv_obj_set_height(seg, 6);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
  }
  static const uint32_t COL[4] = { HX_GOOD, HX_BAD, HX_TRANSIT, HX_CLOSED };
  for (int k = 0; k < 4; k++) {
    lv_obj_t * r = row(c, 6);
    dot(r, 6, C(COL[k]));
    s_parts[k] = label(r, &vn_inter_11, HX_TEXT2, "");
  }
  lv_obj_t * def = wrapLabel(c, &vn_inter_11, HX_TEXT3, PANE_W - 16);
  lv_label_set_text(def, "Disponible = ouverte, \xE2\x89\xA5\xC2\xA0" "1 v\xC3\xA9lo louable ET \xE2\x89\xA5\xC2\xA0" "1 borne libre. "
                         "Vide (on peut rendre, pas prendre) et pleine (l'inverse) ne sont PAS hors service. "
                         "Unit\xC3\xA9 : station \xC3\x97 5 min observ\xC3\xA9" "es, en heures.");

  lv_obj_t * cc = cardCol(p, "ENTR\xC3\x89" "ES EN RUPTURE ET EN SATURATION PAR JOUR \xC2\xB7 30 J");
  s_chart = chartCreate(cc, PANE_W - 16, 70);
  ChartData * d = chartData(s_chart);
  d->nx = NDAYS_CHART; d->ns = 2; d->endDot = false; d->padL = 26; d->padR = 6;
  d->s[0].y = s_evE; d->s[0].n = NDAYS_CHART; d->s[0].kind = SK_BARS; d->s[0].color = C(HX_BAD); d->s[0].opa = 220;
  d->s[1].y = s_evF; d->s[1].n = NDAYS_CHART; d->s[1].kind = SK_LINE; d->s[1].color = C(HX_TRANSIT); d->s[1].width = 2;
  s_chartSub = wrapLabel(cc, &vn_inter_11, HX_TEXT2, PANE_W - 16);

  lv_obj_t * k = cardCol(p, "COMPARAISONS");
  for (int i = 0; i < 3; i++) s_cmp[i] = wrapLabel(k, &vn_inter_11, HX_TEXT2, PANE_W - 16);
}

static void refreshSynth(int64_t t, int32_t today, int done) {
  const vn::DayBook & b = g_model.days;
  char x[320], y[48], z[48], w[32];
  // Aujourd'hui, créneaux complets
  vn::SvcCount td;
  vn::svcSum(b, today, today, done, td);
  if (done > 0) snprintf(x, sizeof(x), "AUJOURD'HUI \xC2\xB7 7 H\xE2\x80\x93%d H", vn::DAY_H0 + done);
  else snprintf(x, sizeof(x), "AUJOURD'HUI");
  lv_label_set_text(s_todayEb, x);
  vn::svcPct(y, sizeof(y), td.avail(), td.obs);
  lv_label_set_text(s_todayPct, y);
  if (!done) snprintf(x, sizeof(x), "Aucune heure de service compl\xC3\xA8te (avant 8\xC2\xA0h)");
  else if (!td.obs) snprintf(x, sizeof(x), "Aucun relev\xC3\xA9 sur ces heures");
  else { vn::svcHours(y, sizeof(y), td.avail()); vn::svcHours(z, sizeof(z), td.obs); snprintf(x, sizeof(x), "%s disponibles sur %s observ\xC3\xA9" "es", y, z); }
  lv_label_set_text(s_todaySub, x);
  // 30 jours complets (hier et les 29 jours d'avant)
  vn::SvcCount mo;
  vn::svcSum(b, today - 30, today - 1, vn::DAY_NH, mo);
  snprintf(x, sizeof(x), "30 JOURS \xC2\xB7 %d RELEV\xC3\x89%s", mo.days, mo.days > 1 ? "S" : "");
  lv_label_set_text(s_monthEb, x);
  vn::svcPct(y, sizeof(y), mo.avail(), mo.obs);
  lv_label_set_text(s_monthPct, y);
  if (!mo.obs) snprintf(x, sizeof(x), "Pas encore de journ\xC3\xA9" "e compl\xC3\xA8te r\xC3\xA9sum\xC3\xA9" "e");
  else {
    vn::svcHours(y, sizeof(y), mo.avail()); vn::svcHours(z, sizeof(z), mo.obs);
    vn::svcPct(w, sizeof(w), mo.slots, mo.slotsMax);
    snprintf(x, sizeof(x), "%s sur %s \xC2\xB7 %d j sur 30 relev\xC3\xA9s, %s des tranches", y, z, mo.days, w);
  }
  lv_label_set_text(s_monthSub, x);
  // Parts
  uint32_t part[4] = { mo.avail(), mo.empty, mo.full, mo.closed };
  static const char * PN[4] = { "Disponible", "Vide", "Pleine", "Hors service" };
  static const uint32_t COL[4] = { HX_GOOD, HX_BAD, HX_TRANSIT, HX_CLOSED };
  const lv_coord_t MW = PANE_W - 16;
  lv_coord_t xpos = 0;
  uint32_t cum = 0;
  for (int k = 0; k < 4; k++) {
    vn::svcPct(y, sizeof(y), part[k], mo.obs);
    vn::svcHours(z, sizeof(z), part[k]);
    snprintf(x, sizeof(x), "%s %s \xC2\xB7 %s", PN[k], y, z);
    lv_label_set_text(s_parts[k], x);
    // bornes cumulées arrondies : la somme des largeurs fait exactement la barre ; une part non nulle ≥ 1 px
    cum += part[k];
    lv_coord_t x1 = mo.obs ? (lv_coord_t)((uint64_t)cum * MW / mo.obs) : 0;
    if (part[k] && x1 <= xpos) x1 = xpos + 1;
    if (x1 > MW) x1 = MW;
    lv_obj_t * seg = lv_obj_get_child(s_meter, k);
    lv_obj_set_pos(seg, xpos, 0);
    lv_obj_set_width(seg, x1 - xpos);
    lv_obj_set_style_bg_color(seg, C(COL[k]), 0);
    xpos = x1;
  }
  // Courbe 30 jours : entrées par jour (jours absents : trou)
  float mx = 4;
  uint32_t sumE = 0, sumF = 0;
  for (int i = 0; i < NDAYS_CHART; i++) {
    int32_t day = today - NDAYS_CHART + 1 + i;
    int k = b.find(day);
    const vn::DayNet * d = k >= 0 ? b.dayAt(k) : nullptr;
    if (!d) { s_evE[i] = s_evF[i] = NAN; continue; }
    uint32_t e = 0, f = 0;
    for (int h = 0; h < vn::DAY_NH; h++) { e += d->evEmpty[h]; f += d->evFull[h]; }
    s_evE[i] = (float)e; s_evF[i] = (float)f;
    if (day < today) { sumE += e; sumF += f; }
    if (e > mx) mx = (float)e;
    if (f > mx) mx = (float)f;
  }
  ChartData * cd = chartData(s_chart);
  cd->ymin = 0; cd->ymax = ceilf(mx / 10) * 10;
  snprintf(cd->ylab[0], sizeof(cd->ylab[0]), "%d", (int)cd->ymax);
  snprintf(cd->ylab[1], sizeof(cd->ylab[1]), "0");
  cd->nxl = 3; cd->xlPos[0] = 0; cd->xlPos[1] = NDAYS_CHART / 2; cd->xlPos[2] = NDAYS_CHART - 1;
  for (int q = 0; q < 3; q++) {
    int dd, mm, yy;
    vn::dayCivil(today - NDAYS_CHART + 1 + cd->xlPos[q], yy, mm, dd);
    if (q == 2) snprintf(cd->xl[q], sizeof(cd->xl[q]), "auj.");
    else snprintf(cd->xl[q], sizeof(cd->xl[q]), "%02d/%02d", dd, mm);
  }
  chartInvalidate(s_chart);
  snprintf(x, sizeof(x), "Barres rouges : ruptures (passage \xC3\xA0 vide) \xC2\xB7 ligne violette : saturations (passage \xC3\xA0 plein), "
           "constat\xC3\xA9" "es entre deux relev\xC3\xA9s de 5 min. 30 j hors aujourd'hui : %u ruptures, %u saturations.", (unsigned)sumE, (unsigned)sumF);
  lv_label_set_text(s_chartSub, x);
  // Comparaisons : 7 derniers jours contre les 7 d'avant ; aujourd'hui contre le même jour il y a 7 jours, mêmes heures
  vn::SvcCount w1, w0, ld;
  vn::svcSum(b, today - 7, today - 1, vn::DAY_NH, w1);
  vn::svcSum(b, today - 14, today - 8, vn::DAY_NH, w0);
  vn::svcPct(y, sizeof(y), w1.avail(), w1.obs);
  vn::svcPct(z, sizeof(z), w0.avail(), w0.obs);
  ptDiff(w, sizeof(w), rate10(w1.avail(), w1.obs), rate10(w0.avail(), w0.obs));
  snprintf(x, sizeof(x), "Disponibilit\xC3\xA9 7 derniers jours %s (%d j) \xC2\xB7 7 jours d'avant %s (%d j) \xC2\xB7 %s",
           y, w1.days, z, w0.days, w);
  lv_label_set_text(s_cmp[0], x);
  snprintf(x, sizeof(x), "Ruptures 7 j : %u contre %u \xC2\xB7 saturations : %u contre %u",
           (unsigned)w1.evEmpty, (unsigned)w0.evEmpty, (unsigned)w1.evFull, (unsigned)w0.evFull);
  lv_label_set_text(s_cmp[1], x);
  vn::svcSum(b, today - 7, today - 7, done, ld);
  char dl[24];
  vn::svcDayLabel(today - 7, dl, sizeof(dl));
  if (!done) snprintf(x, sizeof(x), "Aujourd'hui contre %s : \xC3\xA0 partir de 8\xC2\xA0h (heures compl\xC3\xA8tes)", dl);
  else {
    vn::svcPct(y, sizeof(y), td.avail(), td.obs);
    vn::svcPct(z, sizeof(z), ld.avail(), ld.obs);
    ptDiff(w, sizeof(w), rate10(td.avail(), td.obs), rate10(ld.avail(), ld.obs));
    snprintf(x, sizeof(x), "Aujourd'hui 7\xC2\xA0h\xE2\x80\x93%d\xC2\xA0h %s \xC2\xB7 %s m\xC3\xAAmes heures %s \xC2\xB7 %s \xC2\xB7 ruptures %u contre %u",
             vn::DAY_H0 + done, y, dl, ld.days ? z : "non relev\xC3\xA9", w, (unsigned)td.evEmpty, (unsigned)ld.evEmpty);
  }
  lv_label_set_text(s_cmp[2], x);
}

// ── Récurrents ───────────────────────────────────────────────────────
static void recClick(lv_event_t * e) {
  int st = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (st >= 0 && st < g_model.n) openStation(st);
}
static RRow & makeRRow(int i) {
  RRow & r = s_rrows[i];
  r.obj = row(s_recList, 10);
  lv_obj_set_size(r.obj, LV_PCT(100), 44);
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
  lv_obj_add_event_cb(r.obj, recClick, LV_EVENT_CLICKED, nullptr);
  r.flash = box(r.obj);
  lv_obj_set_size(r.flash, 28, 28);
  lv_obj_set_style_radius(r.flash, 8, 0);
  lv_obj_set_style_bg_opa(r.flash, 36, 0);
  r.ic = lv_img_create(r.flash);
  lv_obj_center(r.ic);
  lv_obj_t * c = col(r.obj, 1);
  lv_obj_set_flex_grow(c, 1);
  r.name = label(c, &vn_inter_13s, HX_TEXT, "");
  oneLine(r.name, 330);
  r.why = label(c, &vn_inter_12, HX_TEXT2, "");
  oneLine(r.why, 330);
  return r;
}

static void buildRecur(lv_obj_t * p) {
  lv_obj_t * c = cardCol(p, "LA R\xC3\x88GLE");
  s_recRule = wrapLabel(c, &vn_inter_11, HX_TEXT2, PANE_W - 16);
  s_recScope = wrapLabel(c, &vn_inter_11, HX_TEXT3, PANE_W - 16);
  s_recList = card(p, 0);
  lv_obj_set_width(s_recList, LV_PCT(100));
  lv_obj_set_height(s_recList, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_recList, LV_FLEX_FLOW_COLUMN);
  s_recEmpty = col(s_recList, 4);
  lv_obj_set_width(s_recEmpty, LV_PCT(100));
  lv_obj_set_style_pad_ver(s_recEmpty, 14, 0);
  lv_obj_set_flex_align(s_recEmpty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  label(s_recEmpty, &vn_inter_13s, HX_TEXT, "Aucun probl\xC3\xA8me r\xC3\xA9" "current");
  s_recEmptySub = wrapLabel(s_recEmpty, &vn_inter_12, HX_TEXT2, PANE_W - 40);
  lv_obj_set_style_text_align(s_recEmptySub, LV_TEXT_ALIGN_CENTER, 0);
}

static void refreshRecur(int32_t today, bool recompute) {
  char x[200];
  vn::svcRecurRule(vn::g_recur, x, sizeof(x));
  lv_label_set_text(s_recRule, x);
  int wd, we;
  vn::svcRecurDays(g_model.days, today, vn::g_recur, wd, we);
  snprintf(x, sizeof(x), "Examin\xC3\xA9s : %d jours ouvrables et %d de week-end ou f\xC3\xA9ri\xC3\xA9s relev\xC3\xA9s sur les %d derniers jours. "
           "Un f\xC3\xA9ri\xC3\xA9 compte avec le week-end, jamais comme un jour normal. Incident du jour : \xC3\xA9" "cran Alertes.",
           wd, we, vn::g_recur.lookback);
  lv_label_set_text(s_recScope, x);
  if (recompute) s_nRec = vn::svcRecurrent(g_model.days, g_model, today, s_rec, MAX_REC);
  int shown = 0;
  for (int k = 0; k < s_nRec; k++) {
    const vn::Recur & r = s_rec[k];
    if (r.st < 0 || r.st >= g_model.n) continue;
    if (shown >= s_nRRows) makeRRow(s_nRRows++);
    RRow & w = s_rrows[shown++];
    lv_obj_clear_flag(w.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(w.obj, (void *)(intptr_t)r.st);
    Tone t = r.kind == vn::RK_EMPTY ? T_BAD : r.kind == vn::RK_FULL ? T_TRANSIT : T_ELEC;
    lv_obj_set_style_bg_color(w.flash, toneColor(t), 0);
    lv_img_set_src(w.ic, r.kind == vn::RK_EMPTY ? &ic_alert_16 : r.kind == vn::RK_FULL ? &ic_dock_16 : &ic_activity_16);
    iconColor(w.ic, toneColor(t));
    lv_label_set_text(w.name, g_model.st[r.st].name);
    vn::svcRecurText(r, x, sizeof(x));
    lv_label_set_text(w.why, x);
  }
  for (int i = shown; i < s_nRRows; i++) lv_obj_add_flag(s_rrows[i].obj, LV_OBJ_FLAG_HIDDEN);
  if (shown) lv_obj_add_flag(s_recEmpty, LV_OBJ_FLAG_HIDDEN);
  else {
    lv_obj_clear_flag(s_recEmpty, LV_OBJ_FLAG_HIDDEN);
    if (wd < vn::g_recur.wdNeed && we < vn::g_recur.weNeed)
      snprintf(x, sizeof(x), "Trop t\xC3\xB4t pour conclure : il faut au moins %d jours ouvrables (ou %d de week-end) relev\xC3\xA9s, "
               "la carte en a %d (et %d).", vn::g_recur.wdNeed, vn::g_recur.weNeed, wd, we);
    else snprintf(x, sizeof(x), "Aucune station n'atteint le seuil sur les jours examin\xC3\xA9s.");
    lv_label_set_text(s_recEmptySub, x);
  }
  char seg[32];
  snprintf(seg, sizeof(seg), "R\xC3\xA9" "currents %d", shown);
  segLabel(s_seg, 1, seg);
}

// ── Classement ───────────────────────────────────────────────────────
static void rankClick(lv_event_t * e) { recClick(e); }
static void makeKRows(lv_obj_t * list, KRow * rows) {
  for (int i = 0; i < NRANK; i++) {
    KRow & r = rows[i];
    r.obj = row(list, 8);
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
    lv_obj_add_event_cb(r.obj, rankClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * c = col(r.obj, 0);
    lv_obj_set_flex_grow(c, 1);
    r.name = label(c, &vn_inter_13s, HX_TEXT, "");
    oneLine(r.name, 290);
    r.why = label(c, &vn_inter_11, HX_TEXT2, "");
    oneLine(r.why, 290);
    r.val = label(r.obj, &vn_mono_12b, HX_TEXT, "");
  }
}
static lv_obj_t * rankList(lv_obj_t * p, const char * eb, KRow * rows) {
  lv_obj_t * t = eyebrow(p, eb);
  lv_obj_set_style_pad_left(t, 2, 0);
  lv_obj_t * l = card(p, 0);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_height(l, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  makeKRows(l, rows);
  return l;
}
static void buildRank(lv_obj_t * p) {
  lv_obj_t * c = cardCol(p, "LA R\xC3\x88GLE");
  s_rankRule = wrapLabel(c, &vn_inter_11, HX_TEXT2, PANE_W - 16);
  s_rankWorst = rankList(p, "LES 5 PLUS SOUVENT INDISPONIBLES", s_worst);
  s_rankBest = rankList(p, "LES 5 PLUS SOUVENT DISPONIBLES", s_best);
}
static void fillKRows(KRow * rows, const vn::StRank * r, int n, bool worst) {
  char x[128], a[24], b2[24], c2[24];
  for (int i = 0; i < NRANK; i++) {
    if (i >= n) { lv_obj_add_flag(rows[i].obj, LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(rows[i].obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(rows[i].obj, (void *)(intptr_t)r[i].st);
    lv_label_set_text(rows[i].name, g_model.st[r[i].st].name);
    vn::svcHours(a, sizeof(a), r[i].empty);
    vn::svcHours(b2, sizeof(b2), r[i].full);
    vn::svcHours(c2, sizeof(c2), r[i].closed);
    char o[24];
    vn::svcHours(o, sizeof(o), r[i].obs);
    snprintf(x, sizeof(x), "vide %s \xC2\xB7 pleine %s \xC2\xB7 HS %s sur %s", a, b2, c2, o);
    lv_label_set_text(rows[i].why, x);
    vn::svcPct(a, sizeof(a), r[i].closed + r[i].empty + r[i].full, r[i].obs);
    lv_label_set_text(rows[i].val, a);
    lv_obj_set_style_text_color(rows[i].val, C(worst ? HX_BAD : HX_GOOD), 0);
  }
}
static void refreshRank(int32_t today) {
  vn::StRank * r = (vn::StRank *)plat::psAlloc(sizeof(vn::StRank) * VN_MAX_ST);   // PSRAM, le temps du rafraîchissement
  if (!r) return;
  int n = vn::svcRank(g_model.days, g_model, today - 30, today - 1, r, VN_MAX_ST);
  char x[220];
  vn::SvcCount mo;
  vn::svcSum(g_model.days, today - 30, today - 1, vn::DAY_NH, mo);
  snprintf(x, sizeof(x), "Part indisponible = (vide + pleine + hors service) \xC3\xB7 temps observ\xC3\xA9, 7\xC2\xA0h\xE2\x80\x93" "22\xC2\xA0h, "
           "%d jours complets. Class\xC3\xA9" "es : %d stations observ\xC3\xA9" "es au moins la moiti\xC3\xA9 du temps de la mieux observ\xC3\xA9" "e.",
           mo.days, n);
  lv_label_set_text(s_rankRule, x);
  vn::svcSortWorst(r, n);
  fillKRows(s_worst, r, n, true);
  vn::svcSortBest(r, n);
  fillKRows(s_best, r, n, false);
  plat::psFree(r);
}

// ── La carte : les chiffres sont-ils frais ? ─────────────────────────
static const char * CARD_T[6] = { "Flux des stations", "M\xC3\xA9t\xC3\xA9o", "Wi-Fi", "M\xC3\xA9moire",
                                  "Apprentissage du profil", "R\xC3\xA9sum\xC3\xA9s par jour" };
static void buildCard(lv_obj_t * p) {
  static const lv_img_dsc_t * IC[6] = { &ic_refresh_16, &ic_cloud_16, &ic_wifi_16, &ic_cpu_16, &ic_activity_16, &ic_clock_16 };
  lv_obj_t * c = card(p, 0);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  for (int k = 0; k < 6; k++) {
    lv_obj_t * r = row(c, 10);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_style_min_height(r, 40, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_ver(r, 5, 0);
    if (k) {
      lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
      lv_obj_set_style_border_color(r, lv_color_white(), 0);
      lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
      lv_obj_set_style_border_width(r, 1, 0);
    }
    icon(r, IC[k], HX_TEXT2);
    lv_obj_t * cc = col(r, 1);
    lv_obj_set_flex_grow(cc, 1);
    label(cc, &vn_inter_13s, HX_TEXT, CARD_T[k]);
    s_cardSub[k] = wrapLabel(cc, &vn_inter_11, HX_TEXT2, 270);
    s_cardBadge[k] = badge(r, "", T_NEUTRAL);
  }
}
static void refreshCard(int64_t t, int32_t today) {
  char x[320], a[32], b2[32];
  net::Status ns = net::status();
  // 1. station_status : âge et seuil (même règle que le bandeau et la pastille LIVE)
  int thr = vn::staleThresholdS(g_set);
  bool stale = vn::isStale(g_model.lastUpdate, t, thr);
  fmtAgo(a, sizeof(a), g_model.lastUpdate, t);
  fmtDur(b2, sizeof(b2), thr / 60);
  if (!g_model.lastUpdate) snprintf(x, sizeof(x), "Aucun relev\xC3\xA9 re\xC3\xA7u depuis le d\xC3\xA9marrage");
  else snprintf(x, sizeof(x), "Dernier relev\xC3\xA9 re\xC3\xA7u %s \xC2\xB7 frais tant qu'il a moins de %s \xC2\xB7 toutes les %d s", a, b2, g_set.refreshSec);
  lv_label_set_text(s_cardSub[0], x);
  badgeSet(s_cardBadge[0], !g_model.lastUpdate ? "AUCUN" : stale ? "VIEUX" : "FRAIS", !g_model.lastUpdate || stale ? T_WARN : T_GOOD);
  // 2. Météo
  int wthr = vn::wxStaleThresholdS();
  bool wst = !g_model.wx.valid || vn::isStale(g_model.wx.fetchedAt, t, wthr);
  fmtDur(b2, sizeof(b2), wthr / 60);
  if (!g_model.wx.valid) snprintf(x, sizeof(x), "Open-Meteo n'a pas encore r\xC3\xA9pondu \xC2\xB7 la pr\xC3\xA9vision s'en passe");
  else { fmtAgo(a, sizeof(a), g_model.wx.fetchedAt, t); snprintf(x, sizeof(x), "Re\xC3\xA7ue %s \xC2\xB7 seuil %s", a, b2); }
  lv_label_set_text(s_cardSub[1], x);
  badgeSet(s_cardBadge[1], !g_model.wx.valid ? "AUCUNE" : wst ? "VIEILLE" : "FRA\xC3\x8E" "CHE", wst ? T_WARN : T_GOOD);
  // 3. Wi-Fi
  bool up = ns.wifi == net::W_CONNECTED;
  if (up) snprintf(x, sizeof(x), "%s \xC2\xB7 %d dBm \xC2\xB7 %s", ns.ssid, ns.rssi, ns.rssi >= -67 ? "bon signal" : ns.rssi >= -78 ? "signal moyen" : "signal faible");
  else snprintf(x, sizeof(x), "D\xC3\xA9" "connect\xC3\xA9 \xE2\x80\x94 les chiffres s'arr\xC3\xAAtent au dernier relev\xC3\xA9");
  lv_label_set_text(s_cardSub[2], x);
  badgeSet(s_cardBadge[2], up ? (ns.rssi >= -78 ? "OK" : "FAIBLE") : "HORS LIGNE", up ? (ns.rssi >= -78 ? T_GOOD : T_WARN) : T_BAD);
  // 4. Mémoire
  plat::SysInfo si;
  plat::sysInfo(si);
  fmtInt(a, sizeof(a), (int)(si.heapFree / 1024));
  fmtInt(b2, sizeof(b2), (int)(si.heapMin / 1024));
  char c2[24], d2[24];
  fmtInt(c2, sizeof(c2), (int)(si.psramFree / 1024));
  fmtInt(d2, sizeof(d2), (int)(si.fsUsed / 1024));
  snprintf(x, sizeof(x), "Tas libre %s ko (au plus bas %s ko) \xC2\xB7 PSRAM libre %s ko \xC2\xB7 flash utilis\xC3\xA9" "e %s ko", a, b2, c2, d2);
  lv_label_set_text(s_cardSub[3], x);
  bool memLow = si.heapMin < 20 * 1024;
  badgeSet(s_cardBadge[3], memLow ? "BASSE" : "OK", memLow ? T_WARN : T_GOOD);
  // 5. Apprentissage
  float lp = g_model.learnPct();
  snprintf(x, sizeof(x), "Profil hebdomadaire couvert \xC3\xA0 %.0f %% (cr\xC3\xA9neaux de 15 min de la semaine relev\xC3\xA9s \xE2\x89\xA5\xC2\xA0" "3 fois) "
           "\xC2\xB7 tant qu'il n'est pas appris, l'\xC3\xA9" "cart au profil se tait", lp);
  lv_label_set_text(s_cardSub[4], x);
  badgeSet(s_cardBadge[4], lp >= 90 ? "APPRIS" : "EN COURS", lp >= 90 ? T_GOOD : T_WARN);
  // 6. Résumés par jour : profondeur et couverture d'aujourd'hui
  const vn::DayBook & b = g_model.days;
  const vn::DayNet * old = b.count ? b.dayAt(b.count - 1) : nullptr;
  vn::SvcCount td;
  int done = vn::svcDoneHours(t);
  vn::svcSum(b, today, today, done, td);
  if (!old) snprintf(x, sizeof(x), "Aucun jour r\xC3\xA9sum\xC3\xA9 pour l'instant");
  else {
    vn::svcDayLabel(old->day, a, sizeof(a));
    if (done) {
      vn::svcPct(b2, sizeof(b2), td.slots, td.slotsMax);
      snprintf(x, sizeof(x), "%d jour%s gard\xC3\xA9%s (depuis %s, au plus %d) \xC2\xB7 aujourd'hui : %u tranches de 5 min relev\xC3\xA9" "es sur %u (%s)",
               b.count, b.count > 1 ? "s" : "", b.count > 1 ? "s" : "", a, VN_DAYS, (unsigned)td.slots, (unsigned)td.slotsMax, b2);
    } else snprintf(x, sizeof(x), "%d jour%s gard\xC3\xA9%s (depuis %s, au plus %d)", b.count, b.count > 1 ? "s" : "", b.count > 1 ? "s" : "", a, VN_DAYS);
  }
  lv_label_set_text(s_cardSub[5], x);
  bool cov = done && td.slotsMax && td.slots * 10 < td.slotsMax * 9;
  badgeSet(s_cardBadge[5], !b.count ? "VIDE" : cov ? "TROUS" : "COMPLET", !b.count || cov ? T_WARN : T_GOOD);
}

// ── Écran ────────────────────────────────────────────────────────────
static void showPane(int i) {
  s_paneIdx = i;
  for (int k = 0; k < NPANE; k++) {
    if (k == i) lv_obj_clear_flag(s_pane[k], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_pane[k], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_scroll_to_y(s_pane[i], 0, LV_ANIM_OFF);
}

static void refresh() {
  int64_t t = now();
  if (!g_model.days.net) return;
  const int32_t today = vn::localDay(t);
  const int done = vn::svcDoneHours(t);
  // Les calculs sur les résumés ne changent qu'à chaque tranche versée ou au changement d'heure
  bool data = g_model.days.version != s_seenDays || today != s_seenToday || done != s_seenDone;
  if (data) {
    s_seenDays = g_model.days.version; s_seenToday = today; s_seenDone = done;
    refreshSynth(t, today, done);
    refreshRecur(today, true);
    refreshRank(today);
  }
  refreshCard(t, today);
}

static void build(lv_obj_t * root) {
  s_seg = segmented(root, PANES, NPANE, 0, [](int i) { showPane(i); }, 30);
  lv_obj_set_pos(s_seg, 8, 6);
  lv_obj_set_width(s_seg, CONTENT_W - 16);
  segSet(s_seg, 0, false);
  for (int k = 0; k < NPANE; k++) s_pane[k] = paneCol(root);
  buildSynth(s_pane[0]);
  buildRecur(s_pane[1]);
  buildRank(s_pane[2]);
  buildCard(s_pane[3]);
  s_seenDays = 0xFFFFFFFF;
  showPane(0);
}

#ifdef VN_SIM
void servicePane(int i) { segSet(s_seg, i, false); showPane(i); }
lv_obj_t * servicePaneObj(int i) { return s_pane[i]; }
#endif

const ScreenDef SCR_SERVICE = { "Service", "VUE SERVICE", &ic_gauge_20, build, refresh, nullptr };

} // namespace ui
