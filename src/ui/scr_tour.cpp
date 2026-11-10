// ── Écran « Tournée » : le Sunton du camion (rôle régulateur, core/regul.h) ─
// Premier onglet quand Réglages → Rôle = Régulateur. Le serveur propose le prochain arrêt (plan ESTIMÉ, avec
// sa marge) ; le régulateur valide ce qui a VRAIMENT été fait — déposés, pris, hors service ramassés, atelier —
// ou dit pourquoi l'arrêt n'a pas pu être fait. Seuls ces nombres validés comptent comme interventions ;
// le serveur les recoupe ensuite avec le flux public, sans jamais les corriger en silence.
// Hors connexion, chaque geste attend dans la file (en flash) et l'écran le dit.
#include "ui_internal.h"
#include "../net/net.h"
#include "../core/regul.h"
#include "../core/sync.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

using vn::g_model;
using vn::g_reg;

namespace ui {

static const lv_coord_t W = CONTENT_W - 16;            // 404

// ── Écran ────────────────────────────────────────────────────────────
static lv_obj_t * s_none, * s_noneTitle, * s_noneSub, * s_startBtn, * s_openBox, * s_noneRej, * s_noneStatus;
static lv_obj_t * s_tour, * s_tTitle, * s_load, * s_loadCap, * s_loadHs, * s_bar;
static lv_obj_t * s_stopEye, * s_stopBadge, * s_stopEta, * s_stopSid, * s_stopName, * s_stopAct, * s_stopWhy, * s_stopNow;
static lv_obj_t * s_btnDone, * s_btnImp, * s_btnOther, * s_btnPlan, * s_info;
static uint32_t s_seen = 0xFFFFFFFF;
static int64_t s_lastMinute = 0;
static int s_cur = -1;                                 // arrêt du plan affiché (index dans snap.stops), ‑1 aucun

static void capFirst(char * s) { if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32); }
static bool sameTour(const vn::RegTourView & v) { return g_reg.snap.valid && g_reg.snap.hasTour && !strcmp(g_reg.snap.tourId, v.id); }
static const char * atelierName() { return g_reg.snap.atelierName[0] ? g_reg.snap.atelierName : "Atelier"; }

// Bouton à choix (régulateur, camion) : bordure orange quand il est choisi
static lv_obj_t * pick(lv_obj_t * parent, const char * txt) {
  lv_obj_t * b = button(parent, txt, nullptr, B_SECONDARY);
  lv_obj_set_style_min_width(b, 92, 0);
  lv_obj_set_height(b, 32);
  return b;
}
static void pickSet(lv_obj_t * b, bool on) {
  lv_obj_set_style_border_color(b, on ? C(HX_ACCENT) : lv_color_white(), 0);
  lv_obj_set_style_border_opa(b, on ? 220 : OPA_BORDER_STRONG, 0);
  lv_obj_set_style_bg_color(b, on ? C(HX_ACCENT) : C(HX_S2), 0);
  lv_obj_set_style_bg_opa(b, on ? 46 : LV_OPA_COVER, 0);
  uint32_t n = lv_obj_get_child_cnt(b);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t * c = lv_obj_get_child(b, i);
    if (lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, C(on ? HX_ACCENT : HX_TEXT), 0);
  }
}
static void btnEnable(lv_obj_t * b, bool on) {
  if (on) lv_obj_clear_state(b, LV_STATE_DISABLED); else lv_obj_add_state(b, LV_STATE_DISABLED);
  lv_obj_set_style_opa(b, on ? LV_OPA_COVER : 110, 0);
}

