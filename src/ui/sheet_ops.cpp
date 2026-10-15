// ── Feuilles d'exploitation : provenance d'un chiffre, ingestion, prévision 24 h ─
// Rien n'est calculé ici : les valeurs, règles et horodatages viennent de
// core/ops (provenance, compteurs) et de Model::project (même règle que la
// prévision 2 h). L'écran ne fait que les mettre en forme.
#include "ui_internal.h"
#include "widgets/vn_chart.h"
#include "../core/ops.h"
#include "../net/net.h"
#include "../platform/platform.h"
#include "../config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

using vn::g_model;
using vn::g_set;

namespace ui {

static const lv_coord_t BW = CONTENT_W - 8 - 24;     // largeur utile d'une feuille
static const lv_coord_t KEY_W = 64;                    // colonne des intitulés
enum OpsKind { OK_NONE, OK_PROV, OK_INGEST, OK_FC24 };
static int s_kind = OK_NONE, s_token = -1, s_arg = -1;

// Ligne « intitulé | texte » ; renvoie le texte (retour à la ligne automatique)
static lv_obj_t * kv(lv_obj_t * parent, const char * key, lv_obj_t ** rowOut = nullptr) {
  lv_obj_t * r = row(parent, 8);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_t * k = eyebrow(r, key);
  lv_obj_set_width(k, KEY_W);
  lv_obj_set_style_pad_top(k, 2, 0);
  lv_obj_t * v = label(r, &vn_inter_12, HX_TEXT2, "");
  lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(v, BW - 16 - KEY_W - 8);
  if (rowOut) *rowOut = r;
  return v;
}

static lv_obj_t * divider(lv_obj_t * o) {
  lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(o, lv_color_white(), 0);
  lv_obj_set_style_border_opa(o, OPA_BORDER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  return o;
}

// « 14:29 (il y a 3 min) »
static void whenText(char * out, size_t n, int64_t at, int64_t t) {
  char c[8], a[32];
  fmtClock(c, sizeof(c), at);
  fmtAgo(a, sizeof(a), at, t);
  snprintf(out, n, "%s (%s)", c, a);
}

// ══ Provenance d'un chiffre ═════════════════════════════════════════
static const uint8_t GROUP_KPI[PG_COUNT][3] = {
  { vn::KPI_BIKES, vn::KPI_DELTA1H, 255 },
  { vn::KPI_INUSE, vn::KPI_FLEET, 255 },
  { vn::KPI_DOCKS, vn::KPI_FILL, 255 },
  { vn::KPI_BIKES_DIS, vn::KPI_DOCKS_DIS, 255 },
  { vn::KPI_TEMP, vn::KPI_WXSCORE, vn::KPI_FC60 },
};
static const char * GROUP_TITLE[PG_COUNT] = {
  "Provenance \xC2\xB7 v\xC3\xA9los disponibles", "Provenance \xC2\xB7 v\xC3\xA9los en trajet", "Provenance \xC2\xB7 bornes libres",
  "Provenance \xC2\xB7 hors service", "Provenance \xC2\xB7 m\xC3\xA9t\xC3\xA9o et pr\xC3\xA9vision",
};

struct ProvW { uint8_t kpi; lv_obj_t * value, * badge, * data, * whyRow, * why, * validRow, * valid, * validSt, * detailRow, * detail; };
static ProvW s_pw[3];
static int s_npw = 0;

// Valeur telle que la tuile l'affiche (mêmes arrondis), « — » si absente
static void provValue(const vn::Provenance & p, uint8_t kpi, char * out, size_t n) {
  if (!p.available) { snprintf(out, n, "\xE2\x80\x94"); return; }
  char v[24];
  if (kpi == vn::KPI_WXSCORE) fmtFloat1(v, sizeof(v), p.value);
  else if (p.isFloat) snprintf(v, sizeof(v), "%.0f", p.value);
  else if (kpi == vn::KPI_DELTA1H) fmtSigned(v, sizeof(v), (int)p.value);
  else fmtInt(v, sizeof(v), (int)p.value);
  if (!p.unit[0]) snprintf(out, n, "%s", v);
  else snprintf(out, n, "%s%s%s", v, p.unit[0] == '/' ? "" : "\xC2\xA0", p.unit);
}

static bool isMeteo(const vn::Provenance & p) { return p.feed && strstr(p.feed, "open-meteo"); }

static void provFill(ProvW & w) {
  vn::Provenance p;
  vn::provenance(g_model, w.kpi, p);
  int64_t t = now();
  char b[200], x[48], y[48];
  provValue(p, w.kpi, b, sizeof(b));
  lv_label_set_text(w.value, b);
  lv_obj_set_style_text_color(w.value, C(p.available ? HX_TEXT : HX_TEXT3), 0);
  if (p.estimate) badgeSet(w.badge, "ESTIMATION", T_WARN);
  else if (p.kind == vn::SRC_FEED) badgeSet(w.badge, "MESURE", T_GOOD);
  else badgeSet(w.badge, "CALCUL", T_ELEC);
  // Âge de la donnée : horodatage du flux et réception par la carte
  if (isMeteo(p)) {
    if (p.recvAt) { whenText(x, sizeof(x), p.recvAt, t); snprintf(b, sizeof(b), "Re\xC3\xA7ue d'Open-Meteo \xC3\xA0 %s.", x); }
    else snprintf(b, sizeof(b), "Aucune r\xC3\xA9ponse d'Open-Meteo depuis le d\xC3\xA9marrage.");
  } else {
    if (p.recvAt) {
      whenText(y, sizeof(y), p.recvAt, t);
      if (p.feedAt) { whenText(x, sizeof(x), p.feedAt, t); snprintf(b, sizeof(b), "last_updated du flux : %s \xC2\xB7 re\xC3\xA7u par la carte : %s.", x, y); }
      else snprintf(b, sizeof(b), "Re\xC3\xA7u par la carte %s (flux sans last_updated).", y);
    } else snprintf(b, sizeof(b), "Aucun relev\xC3\xA9 station_status depuis le d\xC3\xA9marrage.");
  }
  lv_label_set_text(w.data, b);
  if (p.available) lv_obj_add_flag(w.whyRow, LV_OBJ_FLAG_HIDDEN);
  else { lv_label_set_text(w.why, p.missing ? p.missing : "donn\xC3\xA9" "e absente"); lv_obj_clear_flag(w.whyRow, LV_OBJ_FLAG_HIDDEN); }
  if (w.validRow) {
    lv_label_set_text(w.validSt, p.validityMet ? "Condition remplie." : "Condition non remplie.");
    lv_obj_set_style_text_color(w.validSt, C(p.validityMet ? HX_GOOD : HX_WARN), 0);
  }
  if (p.detail[0]) { lv_label_set_text(w.detail, p.detail); lv_obj_clear_flag(w.detailRow, LV_OBJ_FLAG_HIDDEN); }
  else lv_obj_add_flag(w.detailRow, LV_OBJ_FLAG_HIDDEN);
}

void openProvenance(int group) {
  if (group < 0 || group >= PG_COUNT) return;
  lv_obj_t * body = sheetOpen(GROUP_TITLE[group]);
  s_kind = OK_PROV; s_token = sheetToken(); s_arg = group;
  lv_obj_t * sub = label(body, &vn_inter_12, HX_TEXT2,
    "D'o\xC3\xB9 vient chaque chiffre : champ du flux ou r\xC3\xA8gle de calcul, et \xC3\xA2ge de la donn\xC3\xA9" "e.");
  lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(sub, BW - 30);
  s_npw = 0;
  for (int k = 0; k < 3 && GROUP_KPI[group][k] != 255; k++) {
    ProvW & w = s_pw[s_npw++];
    w = ProvW();
    w.kpi = GROUP_KPI[group][k];
    vn::Provenance p;
    vn::provenance(g_model, w.kpi, p);           // parties fixes : libellé, source, règle
    lv_obj_t * c = card(body, 8);
    lv_obj_set_width(c, BW);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_t * h = row(c, 8);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_t * nm = label(h, &vn_inter_13s, HX_TEXT, p.name);
    lv_obj_set_flex_grow(nm, 1);
    w.value = label(h, &vn_inter_20b, HX_TEXT, "\xE2\x80\x94");
    w.badge = badge(h, "", T_NEUTRAL);
    char b[160];
    if (p.kind == vn::SRC_FEED) snprintf(b, sizeof(b), "%s \xC2\xB7 %s \xE2\x80\x94 champ %s", p.source, p.feed, p.field);
    else snprintf(b, sizeof(b), "Calcul interne, d'apr\xC3\xA8s %s", isMeteo(p) ? "Open-Meteo" : p.feed);
    lv_label_set_text(kv(c, "SOURCE"), b);
    lv_label_set_text(kv(c, "R\xC3\x88GLE"), p.rule);
    w.data = kv(c, "DONN\xC3\x89" "E");
    w.why = kv(c, "POURQUOI \xE2\x80\x94", &w.whyRow);
    lv_obj_set_style_text_color(w.why, C(HX_WARN), 0);
    if (p.validity) {
      lv_obj_t * vr = row(c, 8);
      w.validRow = vr;
      lv_obj_set_width(vr, LV_PCT(100));
      lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      lv_obj_t * k2 = eyebrow(vr, "VALIDIT\xC3\x89");
      lv_obj_set_width(k2, KEY_W);
      lv_obj_set_style_pad_top(k2, 2, 0);
      lv_obj_t * vc = col(vr, 1);
      w.valid = label(vc, &vn_inter_12, HX_TEXT2, p.validity);
      lv_label_set_long_mode(w.valid, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(w.valid, BW - 16 - KEY_W - 8);
      w.validSt = label(vc, &vn_inter_12, HX_GOOD, "");
    }
    w.detail = kv(c, "D\xC3\x89TAIL", &w.detailRow);
    provFill(w);
  }
}

// ══ Tableau d'ingestion ═════════════════════════════════════════════
struct IngW { lv_obj_t * badge, * ok, * last, * err; };
static IngW s_iw[vn::FEED_COUNT];
static lv_obj_t * s_ntpBadge, * s_ntp, * s_wifiBadge, * s_wifi;

static void errText(char * out, size_t n, int code) {
  if (code > 0) snprintf(out, n, "HTTP %d (%s)", code, vn::ingestErrText(code));
  else snprintf(out, n, "erreur %d (%s)", code, vn::ingestErrText(code));
}

static void ingestFill() {
  net::Status ns = net::status();
  int64_t t = now();
  char b[200], w[48], e[48], n1[16], n2[16];
  for (int f = 0; f < vn::FEED_COUNT; f++) {
    const vn::FeedStat & s = ns.feed[f];
    IngW & r = s_iw[f];
    if (!s.okCount && !s.errCount) badgeSet(r.badge, "EN ATTENTE", T_NEUTRAL);
    else if (s.fails) { snprintf(b, sizeof(b), "\xC3\x89" "CHEC \xC3\x97%u", (unsigned)s.fails); badgeSet(r.badge, b, T_BAD); }
    else badgeSet(r.badge, "OK", T_GOOD);
    fmtInt(n1, sizeof(n1), (int)s.okCount);
    if (s.okAt) { whenText(w, sizeof(w), s.okAt, t); snprintf(b, sizeof(b), "Dernier succ\xC3\xA8s %s \xC2\xB7 %s r\xC3\xA9ussi%s", w, n1, s.okCount > 1 ? "s" : ""); }
    else snprintf(b, sizeof(b), "Aucun succ\xC3\xA8s depuis le d\xC3\xA9marrage");
    lv_label_set_text(r.ok, b);
    if (s.okCount || s.errCount) {
      fmtInt(n2, sizeof(n2), (int)s.bytes);
      snprintf(b, sizeof(b), "Dernier relev\xC3\xA9 : %s octets re\xC3\xA7us en %u ms", n2, (unsigned)s.ms);
    } else snprintf(b, sizeof(b), "Dernier relev\xC3\xA9 : aucun");
    lv_label_set_text(r.last, b);
    if (s.errCount) {
      errText(e, sizeof(e), s.errCode);
      whenText(w, sizeof(w), s.errAt, t);
      fmtInt(n2, sizeof(n2), (int)s.errCount);
      snprintf(b, sizeof(b), "Dernier \xC3\xA9" "chec : %s \xC3\xA0 %s \xC2\xB7 %u cons\xC3\xA9" "cutif%s \xC2\xB7 %s au total",
               e, w, (unsigned)s.fails, s.fails > 1 ? "s" : "", n2);
    } else snprintf(b, sizeof(b), "Aucun \xC3\xA9" "chec depuis le d\xC3\xA9marrage");
    lv_label_set_text(r.err, b);
    lv_obj_set_style_text_color(r.err, C(s.fails ? HX_WARN : HX_TEXT3), 0);
  }
  if (ns.timeOk) {
    badgeSet(s_ntpBadge, "SYNCHRO", T_GOOD);
    if (ns.timeSyncAt) { whenText(w, sizeof(w), ns.timeSyncAt, t); snprintf(b, sizeof(b), "Heure synchronis\xC3\xA9" "e depuis %s \xC2\xB7 %s, %s", w, VN_NTP1, VN_NTP2); }
    else snprintf(b, sizeof(b), "Heure synchronis\xC3\xA9" "e \xC2\xB7 %s, %s", VN_NTP1, VN_NTP2);
  } else {
    badgeSet(s_ntpBadge, "NON SYNCHRO", T_WARN);
    snprintf(b, sizeof(b), "Heure non synchronis\xC3\xA9" "e : aucun horodatage possible \xC2\xB7 %s, %s", VN_NTP1, VN_NTP2);
  }
  lv_label_set_text(s_ntp, b);
  switch (ns.wifi) {
    case net::W_CONNECTED:
      badgeSet(s_wifiBadge, "CONNECT\xC3\x89", T_GOOD);
      snprintf(b, sizeof(b), "%s \xC2\xB7 RSSI %d dBm \xC2\xB7 IP %s", ns.ssid, ns.rssi, ns.ip);
      break;
    case net::W_CONNECTING:
      badgeSet(s_wifiBadge, "CONNEXION", T_WARN);
      snprintf(b, sizeof(b), "Connexion \xC3\xA0 %s\xE2\x80\xA6 \xC2\xB7 RSSI et IP inconnus", ns.ssid[0] ? ns.ssid : g_set.ssid);
      break;
    case net::W_FAILED:
      badgeSet(s_wifiBadge, "\xC3\x89" "CHEC", T_BAD);
      snprintf(b, sizeof(b), "Impossible de joindre %s", ns.ssid[0] ? ns.ssid : g_set.ssid);
      break;
    default:
      badgeSet(s_wifiBadge, "AUCUN", T_NEUTRAL);
      snprintf(b, sizeof(b), "Aucun r\xC3\xA9seau configur\xC3\xA9");
  }
  lv_label_set_text(s_wifi, b);
}

static lv_obj_t * ingRow(lv_obj_t * list, const char * name, lv_obj_t ** badgeOut, bool first) {
  lv_obj_t * r = col(list, 2);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_style_pad_hor(r, 10, 0);
  lv_obj_set_style_pad_ver(r, 7, 0);
  if (!first) divider(r);
  lv_obj_t * h = row(r, 8);
  lv_obj_set_width(h, LV_PCT(100));
  lv_obj_t * nm = label(h, &vn_mono_12b, HX_TEXT, name);
  lv_obj_set_flex_grow(nm, 1);
  *badgeOut = badge(h, "", T_NEUTRAL);
  return r;
}
static lv_obj_t * ingLine(lv_obj_t * r, const lv_font_t * f, uint32_t col) {
  lv_obj_t * l = label(r, f, col, "");
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, BW - 20);
  return l;
}

void openIngest() {
  lv_obj_t * body = sheetOpen("Ingestion des flux");
  s_kind = OK_INGEST; s_token = sheetToken(); s_arg = -1;
  lv_obj_t * sub = label(body, &vn_inter_12, HX_TEXT2,
    "Mesures de la t\xC3\xA2" "che r\xC3\xA9seau depuis le d\xC3\xA9marrage (rien d'estim\xC3\xA9) \xC2\xB7 GBFS api.cyclocity.fr, api.open-meteo.com.");
  lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(sub, BW - 30);
  lv_obj_t * list = card(body, 0);
  lv_obj_set_width(list, BW);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  for (int f = 0; f < vn::FEED_COUNT; f++) {
    lv_obj_t * r = ingRow(list, vn::feedName(f), &s_iw[f].badge, f == 0);
    s_iw[f].ok = ingLine(r, &vn_inter_12, HX_TEXT2);
    s_iw[f].last = ingLine(r, &vn_inter_12, HX_TEXT2);
    s_iw[f].err = ingLine(r, &vn_inter_11, HX_TEXT3);
  }
  lv_obj_t * sys = card(body, 0);
  lv_obj_set_width(sys, BW);
  lv_obj_set_flex_flow(sys, LV_FLEX_FLOW_COLUMN);
  lv_obj_t * r = ingRow(sys, "NTP", &s_ntpBadge, true);
  s_ntp = ingLine(r, &vn_inter_12, HX_TEXT2);
  r = ingRow(sys, "Wi-Fi", &s_wifiBadge, false);
  s_wifi = ingLine(r, &vn_inter_12, HX_TEXT2);
  ingestFill();
}

// ══ Prévision 24 h d'une station ════════════════════════════════════
// Pas de 15 min sur 24 h. Un créneau dont la confiance du profil est < 50 % est
// tracé en pointillés : la courbe y repose surtout sur la valeur actuelle.
static const int N24 = 24 * 4 + 1;
static const float FC24_MIN_CONF = 0.5f;
static float s_v[N24], s_c[N24], s_ok[N24], s_weak[N24], s_w[N24];
static lv_obj_t * s_chart, * s_basis, * s_range, * s_wx, * s_boxV[4], * s_boxC[4];

static void fc24Fill() {
  int st = s_arg;
  if (st < 0 || st >= g_model.n) return;
  const vn::Station & s = g_model.st[st];
  int64_t t = now();
  g_model.project(st, t, 15, N24, s_v, s_c, s_w);
  int good = 0;
  for (int k = 0; k < N24; k++) good += s_c[k] >= FC24_MIN_CONF;
  // Deux séries jointives : fiable (trait plein) / peu nourri (pointillés)
  for (int k = 0; k < N24; k++) {
    bool w = s_c[k] < FC24_MIN_CONF;
    bool wn = (k > 0 && s_c[k - 1] < FC24_MIN_CONF) || (k + 1 < N24 && s_c[k + 1] < FC24_MIN_CONF);
    s_ok[k] = w ? NAN : s_v[k];
    s_weak[k] = (w || wn) ? s_v[k] : NAN;
  }
  ChartData * d = chartData(s_chart);
  float capE = s.capEff(), mx = fmaxf(4.f, fmaxf(capE, (float)s.cap));
  d->ymin = 0; d->ymax = mx; d->hline = capE;
  snprintf(d->ylab[0], sizeof(d->ylab[0]), "%d", (int)mx);
  snprintf(d->ylab[1], sizeof(d->ylab[1]), "0");
  d->nxl = 0;
  for (int k = 3; k < N24 - 3 && d->nxl < 6; k++) {
    struct tm lt;
    plat::localTime(t + (int64_t)k * 900, &lt);
    if (lt.tm_min < 15 && lt.tm_hour % 6 == 0) {
      d->xlPos[d->nxl] = k;
      snprintf(d->xl[d->nxl], sizeof(d->xl[0]), "%02dh", lt.tm_hour);
      d->nxl++;
    }
  }
  chartInvalidate(s_chart);

  char b[220], dd[16];
  int days = g_model.learnSince ? (int)((t - g_model.learnSince) / 86400) : -1;
  int pct = (int)lroundf(100.f * good / N24);
  if (days < 0) snprintf(b, sizeof(b), "Aucun profil appris : la courbe reste \xC3\xA0 la valeur actuelle, tout en pointill\xC3\xA9s.");
  else {
    struct tm lt;
    plat::localTime(g_model.learnSince, &lt);
    snprintf(dd, sizeof(dd), "%02d/%02d", lt.tm_mday, lt.tm_mon + 1);
    if (days < 1) snprintf(b, sizeof(b), "Profil appris sur moins d'un jour (depuis le %s) \xC2\xB7 profil fiable (confiance \xE2\x89\xA5 50 %%) sur %d %% des 24 h.", dd, pct);
    else snprintf(b, sizeof(b), "Profil appris sur %d jour%s (depuis le %s) \xC2\xB7 profil fiable (confiance \xE2\x89\xA5 50 %%) sur %d %% des 24 h.",
                  days, days > 1 ? "s" : "", dd, pct);
  }
  // Jours fériés couverts par la courbe : prévus avec le profil du dimanche (étape 5a-ii)
  for (int k = 0; k < N24 && days >= 0; k += 4) {
    int64_t tk = t + (int64_t)k * 900;
    if (!g_model.holidayProfile(tk)) continue;
    struct tm hl;
    plat::localTime(tk, &hl);
    size_t L = strlen(b);
    const char * hn = vn::holidayNameAt(tk);
    snprintf(b + L, sizeof(b) - L, " Le %02d/%02d est f\xC3\xA9ri\xC3\xA9 (%s) : profil du dimanche.", hl.tm_mday, hl.tm_mon + 1, hn ? hn : "?");
    break;
  }
  lv_label_set_text(s_basis, b);
  // Points remarquables (créneaux à 15 min)
  int kMin = 0, kMax = 0;
  for (int k = 1; k < N24; k++) { if (s_v[k] < s_v[kMin]) kMin = k; if (s_v[k] > s_v[kMax]) kMax = k; }
  char h1[8], h2[8];
  fmtClock(h1, sizeof(h1), t + (int64_t)kMin * 900);
  fmtClock(h2, sizeof(h2), t + (int64_t)kMax * 900);
  snprintf(b, sizeof(b), "Plus bas ~%d v\xC3\xA9lo%s vers %s%s \xC2\xB7 plus haut ~%d vers %s%s",
           (int)lroundf(s_v[kMin]), lroundf(s_v[kMin]) > 1 ? "s" : "", h1, s_c[kMin] < FC24_MIN_CONF ? " (peu fiable)" : "",
           (int)lroundf(s_v[kMax]), h2, s_c[kMax] < FC24_MIN_CONF ? " (peu fiable)" : "");
  lv_label_set_text(s_range, b);
  // Part météo : la plus marquée sur la courbe (au-delà de 2 h, elle vient de la météo annoncée)
  {
    int kw = 0;
    for (int k = 1; k < N24; k++) if (fabsf(s_w[k]) > fabsf(s_w[kw])) kw = k;
    char wb[240];
    vn::wxPartText(g_model, s_w[kw], kw * 15 > 120, t + (int64_t)kw * 900, wb, sizeof(wb));
    lv_label_set_text(s_wx, wb + (strncmp(wb, "M\xC3\xA9t\xC3\xA9o : ", 10) == 0 ? 10 : 0));
  }
  static const int K[4] = { 12, 24, 48, 96 };
  for (int i = 0; i < 4; i++) {
    lv_label_set_text_fmt(s_boxV[i], "%d", (int)lroundf(s_v[K[i]]));
    bool ok = s_c[K[i]] >= FC24_MIN_CONF;
    lv_obj_set_style_text_color(s_boxV[i], C(ok ? HX_TEXT : HX_TEXT3), 0);
    lv_label_set_text_fmt(s_boxC[i], "conf. %d %%", (int)lroundf(s_c[K[i]] * 100));
    lv_obj_set_style_text_color(s_boxC[i], C(ok ? HX_TEXT3 : HX_WARN), 0);
  }
}

void openForecast24(int st) {
  if (st < 0 || st >= g_model.n) return;
  lv_obj_t * body = sheetOpen("Pr\xC3\xA9vision 24 h");
  s_kind = OK_FC24; s_token = sheetToken(); s_arg = st;
  lv_obj_t * nm = label(body, &vn_inter_13s, HX_TEXT2, g_model.st[st].name);
  oneLine(nm, BW - 30);

  lv_obj_t * cc = card(body, 8);
  lv_obj_set_width(cc, BW);
  lv_obj_set_style_bg_color(cc, C(HX_BG), 0);
  lv_obj_t * ch = row(cc, 6);
  lv_obj_set_width(ch, LV_PCT(100));
  eyebrow(ch, "V\xC3\x89LOS PR\xC3\x89VUS \xC2\xB7 24 H");
  lv_obj_t * lg = label(ch, &vn_mono_10, HX_TEXT3, "#F5820D \xE2\x80\x94# profil fiable  #F5820D \xE2\x80\xA2\xE2\x80\xA2# peu nourri");
  lv_label_set_recolor(lg, true);
  lv_obj_set_flex_grow(lg, 1);
  lv_obj_set_style_text_align(lg, LV_TEXT_ALIGN_RIGHT, 0);
  s_chart = chartCreate(cc, BW - 16, 96);
  lv_obj_set_y(s_chart, 18);
  ChartData * d = chartData(s_chart);
  d->nx = N24; d->ns = 2; d->endDot = false; d->padL = 20;
  d->nowX = 0;
  d->s[0].y = s_ok; d->s[0].n = N24; d->s[0].kind = SK_LINE; d->s[0].width = 2; d->s[0].color = C(HX_ACCENT);
  d->s[1].y = s_weak; d->s[1].n = N24; d->s[1].kind = SK_DOTS; d->s[1].width = 2; d->s[1].color = C(HX_ACCENT);

  lv_obj_t * fr = row(body, 6);
  lv_obj_set_width(fr, BW);
  static const char * FL[4] = { "+3 H", "+6 H", "+12 H", "+24 H" };
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
    s_boxV[k] = label(f, &vn_inter_15b, HX_TEXT, "\xE2\x80\x94");
    s_boxC[k] = label(f, &vn_mono_10, HX_TEXT3, "");
  }
  lv_obj_t * info = card(body, 8);
  lv_obj_set_width(info, BW);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(info, 4, 0);
  s_basis = kv(info, "REPOSE SUR");
  s_range = kv(info, "REP\xC3\x88RES");
  s_wx = kv(info, "M\xC3\x89T\xC3\x89O");
  lv_label_set_text(kv(info, "R\xC3\x88GLE"),
    "Celle de la pr\xC3\xA9vision 2 h, prolong\xC3\xA9" "e : actuel + c\xC2\xB7(profil(t+h) \xE2\x88\x92 profil(t)) + part m\xC3\xA9t\xC3\xA9o + (1 \xE2\x88\x92 0,6c)\xC2\xB7tendance 30 min amortie. "
    "Part m\xC3\xA9t\xC3\xA9o : m\xC3\xA9t\xC3\xA9o actuelle sur 2 h, annonc\xC3\xA9" "e jusqu'\xC3\xA0 6 h, rien au-del\xC3\xA0. "
    "Pointill\xC3\xA9s : cr\xC3\xA9neaux o\xC3\xB9 la confiance c du profil est < 50 %.");
  lv_obj_t * btns = row(body, 8);
  lv_obj_t * bb = button(btns, "Retour \xC3\xA0 la fiche", nullptr, B_SECONDARY, true);
  lv_obj_add_event_cb(bb, [](lv_event_t *) { openStation(s_arg); }, LV_EVENT_CLICKED, nullptr);
  fc24Fill();
}

// ── Mise à jour de la feuille ouverte (nouvelles données) ────────────
void opsSheetRefresh() {
  if (s_kind == OK_NONE) return;
  if (sheetToken() != s_token || s_token < 0) { s_kind = OK_NONE; return; }
  if (s_kind == OK_PROV) for (int i = 0; i < s_npw; i++) provFill(s_pw[i]);
  else if (s_kind == OK_INGEST) ingestFill();
  else if (s_kind == OK_FC24) fc24Fill();
}

} // namespace ui