// ── Compteurs [−] n [+] des feuilles ─────────────────────────────────
struct Step { int * v; int lo, hi; lv_obj_t * val; void (*changed)(); };
static Step s_step[4];
static int s_nStep = 0;
static void stepApply(int i, int d) {
  Step & s = s_step[i];
  int v = *s.v + d;
  if (v < s.lo) v = s.lo;
  if (v > s.hi) v = s.hi;
  *s.v = v;
  lv_label_set_text_fmt(s.val, "%d", v);
  if (s.changed) s.changed();
}
static void stepCb(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e) & 0xFF;
  int d = ((int)(intptr_t)lv_event_get_user_data(e) >> 8) ? 1 : -1;
  stepApply(i, d);
}
static void stepRow(lv_obj_t * body, const char * txt, int * v, int hi, void (*changed)()) {
  int i = s_nStep++;
  Step & s = s_step[i];
  s.v = v; s.lo = 0; s.hi = hi; s.changed = changed;
  lv_obj_t * r = row(body, 6);
  lv_obj_set_size(r, LV_PCT(100), 36);
  lv_obj_t * l = label(r, &vn_inter_13s, HX_TEXT, txt);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t * m = iconButton(r, &ic_minus_16, true, 36);
  s.val = label(r, &vn_mono_20b, HX_TEXT, "");
  lv_obj_set_width(s.val, 44);
  lv_obj_set_style_text_align(s.val, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text_fmt(s.val, "%d", *v);
  lv_obj_t * p = iconButton(r, &ic_plus_16, true, 36);
  static const lv_event_code_t CODES[2] = { LV_EVENT_CLICKED, LV_EVENT_LONG_PRESSED_REPEAT };   // appui long : ça défile
  for (lv_event_code_t c : CODES) {
    lv_obj_add_event_cb(m, stepCb, c, (void *)(intptr_t)(i));
    lv_obj_add_event_cb(p, stepCb, c, (void *)(intptr_t)(i | 0x100));
  }
}
// Deux compteurs sur une ligne (chargement au départ : en service | hors service)
static void stepHalf(lv_obj_t * r, const char * txt, int * v) {
  int i = s_nStep++;
  Step & s = s_step[i];
  s.v = v; s.lo = 0; s.hi = vn::REG_MAX_N; s.changed = nullptr;
  lv_obj_t * h = row(r, 4);
  lv_obj_set_flex_grow(h, 1);
  lv_obj_set_height(h, 36);
  lv_obj_t * l = label(h, &vn_inter_12, HX_TEXT, txt);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t * m = iconButton(h, &ic_minus_16, true, 36);
  s.val = label(h, &vn_mono_20b, HX_TEXT, "");
  lv_obj_set_width(s.val, 30);
  lv_obj_set_style_text_align(s.val, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text_fmt(s.val, "%d", *v);
  lv_obj_t * p = iconButton(h, &ic_plus_16, true, 36);
  static const lv_event_code_t CODES[2] = { LV_EVENT_CLICKED, LV_EVENT_LONG_PRESSED_REPEAT };
  for (lv_event_code_t c : CODES) {
    lv_obj_add_event_cb(m, stepCb, c, (void *)(intptr_t)(i));
    lv_obj_add_event_cb(p, stepCb, c, (void *)(intptr_t)(i | 0x100));
  }
}
static lv_obj_t * sayLabel(lv_obj_t * body) {
  lv_obj_t * s = label(body, &vn_inter_12, HX_WARN, "");
  lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s, LV_PCT(100));
  return s;
}
static lv_obj_t * para(lv_obj_t * body, const char * txt, uint32_t col = HX_TEXT2, const lv_font_t * f = &vn_inter_12) {
  lv_obj_t * s = label(body, f, col, txt);
  lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s, LV_PCT(100));
  return s;
}
// Dernière ligne d'une feuille : le message (chargement après, ou la raison d'un refus) et le bouton, côte à côte :
// tout tient sans faire défiler, même au volant d'un camion arrêté
static lv_obj_t * actionRow(lv_obj_t * body, const char * txt, const lv_img_dsc_t * ic, lv_event_cb_t cb, lv_obj_t ** msg) {
  lv_obj_t * r = row(body, 8);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_style_min_height(r, 38, 0);
  lv_obj_set_style_pad_top(r, 2, 0);
  *msg = label(r, &vn_inter_12, HX_TEXT2, "");
  lv_obj_set_flex_grow(*msg, 1);
  lv_label_set_long_mode(*msg, LV_LABEL_LONG_WRAP);
  lv_obj_t * b = button(r, txt, ic, B_PRIMARY);
  lv_obj_set_height(b, 38);
  lv_obj_set_style_min_width(b, 132, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}
static void sayErr(lv_obj_t * msg, const char * why) {
  lv_label_set_text(msg, why);
  lv_obj_set_style_text_color(msg, C(HX_WARN), 0);
}
static void compact(lv_obj_t * body) { lv_obj_set_style_pad_row(body, 4, 0); }

static void declared(const char * title, const char * msg) {
  sheetClose();
  toast(title, msg, T_GOOD, &ic_check_16);
  net::requestReg();
  s_seen = 0xFFFFFFFF;
}

// ── Arrêt fait : ce qui a VRAIMENT été fait ──────────────────────────
static struct {
  uint8_t kind;
  char sid[8], name[48];
  int8_t plSeq;
  vn::RegCount plDrop, plTake;
  int v[3];
  lv_obj_t * msg;
} s_val;

static void valCounts(vn::RegCount & took, vn::RegCount & dropped) {
  if (s_val.kind == vn::RK_ATELIER) { dropped = { (int16_t)s_val.v[1], (int16_t)s_val.v[0] }; took = { (int16_t)s_val.v[2], 0 }; }
  else { dropped = { (int16_t)s_val.v[0], 0 }; took = { (int16_t)s_val.v[1], (int16_t)s_val.v[2] }; }
}
static void valAfter() {
  vn::RegTourView v;
  vn::regTourNow(g_reg, v);
  vn::RegCount l = vn::regLoadNow(g_reg), took, dropped;
  valCounts(took, dropped);
  int ok = l.ok + took.ok - dropped.ok, hs = l.hs + took.hs - dropped.hs;
  bool bad = ok < 0 || hs < 0 || (v.cap && ok + hs > v.cap);
  char b[96];
  snprintf(b, sizeof(b), "\xC3\x80 bord apr\xC3\xA8s : %d + %d HS / %d", ok, hs, v.cap);
  lv_label_set_text(s_val.msg, b);
  lv_obj_set_style_text_color(s_val.msg, C(bad ? HX_WARN : HX_TEXT2), 0);
}
static void valSubmit(lv_event_t *) {
  vn::RegDecl d;
  memset(&d, 0, sizeof(d));
  d.kind = s_val.kind;
  snprintf(d.sid, sizeof(d.sid), "%s", s_val.sid);
  snprintf(d.name, sizeof(d.name), "%s", s_val.name);
  valCounts(d.took, d.dropped);
  d.plSeq = s_val.plSeq;
  d.plDrop = s_val.plDrop;
  d.plTake = s_val.plTake;
  char why[200];
  if (!vn::regDeclare(g_reg, d, now(), net::deviceId(), why, sizeof(why))) { sayErr(s_val.msg, why); return; }
  char m[160], t[80];
  vn::regDeclText(d.kind, d.took, d.dropped, t, sizeof(t));
  snprintf(m, sizeof(m), "%s : %s", d.name, t);
  declared("Arr\xC3\xAAt d\xC3\xA9" "clar\xC3\xA9", m);
}

static void openValidate(uint8_t kind, const char * sid, const char * name, const vn::RegStop * planned) {
  memset(&s_val, 0, sizeof(s_val));
  s_val.kind = kind;
  snprintf(s_val.sid, sizeof(s_val.sid), "%s", sid ? sid : "");
  snprintf(s_val.name, sizeof(s_val.name), "%s", name ? name : "");
  s_val.plSeq = planned ? (int8_t)planned->seq : -1;
  if (planned) { s_val.plDrop = planned->drop; s_val.plTake = planned->take; }
  char t[96];
  snprintf(t, sizeof(t), "%s", kind == vn::RK_ATELIER ? atelierName() : s_val.name);
  lv_obj_t * body = sheetOpen(t);
  compact(body);
  if (planned) {
    char a[96], p[160];
    vn::regActText(*planned, a, sizeof(a));
    snprintf(p, sizeof(p), "Ce qui a vraiment \xC3\xA9t\xC3\xA9 fait \xC2\xB7 propos\xC3\xA9 : %s (estim\xC3\xA9)", a);
    oneLine(para(body, p, HX_TEXT3, &vn_inter_11), LV_PCT(100));
  } else para(body, "Arr\xC3\xAAt hors plan : r\xC3\xA9gler ce qui a vraiment \xC3\xA9t\xC3\xA9 fait.", HX_TEXT3, &vn_inter_11);
  s_nStep = 0;
  if (kind == vn::RK_ATELIER) {
    s_val.v[0] = planned ? planned->drop.hs : 0;
    s_val.v[1] = planned ? planned->drop.ok : 0;
    s_val.v[2] = planned ? planned->take.ok : 0;
    stepRow(body, "V\xC3\xA9los HS d\xC3\xA9" "charg\xC3\xA9s", &s_val.v[0], vn::REG_MAX_N, valAfter);
    stepRow(body, "En service d\xC3\xA9pos\xC3\xA9s", &s_val.v[1], vn::REG_MAX_N, valAfter);
    stepRow(body, "Neufs / r\xC3\xA9par\xC3\xA9s repris", &s_val.v[2], vn::REG_MAX_N, valAfter);
  } else {
    s_val.v[0] = planned ? planned->drop.ok : 0;
    s_val.v[1] = planned ? planned->take.ok : 0;
    s_val.v[2] = planned ? planned->take.hs : 0;
    stepRow(body, "V\xC3\xA9los d\xC3\xA9pos\xC3\xA9s", &s_val.v[0], vn::REG_MAX_N, valAfter);
    stepRow(body, "V\xC3\xA9los pris", &s_val.v[1], vn::REG_MAX_N, valAfter);
    stepRow(body, "V\xC3\xA9los HS ramass\xC3\xA9s", &s_val.v[2], vn::REG_MAX_N, valAfter);
  }
  actionRow(body, "Valider", &ic_check_16, valSubmit, &s_val.msg);
  valAfter();
}

// ── Arrêt impossible ─────────────────────────────────────────────────
static struct { uint8_t kind; char sid[8], name[48]; int8_t plSeq; vn::RegCount plDrop, plTake; lv_obj_t * say; } s_imp;
static void impPick(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  vn::RegDecl d;
  memset(&d, 0, sizeof(d));
  d.kind = s_imp.kind;
  snprintf(d.sid, sizeof(d.sid), "%s", s_imp.sid);
  snprintf(d.name, sizeof(d.name), "%s", s_imp.name);
  d.impossible = vn::regWhy(i);
  d.plSeq = s_imp.plSeq; d.plDrop = s_imp.plDrop; d.plTake = s_imp.plTake;
  char why[200];
  if (!vn::regDeclare(g_reg, d, now(), net::deviceId(), why, sizeof(why))) { lv_label_set_text(s_imp.say, why); return; }
  char m[120];
  snprintf(m, sizeof(m), "%s : %s", d.name, d.impossible);
  declared("Arr\xC3\xAAt impossible d\xC3\xA9" "clar\xC3\xA9", m);
}
static void openImpossible(const vn::RegStop & s) {
  memset(&s_imp, 0, sizeof(s_imp));
  s_imp.kind = s.kind;
  snprintf(s_imp.sid, sizeof(s_imp.sid), "%s", s.sid);
  snprintf(s_imp.name, sizeof(s_imp.name), "%s", s.kind == vn::RK_ATELIER ? atelierName() : s.name);
  s_imp.plSeq = (int8_t)s.seq; s_imp.plDrop = s.drop; s_imp.plTake = s.take;
  char t[96];
  snprintf(t, sizeof(t), "Pourquoi pas %s ?", s_imp.name);
  lv_obj_t * body = sheetOpen(t);
  para(body, "L'arr\xC3\xAAt est d\xC3\xA9" "clar\xC3\xA9 non fait, avec la raison ; rien n'est compt\xC3\xA9 comme d\xC3\xA9pos\xC3\xA9 ou pris.");
  for (int i = 0; i < vn::REG_N_WHY; i++) {
    lv_obj_t * b = button(body, vn::regWhy(i), nullptr, B_SECONDARY);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, impPick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  s_imp.say = sayLabel(body);
}

// ── Autre arrêt : atelier, ou station par son numéro ─────────────────
static char s_digits[5];
static lv_obj_t * s_otherList, * s_otherTitle;
static float s_refLat, s_refLon;
static bool s_hasRef;
static void otherPickSt(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= g_model.n) return;
  const vn::Station & s = g_model.st[i];
  openValidate(vn::RK_STATION, s.sid, s.name, nullptr);
}
static lv_obj_t * listRow(lv_obj_t * parent, bool tonal) {
  lv_obj_t * r = row(parent, 6);
  lv_obj_set_size(r, LV_PCT(100), 34);
  lv_obj_set_style_pad_hor(r, 6, 0);
  lv_obj_set_style_radius(r, 6, 0);
  lv_obj_set_style_bg_color(r, C(tonal ? HX_ACCENT : HX_S2), 0);
  lv_obj_set_style_bg_opa(r, tonal ? 36 : LV_OPA_COVER, 0);
  if (tonal) { lv_obj_set_style_border_color(r, C(HX_ACCENT), 0); lv_obj_set_style_border_opa(r, 90, 0); lv_obj_set_style_border_width(r, 1, 0); }
  lv_obj_set_style_bg_color(r, C(HX_S3), LV_STATE_PRESSED);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  return r;
}
static void otherFill() {
  lv_obj_clean(s_otherList);
  char t[48];
  if (s_digits[0]) snprintf(t, sizeof(t), "Autre arr\xC3\xAAt \xC2\xB7 n\xC2\xB0 %s", s_digits);
  else snprintf(t, sizeof(t), "Autre arr\xC3\xAAt");
  lv_label_set_text(s_otherTitle, t);
  if (!s_digits[0]) {                                   // l'atelier en tête, tant qu'aucun numéro n'est tapé
    lv_obj_t * r = listRow(s_otherList, true);
    lv_obj_add_event_cb(r, [](lv_event_t *) { openValidate(vn::RK_ATELIER, "", atelierName(), nullptr); }, LV_EVENT_CLICKED, nullptr);
    icon(r, &ic_wrench_16, HX_ACCENT);
    lv_obj_t * nm = label(r, &vn_inter_13s, HX_ACCENT, atelierName());
    lv_obj_set_flex_grow(nm, 1);
    oneLine(nm, 170);
  }
  int best[8], nb = 0;
  float bestD[8];
  size_t dl = strlen(s_digits);
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (!s.active || !s.sid[0]) continue;
    if (dl && strncmp(s.sid, s_digits, dl)) continue;
    // sans numéro : les plus proches du prochain arrêt (aucun GPS : c'est la meilleure référence) ; avec : par numéro
    float d = dl || !s_hasRef ? (float)atoi(s.sid) : vn::haversineM(s_refLat, s_refLon, s.lat, s.lon);
    int k = nb < 8 ? nb++ : 8;
    if (k == 8) { if (d >= bestD[7]) continue; k = 7; }
    while (k > 0 && bestD[k - 1] > d) { best[k] = best[k - 1]; bestD[k] = bestD[k - 1]; k--; }
    best[k] = i; bestD[k] = d;
  }
  if (!nb) { label(s_otherList, &vn_inter_12, HX_TEXT3, "Aucune station avec ce num\xC3\xA9ro"); return; }
  for (int k = 0; k < nb; k++) {
    const vn::Station & s = g_model.st[best[k]];
    lv_obj_t * r = listRow(s_otherList, false);
    lv_obj_add_event_cb(r, otherPickSt, LV_EVENT_CLICKED, (void *)(intptr_t)best[k]);
    lv_obj_t * n = label(r, &vn_mono_12b, HX_ACCENT, s.sid);
    lv_obj_set_width(n, 28);
    lv_obj_t * nm = label(r, &vn_inter_12, HX_TEXT, s.name);
    lv_obj_set_flex_grow(nm, 1);
    oneLine(nm, 120);
    char b[24];
    if (s.live) snprintf(b, sizeof(b), "%u v \xC2\xB7 %u b", (unsigned)s.bikes, (unsigned)s.docks);
    else snprintf(b, sizeof(b), "\xE2\x80\x94");
    label(r, &vn_mono_10, HX_TEXT3, b);
  }
}
static void otherKey(lv_event_t * e) {
  lv_obj_t * m = lv_event_get_target(e);
  uint16_t id = lv_btnmatrix_get_selected_btn(m);
  if (id == LV_BTNMATRIX_BTN_NONE) return;
  const char * t = lv_btnmatrix_get_btn_text(m, id);
  size_t l = strlen(s_digits);
  if (!t) return;
  if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { if (l) s_digits[l - 1] = 0; }
  else if (!strcmp(t, "C")) s_digits[0] = 0;
  else if (t[0] >= '0' && t[0] <= '9' && l + 1 < sizeof(s_digits)) { s_digits[l] = t[0]; s_digits[l + 1] = 0; }
  otherFill();
}
static void openOther() {
  lv_obj_t * body = sheetOpen("Autre arr\xC3\xAAt");
  s_otherTitle = lv_obj_get_child(body, 0);             // le titre affiche le numéro tapé
  lv_obj_t * r = row(body, 8);
  lv_obj_set_size(r, LV_PCT(100), 144);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_t * left = col(r, 4);
  lv_obj_set_width(left, 150);
  static const char * MAP[] = { "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", "C", "0", LV_SYMBOL_BACKSPACE, "" };
  lv_obj_t * kp = lv_btnmatrix_create(left);
  lv_btnmatrix_set_map(kp, MAP);
  lv_obj_set_size(kp, 150, 144);
  lv_obj_set_style_bg_opa(kp, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(kp, 0, 0);
  lv_obj_set_style_pad_all(kp, 0, 0);
  lv_obj_set_style_pad_gap(kp, 4, 0);
  lv_obj_set_style_text_font(kp, &lv_font_montserrat_14, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kp, C(HX_S3), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kp, C(HX_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_radius(kp, 6, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kp, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kp, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kp, C(HX_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(kp, otherKey, LV_EVENT_VALUE_CHANGED, nullptr);
  s_otherList = col(r, 4);
  lv_obj_set_size(s_otherList, 222, 144);
  lv_obj_add_flag(s_otherList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_otherList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_otherList, LV_SCROLLBAR_MODE_OFF);
  // Référence pour « les plus proches » : le prochain arrêt du plan, sinon la dernière station déclarée
  s_hasRef = false;
  const char * refSid = nullptr;
  if (s_cur >= 0 && g_reg.snap.stops[s_cur].kind == vn::RK_STATION) refSid = g_reg.snap.stops[s_cur].sid;
  for (int i = 0; !refSid && i < g_reg.snap.nEvents; i++) if (g_reg.snap.events[i].kind == vn::RK_STATION) refSid = g_reg.snap.events[i].sid;
  int ri = refSid ? g_model.find(refSid) : -1;
  if (ri >= 0) { s_refLat = g_model.st[ri].lat; s_refLon = g_model.st[ri].lon; s_hasRef = true; }
  s_digits[0] = 0;
  otherFill();
}

// ── Début de tournée ─────────────────────────────────────────────────
static struct { int reg, truck, ok, hs; lv_obj_t * regB[vn::REG_MAX_PEOPLE], * truckB[vn::REG_MAX_TRUCKS], * msg; } s_st;
static void startRefresh() {
  for (int i = 0; i < g_reg.snap.nRegs; i++) pickSet(s_st.regB[i], i == s_st.reg);
  for (int i = 0; i < g_reg.snap.nTrucks; i++) pickSet(s_st.truckB[i], i == s_st.truck);
  if (s_st.msg) { lv_label_set_text(s_st.msg, ""); lv_obj_set_style_text_color(s_st.msg, C(HX_TEXT2), 0); }
}
static void openStart() {
  memset(&s_st, 0, sizeof(s_st));
  s_st.reg = s_st.truck = -1;
  for (int i = 0; i < g_reg.snap.nRegs; i++) if (!strcmp(g_reg.snap.regs[i].id, vn::g_regCfg.lastReg)) s_st.reg = i;
  for (int i = 0; i < g_reg.snap.nTrucks; i++) if (!strcmp(g_reg.snap.trucks[i].id, vn::g_regCfg.lastTruck)) s_st.truck = i;
  if (g_reg.snap.nRegs == 1) s_st.reg = 0;
  if (g_reg.snap.nTrucks == 1) s_st.truck = 0;
  lv_obj_t * body = sheetOpen("D\xC3\xA9" "but de tourn\xC3\xA9" "e");
  compact(body);
  lv_obj_set_style_pad_bottom(body, 4, 0);
  eyebrow(body, "R\xC3\x89GULATEUR");
  lv_obj_t * rr = row(body, 6);
  lv_obj_set_width(rr, LV_PCT(100));
  lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(rr, 6, 0);
  for (int i = 0; i < g_reg.snap.nRegs; i++) {
    s_st.regB[i] = pick(rr, g_reg.snap.regs[i].name);
    lv_obj_add_event_cb(s_st.regB[i], [](lv_event_t * e) { s_st.reg = (int)(intptr_t)lv_event_get_user_data(e); startRefresh(); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  eyebrow(body, "CAMION DU JOUR");
  lv_obj_t * tr = row(body, 6);
  lv_obj_set_width(tr, LV_PCT(100));
  lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(tr, 6, 0);
  for (int i = 0; i < g_reg.snap.nTrucks; i++) {
    char b[64];
    snprintf(b, sizeof(b), "%s \xC2\xB7 %u v\xC3\xA9los", g_reg.snap.trucks[i].name, (unsigned)g_reg.snap.trucks[i].cap);
    s_st.truckB[i] = pick(tr, b);
    lv_obj_add_event_cb(s_st.truckB[i], [](lv_event_t * e) { s_st.truck = (int)(intptr_t)lv_event_get_user_data(e); startRefresh(); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  // déjà à bord au départ : les deux nombres sur une ligne
  s_nStep = 0;
  lv_obj_t * lr = row(body, 8);
  lv_obj_set_width(lr, LV_PCT(100));
  lv_obj_t * cap = label(lr, &vn_inter_11, HX_TEXT3, "D\xC3\xA9j\xC3\xA0 \xC3\xA0 bord");
  lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(cap, 46);
  stepHalf(lr, "Service", &s_st.ok);
  stepHalf(lr, "HS", &s_st.hs);
  actionRow(body, "Commencer", &ic_play_16, [](lv_event_t *) {
    char why[200];
    if (!vn::regStart(g_reg, s_st.truck, s_st.reg, vn::RegCount{ (int16_t)s_st.ok, (int16_t)s_st.hs }, now(), net::deviceId(), why, sizeof(why))) {
      sayErr(s_st.msg, why);
      return;
    }
    char m[96];
    snprintf(m, sizeof(m), "%s \xC2\xB7 %s \xC2\xB7 \xC3\xA0 bord %d + %d HS", g_reg.snap.regs[s_st.reg].name, g_reg.snap.trucks[s_st.truck].name, s_st.ok, s_st.hs);
    declared("Tourn\xC3\xA9" "e commenc\xC3\xA9" "e", m);
  }, &s_st.msg);
  startRefresh();
}

// ── Fin de tournée ───────────────────────────────────────────────────
static struct { int ok, hs; lv_obj_t * msg; } s_end;
static void openEnd() {
  vn::RegCount l = vn::regLoadNow(g_reg);
  s_end.ok = l.ok > 0 ? l.ok : 0;
  s_end.hs = l.hs > 0 ? l.hs : 0;
  lv_obj_t * body = sheetOpen("Fin de tourn\xC3\xA9" "e");
  compact(body);
  para(body, "Ce qui reste \xC3\xA0 bord. Le compte calcul\xC3\xA9 \xC3\xA0 partir des arr\xC3\xAAts d\xC3\xA9" "clar\xC3\xA9s est propos\xC3\xA9 ; "
             "s'il ne correspond pas, le corriger : les deux sont gard\xC3\xA9s c\xC3\xB4te \xC3\xA0 c\xC3\xB4te.");
  s_nStep = 0;
  stepRow(body, "V\xC3\xA9los en service", &s_end.ok, vn::REG_MAX_N, nullptr);
  stepRow(body, "V\xC3\xA9los hors service", &s_end.hs, vn::REG_MAX_N, nullptr);
  actionRow(body, "Terminer", &ic_flag_16, [](lv_event_t *) {
    char why[200];
    if (!vn::regEnd(g_reg, vn::RegCount{ (int16_t)s_end.ok, (int16_t)s_end.hs }, now(), why, sizeof(why))) { sayErr(s_end.msg, why); return; }
    char m[64];
    snprintf(m, sizeof(m), "Reste \xC3\xA0 bord : %d + %d HS (d\xC3\xA9" "clar\xC3\xA9)", s_end.ok, s_end.hs);
    declared("Tourn\xC3\xA9" "e termin\xC3\xA9" "e", m);
  }, &s_end.msg);
}

// ── Plan de la tournée et déclarations ───────────────────────────────
static void planStopCb(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= g_reg.snap.nStops) return;
  const vn::RegStop & s = g_reg.snap.stops[i];
  openValidate(s.kind, s.sid, s.kind == vn::RK_ATELIER ? atelierName() : s.name, &s);
}
static void eventRow(lv_obj_t * body, const char * name, const char * what, const char * state, uint32_t stateCol) {
  lv_obj_t * c = col(body, 1);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_style_pad_ver(c, 4, 0);
  lv_obj_set_style_border_side(c, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(c, lv_color_white(), 0);
  lv_obj_set_style_border_opa(c, OPA_BORDER, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_t * r = row(c, 6);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_t * n = label(r, &vn_inter_13s, HX_TEXT, name);
  lv_obj_set_flex_grow(n, 1);
  oneLine(n, 200);
  label(r, &vn_inter_12, HX_TEXT2, what);
  lv_obj_t * s = label(c, &vn_mono_10, stateCol, state);
  lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s, LV_PCT(100));
}
static void openPlan() {
  lv_obj_t * body = sheetOpen("Plan de la tourn\xC3\xA9" "e");
  para(body, "Propos\xC3\xA9 par le serveur : arriv\xC3\xA9" "es et ruptures \xC3\xA9vit\xC3\xA9" "es ESTIM\xC3\x89" "ES. Toucher un arr\xC3\xAAt pour le faire maintenant.", HX_TEXT3, &vn_inter_11);
  int idx[vn::REG_MAX_STOPS];
  int n = vn::regStopsNow(g_reg, idx, vn::REG_MAX_STOPS);
  if (!n) para(body, "Aucun arr\xC3\xAAt propos\xC3\xA9 pour l'instant.");
  char a[96], e[24];
  for (int k = 0; k < n; k++) {
    const vn::RegStop & s = g_reg.snap.stops[idx[k]];
    lv_obj_t * r = row(body, 8);
    lv_obj_set_size(r, LV_PCT(100), 38);
    lv_obj_set_style_pad_hor(r, 6, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, C(HX_S2), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, C(HX_S3), LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, planStopCb, LV_EVENT_CLICKED, (void *)(intptr_t)idx[k]);
    char sq[6];
    snprintf(sq, sizeof(sq), "%d", k + 1);
    lv_obj_t * q = label(r, &vn_mono_12b, k == 0 ? HX_ACCENT : HX_TEXT3, sq);
    lv_obj_set_width(q, 14);
    lv_obj_t * c = col(r, 0);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_t * nm = label(c, &vn_inter_13s, HX_TEXT, s.kind == vn::RK_ATELIER ? atelierName() : s.name);
    oneLine(nm, 250);
    vn::regActText(s, a, sizeof(a));
    lv_obj_t * ac = label(c, &vn_inter_11, s.take.ok && !s.drop.ok && s.kind == vn::RK_STATION ? HX_ELEC : HX_ACCENT, a);
    oneLine(ac, 250);
    if (s.arriveAt) {
      int m = (int)((s.arriveAt - now() + 30) / 60);
      snprintf(e, sizeof(e), "~%d min", m < 0 ? 0 : m);
      label(r, &vn_mono_10, HX_TEXT2, e);
    }
  }
  eyebrow(body, "MES D\xC3\x89" "CLARATIONS");
  vn::RegTourView v;
  vn::regTourNow(g_reg, v);
  char what[64], st[96];
  int shown = 0;
  for (int i = g_reg.nq - 1; i >= 0; i--) {
    const vn::RegItem & it = g_reg.q[i];
    if (it.op != vn::RO_EVENT || strcmp(it.tour, v.id)) continue;
    if (it.impossible[0]) snprintf(what, sizeof(what), "impossible");
    else vn::regDeclText(it.kind, it.took, it.dropped, what, sizeof(what));
    eventRow(body, it.name, what, "en attente d'envoi (gard\xC3\xA9" "e dans le Sunton)", HX_WARN);
    shown++;
  }
  if (sameTour(v)) {
    for (int i = 0; i < g_reg.snap.nEvents; i++) {
      const vn::RegEvent & ev = g_reg.snap.events[i];
      if (ev.impossible[0]) snprintf(what, sizeof(what), "impossible");
      else vn::regDeclText(ev.kind, ev.took, ev.dropped, what, sizeof(what));
      vn::regVerdictText(ev, st, sizeof(st));
      if (ev.impossible[0]) { size_t l = strlen(st); snprintf(st + l, sizeof(st) - l, " \xC2\xB7 %s", ev.impossible); }
      eventRow(body, ev.name[0] ? ev.name : ev.sid, what, st, ev.verdict == vn::RV_CONCORDANT ? HX_GOOD : ev.verdict == vn::RV_ECART ? HX_WARN : HX_TEXT3);
      shown++;
    }
  }
  if (!shown) para(body, "Aucun arr\xC3\xAAt d\xC3\xA9" "clar\xC3\xA9 pour l'instant.", HX_TEXT3);
  for (int i = 0; i < g_reg.nRej; i++) {
    char b[200];
    snprintf(b, sizeof(b), "Refus\xC3\xA9 par le serveur : %s \xE2\x80\x94 %s", g_reg.rej[i].what, g_reg.rej[i].why);
    para(body, b, HX_WARN, &vn_inter_11);
  }
  if (g_reg.snap.method[0]) para(body, g_reg.snap.method, HX_TEXT3, &vn_inter_11);
  para(body, "Chaque arr\xC3\xAAt d\xC3\xA9" "clar\xC3\xA9 est recoup\xC3\xA9 avec le flux public 10 min plus tard (\xC2\xAB d\xC3\xA9" "clar\xC3\xA9 +6, observ\xC3\xA9 +6 \xC2\xBB). Rien n'est corrig\xC3\xA9 en silence.",
       HX_TEXT3, &vn_inter_11);
  lv_obj_t * b = button(body, "Terminer la tourn\xC3\xA9" "e\xE2\x80\xA6", &ic_flag_16, B_SECONDARY);
  lv_obj_set_width(b, LV_PCT(100));
  lv_obj_add_event_cb(b, [](lv_event_t *) { openEnd(); }, LV_EVENT_CLICKED, nullptr);
}

// ── Construction ─────────────────────────────────────────────────────
static void build(lv_obj_t * root) {
  // Sans tournée
  s_none = col(root, 6);
  lv_obj_set_size(s_none, CONTENT_W, CONTENT_H);
  lv_obj_set_style_pad_all(s_none, 8, 0);
  lv_obj_add_flag(s_none, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(s_none, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_none, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t * c = card(s_none, 10);
  lv_obj_set_width(c, W);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 6, 0);
  lv_obj_t * h = row(c, 6);
  icon(h, &ic_truck_16, HX_ACCENT);
  eyebrow(h, "TOURN\xC3\x89" "E DU CAMION");
  s_noneTitle = label(c, &vn_inter_15b, HX_TEXT, "");
  s_noneSub = label(c, &vn_inter_12, HX_TEXT2, "");
  lv_label_set_long_mode(s_noneSub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_noneSub, W - 20);
  s_startBtn = button(c, "Commencer une tourn\xC3\xA9" "e", &ic_play_16, B_PRIMARY);
  lv_obj_set_width(s_startBtn, W - 20);
  lv_obj_add_event_cb(s_startBtn, [](lv_event_t *) { openStart(); }, LV_EVENT_CLICKED, nullptr);
  s_openBox = col(s_none, 6);
  lv_obj_set_width(s_openBox, W);
  s_noneRej = label(s_none, &vn_inter_11, HX_WARN, "");
  lv_label_set_long_mode(s_noneRej, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_noneRej, W);
  s_noneStatus = label(s_none, &vn_inter_11, HX_TEXT3, "");
  lv_label_set_long_mode(s_noneStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_noneStatus, W);

  // Tournée en cours
  s_tour = box(root);
  lv_obj_set_size(s_tour, CONTENT_W, CONTENT_H);
  lv_obj_t * ra = row(s_tour, 6);
  lv_obj_set_pos(ra, 8, 4);
  lv_obj_set_size(ra, W, 30);
  icon(ra, &ic_truck_16, HX_ACCENT);
  s_tTitle = label(ra, &vn_inter_13s, HX_TEXT, "");
  lv_obj_set_flex_grow(s_tTitle, 1);
  lv_label_set_long_mode(s_tTitle, LV_LABEL_LONG_DOT);
  eyebrow(ra, "\xC3\x80 BORD");
  s_load = label(ra, &vn_mono_20b, HX_ACCENT, "0");
  s_loadCap = label(ra, &vn_mono_12b, HX_TEXT3, "/0");
  s_loadHs = label(ra, &vn_mono_10, HX_WARN, "");
  s_bar = meter(s_tour, W, 6);
  lv_obj_set_pos(s_bar, 8, 36);

  // Carte du prochain arrêt : positions tirées des hauteurs de ligne (rien ne se chevauche)
  const lv_coord_t l11 = lv_font_get_line_height(&vn_inter_11), l10 = lv_font_get_line_height(&vn_mono_10);
  const lv_coord_t yWhy = 64, yNow = yWhy + 2 * l11, cardH = yNow + l10 + 16;
  lv_obj_t * sc = card(s_tour, 8);
  lv_obj_set_pos(sc, 8, 46);
  lv_obj_set_size(sc, W, cardH);
  lv_obj_t * sh = row(sc, 6);
  lv_obj_set_width(sh, W - 18);
  s_stopEye = eyebrow(sh, "PROCHAIN ARR\xC3\x8AT");
  s_stopBadge = badge(sh, "PLAN ESTIM\xC3\x89", T_ACCENT);
  lv_obj_t * sp = box(sh);
  lv_obj_set_flex_grow(sp, 1);
  s_stopEta = label(sh, &vn_mono_12b, HX_TEXT, "");
  s_stopSid = label(sh, &vn_mono_10, HX_TEXT3, "");
  s_stopName = label(sc, &vn_inter_20b, HX_TEXT, "");
  lv_obj_set_pos(s_stopName, 0, 18);
  oneLine(s_stopName, W - 18);
  s_stopAct = label(sc, &vn_inter_20b, HX_ACCENT, "");
  lv_obj_set_pos(s_stopAct, 0, 40);
  oneLine(s_stopAct, W - 18);
  s_stopWhy = label(sc, &vn_inter_11, HX_TEXT2, "");
  lv_obj_set_pos(s_stopWhy, 0, yWhy);
  lv_obj_set_size(s_stopWhy, W - 18, 2 * l11);
  lv_label_set_long_mode(s_stopWhy, LV_LABEL_LONG_DOT);
  s_stopNow = label(sc, &vn_mono_10, HX_TEXT3, "");
  lv_obj_set_pos(s_stopNow, 0, yNow);
  oneLine(s_stopNow, W - 18);
  const lv_coord_t yBtn = 46 + cardH + 5;

  lv_obj_t * rb = row(s_tour, 4);
  lv_obj_set_pos(rb, 8, yBtn);
  lv_obj_set_size(rb, W, 38);
  s_btnDone = button(rb, "Fait", &ic_check_16, B_PRIMARY);
  lv_obj_set_size(s_btnDone, 112, 38);
  s_btnImp = button(rb, "Impossible", nullptr, B_SECONDARY);
  lv_obj_set_size(s_btnImp, 100, 38);
  s_btnOther = button(rb, "Autre arr\xC3\xAAt", nullptr, B_SECONDARY);
  lv_obj_set_size(s_btnOther, 100, 38);
  s_btnPlan = button(rb, "Plan", &ic_list_16, B_SECONDARY);
  lv_obj_set_size(s_btnPlan, 80, 38);
  lv_obj_add_event_cb(s_btnDone, [](lv_event_t *) {
    if (s_cur < 0) return;
    const vn::RegStop & s = g_reg.snap.stops[s_cur];
    openValidate(s.kind, s.sid, s.kind == vn::RK_ATELIER ? atelierName() : s.name, &s);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_btnImp, [](lv_event_t *) { if (s_cur >= 0) openImpossible(g_reg.snap.stops[s_cur]); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_btnOther, [](lv_event_t *) { openOther(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_btnPlan, [](lv_event_t *) { openPlan(); }, LV_EVENT_CLICKED, nullptr);
  s_info = label(s_tour, &vn_inter_11, HX_TEXT2, "");
  lv_obj_set_pos(s_info, 8, yBtn + 38 + 4);
  oneLine(s_info, W);
  lv_obj_add_flag(s_tour, LV_OBJ_FLAG_HIDDEN);
  s_seen = 0xFFFFFFFF;
}

static void joinCb(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= g_reg.snap.nOpen) return;
  vn::regJoin(g_reg, g_reg.snap.open[i].id);
  net::requestReg();
  char m[96];
  snprintf(m, sizeof(m), "%s \xC2\xB7 %s : les arr\xC3\xAAts d\xC3\xA9" "clar\xC3\xA9s ici s'y ajoutent", g_reg.snap.open[i].regName, g_reg.snap.open[i].truckName);
  toast("Tourn\xC3\xA9" "e rejointe", m, T_ACCENT, &ic_truck_16);
}

static void refreshNone() {
  const bool url = vn::g_sync.url[0] != 0;
  const vn::RegSnap & sn = g_reg.snap;
  bool canStart = false;
  if (!url) {
    lv_label_set_text(s_noneTitle, "Serveur en ligne non r\xC3\xA9gl\xC3\xA9");
    lv_label_set_text(s_noneSub, "Ce Sunton est en r\xC3\xB4le \xC2\xAB r\xC3\xA9gulateur \xC2\xBB : il lui faut l'adresse du serveur (R\xC3\xA9glages \xE2\x86\x92 Synchro en ligne), "
                                 "qui tient le registre des tourn\xC3\xA9" "es et propose le plan.");
  } else if (!sn.valid) {
    lv_label_set_text(s_noneTitle, "Connexion au serveur\xE2\x80\xA6");
    lv_label_set_text(s_noneSub, "Camions et r\xC3\xA9gulateurs sont lus sur le serveur. Sans r\xC3\xA9seau, d\xC3\xA9" "clarer sur le t\xC3\xA9l\xC3\xA9phone (page /regulateur.html).");
  } else if (!sn.nTrucks || !sn.nRegs) {
    lv_label_set_text(s_noneTitle, "Aucun camion ou r\xC3\xA9gulateur d\xC3\xA9" "clar\xC3\xA9");
    lv_label_set_text(s_noneSub, "\xC3\x80 saisir sur le serveur : tableau de bord, onglet R\xC3\xA9gulation (camions et capacit\xC3\xA9s, r\xC3\xA9gulateurs, point de l'atelier).");
  } else {
    canStart = true;
    lv_label_set_text(s_noneTitle, "Aucune tourn\xC3\xA9" "e en cours");
    lv_label_set_text(s_noneSub, "Commencer : r\xC3\xA9gulateur, camion du jour, v\xC3\xA9los d\xC3\xA9j\xC3\xA0 \xC3\xA0 bord. Chaque arr\xC3\xAAt est ensuite valid\xC3\xA9 ici : "
                                 "seuls les nombres valid\xC3\xA9s comptent comme interventions. Le plan propos\xC3\xA9 est une estimation.");
  }
  if (canStart) lv_obj_clear_flag(s_startBtn, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_startBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(s_openBox);
  for (int i = 0; sn.valid && i < sn.nOpen; i++) {
    const vn::RegOpen & o = sn.open[i];
    if (!strcmp(o.device, sn.device)) continue;
    lv_obj_t * c = card(s_openBox, 8);
    lv_obj_set_width(c, W);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 8, 0);
    lv_obj_t * t = col(c, 1);
    lv_obj_set_flex_grow(t, 1);
    char b[96];
    snprintf(b, sizeof(b), "%s \xC2\xB7 %s", o.regName, o.truckName);
    label(t, &vn_inter_13s, HX_TEXT, b);
    snprintf(b, sizeof(b), "tourn\xC3\xA9" "e en cours sur %s", o.device);
    label(t, &vn_inter_11, HX_TEXT3, b);
    lv_obj_t * jb = button(c, "Rejoindre", nullptr, B_TONAL, true);
    lv_obj_add_event_cb(jb, joinCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  if (g_reg.nRej) {
    char b[200];
    snprintf(b, sizeof(b), "Refus\xC3\xA9 par le serveur : %s \xE2\x80\x94 %s", g_reg.rej[0].what, g_reg.rej[0].why);
    lv_label_set_text(s_noneRej, b);
    lv_obj_clear_flag(s_noneRej, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(s_noneRej, LV_OBJ_FLAG_HIDDEN);
  char line[200];
  vn::regStatusLine(g_reg, url, now(), line, sizeof(line));
  lv_label_set_text(s_noneStatus, line);
}

static void refreshTour(const vn::RegTourView & v) {
  const int64_t t = now();
  char b[200], a[96];
  snprintf(b, sizeof(b), "%s \xC2\xB7 %s", v.truckName, v.regName);
  lv_label_set_text(s_tTitle, b);
  vn::RegCount l = vn::regLoadNow(g_reg);
  lv_label_set_text_fmt(s_load, "%d", l.ok);
  lv_label_set_text_fmt(s_loadCap, "/%u", (unsigned)v.cap);
  if (l.hs) lv_label_set_text_fmt(s_loadHs, "+%d HS", l.hs); else lv_label_set_text(s_loadHs, "");
  int free = v.cap - l.ok - l.hs;
  int vals[3] = { l.ok > 0 ? l.ok : 0, l.hs > 0 ? l.hs : 0, free > 0 ? free : 0 };
  lv_color_t cols[3] = { C(HX_ACCENT), C(HX_TEXT3), C(HX_S3) };
  meterSet(s_bar, vals, cols, 3);

  int idx[vn::REG_MAX_STOPS];
  int n = vn::regStopsNow(g_reg, idx, vn::REG_MAX_STOPS);
  s_cur = n ? idx[0] : -1;
  const bool live = g_reg.lastOk && t - g_reg.lastOk < 120 && !g_reg.snapFromFlash;
  if (s_cur >= 0) {
    const vn::RegStop & s = g_reg.snap.stops[s_cur];
    lv_label_set_text(s_stopEye, "PROCHAIN ARR\xC3\x8AT");
    if (live) badgeSet(s_stopBadge, "PLAN ESTIM\xC3\x89", T_ACCENT);
    else {
      char hm[8];
      fmtClock(hm, sizeof(hm), g_reg.snapAt);
      snprintf(b, sizeof(b), "PLAN DE %s", hm);
      badgeSet(s_stopBadge, b, T_WARN);
    }
    if (s.arriveAt) { int m = (int)((s.arriveAt - t + 30) / 60); lv_label_set_text_fmt(s_stopEta, "~%d min", m < 0 ? 0 : m); }
    else lv_label_set_text(s_stopEta, "");
    if (s.kind == vn::RK_STATION) lv_label_set_text_fmt(s_stopSid, "n\xC2\xB0%s", s.sid); else lv_label_set_text(s_stopSid, "");
    lv_label_set_text(s_stopName, s.kind == vn::RK_ATELIER ? atelierName() : s.name);
    vn::regActText(s, a, sizeof(a));
    capFirst(a);
    lv_label_set_text(s_stopAct, a);
    lv_obj_set_style_text_color(s_stopAct, C(s.kind == vn::RK_ATELIER ? HX_TEXT : (s.take.ok && !s.drop.ok) ? HX_ELEC : HX_ACCENT), 0);
    lv_label_set_text(s_stopWhy, s.why);
    if (s.hasNow) snprintf(b, sizeof(b), "au calcul du plan : %d v\xC3\xA9lo%s \xC2\xB7 %d borne%s libre%s \xC2\xB7 %d HS (mesur\xC3\xA9)", s.nowBikes, s.nowBikes > 1 ? "s" : "",
                           s.nowDocks, s.nowDocks > 1 ? "s" : "", s.nowDocks > 1 ? "s" : "", s.nowDis);
    else if (s.kind == vn::RK_ATELIER && g_reg.snap.atelierStock >= 0) snprintf(b, sizeof(b), "stock d\xC3\xA9" "clar\xC3\xA9 \xC3\xA0 l'atelier : %d v\xC3\xA9lo%s pr\xC3\xAAt%s",
                                                                              g_reg.snap.atelierStock, g_reg.snap.atelierStock > 1 ? "s" : "", g_reg.snap.atelierStock > 1 ? "s" : "");
    else if (s.kind == vn::RK_ATELIER) snprintf(b, sizeof(b), "stock de l'atelier non d\xC3\xA9" "clar\xC3\xA9");
    else b[0] = 0;
    lv_label_set_text(s_stopNow, b);
  } else {
    lv_label_set_text(s_stopEye, "PROCHAIN ARR\xC3\x8AT");
    badgeSet(s_stopBadge, "AUCUN", T_NEUTRAL);
    lv_label_set_text(s_stopEta, "");
    lv_label_set_text(s_stopSid, "");
    lv_label_set_text(s_stopAct, "");
    lv_label_set_text(s_stopNow, "");
    if (!sameTour(v)) {
      lv_label_set_text(s_stopName, "Plan pas encore re\xC3\xA7u");
      lv_label_set_text(s_stopWhy, "Hors connexion : d\xC3\xA9" "clarer les arr\xC3\xAAts avec \xC2\xAB Autre arr\xC3\xAAt \xC2\xBB ; ils partent d\xC3\xA8s le retour du r\xC3\xA9seau.");
    } else if (!g_reg.snap.nStops) {
      lv_label_set_text(s_stopName, "Rien \xC3\xA0 proposer pour l'instant");
      lv_label_set_text(s_stopWhy, "Aucune rupture \xC3\xA9vitable estim\xC3\xA9" "e dans les 2 h avec ce chargement. Le plan est recalcul\xC3\xA9 \xC3\xA0 chaque relev\xC3\xA9.");
    } else {
      lv_label_set_text(s_stopName, "Arr\xC3\xAAts du plan faits");
      lv_label_set_text(s_stopWhy, "En attente du nouveau plan (il part du chargement d\xC3\xA9" "clar\xC3\xA9).");
    }
  }
  btnEnable(s_btnDone, s_cur >= 0);
  btnEnable(s_btnImp, s_cur >= 0);
  // Ligne du bas : un refus récent d'abord, puis la file, puis la suite du plan, sinon l'état de la liaison
  uint32_t col = HX_TEXT2;
  if (g_reg.nRej && t - g_reg.rej[0].at < 900) {
    snprintf(b, sizeof(b), "Refus\xC3\xA9 : %s \xE2\x80\x94 %s", g_reg.rej[0].what, g_reg.rej[0].why);
    col = HX_WARN;
  } else if (g_reg.nq) {
    vn::regStatusLine(g_reg, vn::g_sync.url[0] != 0, t, b, sizeof(b));
    col = HX_WARN;
  } else if (n > 1) {
    snprintf(b, sizeof(b), "Ensuite : ");
    for (int k = 1; k < n && k < 4; k++) {
      const vn::RegStop & s = g_reg.snap.stops[idx[k]];
      vn::regActText(s, a, sizeof(a));
      size_t L = strlen(b);
      snprintf(b + L, sizeof(b) - L, "%s%s (%s)", k > 1 ? " \xC2\xB7 " : "", s.kind == vn::RK_ATELIER ? atelierName() : s.name, a);
    }
  } else {
    vn::regStatusLine(g_reg, vn::g_sync.url[0] != 0, t, b, sizeof(b));
    if (!live) col = HX_WARN;
  }
  lv_label_set_text(s_info, b);
  lv_obj_set_style_text_color(s_info, C(col), 0);
}

static void refresh() {
  if (!s_none) return;
  vn::RegTourView v;
  vn::regTourNow(g_reg, v);
  if (v.has) { lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_tour, LV_OBJ_FLAG_HIDDEN); refreshTour(v); }
  else { lv_obj_add_flag(s_tour, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_none, LV_OBJ_FLAG_HIDDEN); refreshNone(); s_cur = -1; }
  s_seen = g_reg.version;
  s_lastMinute = now() / 60;
}

// Appelé par la coquille toutes les 200 ms : l'état change sans relevé (geste, réponse du serveur)
void tourTick() {
  if (!s_none || lv_obj_has_flag(lv_obj_get_parent(s_none), LV_OBJ_FLAG_HIDDEN)) return;
  if (g_reg.version != s_seen || now() / 60 != s_lastMinute) refresh();
}

const ScreenDef SCR_TOUR = { "Tourn\xC3\xA9" "e", "TOURN\xC3\x89" "E", &ic_truck_20, build, refresh, refresh };

} // namespace ui
