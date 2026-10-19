// ── Sheets : transferts suggérés, configuration Wi-Fi ────────────────
#include "ui_internal.h"
#include "../net/net.h"
#include <stdio.h>
#include <string.h>

using vn::g_model;
using vn::g_set;

namespace ui {

// ── Plan de régulation ───────────────────────────────────────────────
static void trClick(lv_event_t * e) {
  int st = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (st >= 0) openStation(st);
}

void openTransfers() {
  lv_obj_t * body = sheetOpen("Transferts sugg\xC3\xA9r\xC3\xA9s");
  const lv_coord_t BW = CONTENT_W - 8 - 24;
  lv_obj_t * sub = label(body, &vn_inter_12, HX_TEXT2,
    "Du donneur le plus proche vers les stations en tension, pour viser 50 % de remplissage \xC3\xA0 +1 h.");
  lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(sub, BW - 30);
  if (!g_model.nTr) {
    lv_obj_t * e = col(body, 4);
    lv_obj_set_width(e, BW);
    lv_obj_set_flex_align(e, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(e, 18, 0);
    icon(e, &ic_check_20, HX_GOOD);
    label(e, &vn_inter_13s, HX_TEXT, "Aucun transfert n\xC3\xA9" "cessaire");
    label(e, &vn_inter_12, HX_TEXT2, "Les stations en tension n'ont pas de donneur proche, ou le r\xC3\xA9seau est \xC3\xA9quilibr\xC3\xA9.");
    return;
  }
  lv_obj_t * list = card(body, 0);
  lv_obj_set_width(list, BW);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  int total = 0;
  float dist = 0;
  for (int k = 0; k < g_model.nTr; k++) {
    const vn::Transfer & t = g_model.tr[k];
    total += t.n; dist += t.distM;
    lv_obj_t * r = row(list, 8);
    lv_obj_set_size(r, LV_PCT(100), 50);
    lv_obj_set_style_pad_hor(r, 10, 0);
    if (k) {
      lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
      lv_obj_set_style_border_color(r, lv_color_white(), 0);
      lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
      lv_obj_set_style_border_width(r, 1, 0);
    }
    lv_obj_t * num = box(r);
    lv_obj_set_size(num, 22, 22);
    lv_obj_set_style_radius(num, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(num, C(HX_ACCENT), 0);
    lv_obj_set_style_bg_opa(num, 40, 0);
    lv_obj_t * nl = label(num, &vn_mono_10, HX_ACCENT, "");
    lv_label_set_text_fmt(nl, "%d", k + 1);
    lv_obj_center(nl);
    lv_obj_t * c = col(r, 1);
    lv_obj_set_flex_grow(c, 1);
    for (int side = 0; side < 2; side++) {     // ligne « donneur » puis « receveur », chacune ouvre sa fiche
      int st = side ? t.to : t.from;
      lv_obj_t * ln = row(c, 4);
      lv_obj_set_size(ln, 250, 21);
      lv_obj_add_flag(ln, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(ln, 2);
      lv_obj_set_style_radius(ln, 4, 0);
      lv_obj_set_style_bg_color(ln, C(HX_S2), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, LV_STATE_PRESSED);
      icon(ln, side ? &ic_arrow_right_12 : &ic_arrow_up_12, side ? HX_GOOD : HX_TRANSIT);
      lv_obj_t * nl2 = label(ln, &vn_inter_13s, HX_TEXT, g_model.st[st].name);
      oneLine(nl2, 230);
      lv_obj_set_user_data(ln, (void *)(intptr_t)st);
      lv_obj_add_event_cb(ln, trClick, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t * rc = col(r, 1);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    char b[24];
    snprintf(b, sizeof(b), "%d V\xC3\x89LOS", t.n);
    badge(rc, b, T_ACCENT);
    fmtDist(b, sizeof(b), t.distM);
    label(rc, &vn_mono_10, HX_TEXT3, b);
  }
  char d[24], m[96];
  fmtDist(d, sizeof(d), dist);
  snprintf(m, sizeof(m), "%d v\xC3\xA9los \xC3\xA0 d\xC3\xA9placer \xC2\xB7 %s cumul\xC3\xA9s (vol d'oiseau)", total, d);
  label(body, &vn_inter_12, HX_TEXT2, m);
}

// ── Wi-Fi ────────────────────────────────────────────────────────────
static lv_obj_t * s_wList, * s_wStatus, * s_wTa, * s_kb, * s_wSel, * s_wHead;
static lv_timer_t * s_wTimer;
static char s_selSsid[33];
static int s_wToken = -1;

static void kbClose() {
  if (s_kb) { lv_obj_t * k = s_kb; s_kb = nullptr; lv_obj_del_async(k); }
}

static void doConnect() {
  if (!s_selSsid[0]) return;
  const char * pass = s_wTa ? lv_textarea_get_text(s_wTa) : "";
  // Rien n'est enregistré ici : une faute de frappe deviendrait le réseau du démarrage et la
  // carte resterait à s'associer sans pouvoir en balayer un autre. La tâche réseau enregistre
  // le réseau une fois l'association réussie.
  net::connect(s_selSsid, pass);
  // Ne PAS appeler kbClose() avant sheetClose() : les deux passent par lv_obj_del_async, et
  // LVGL empile les minuteurs en tête (lv_timer.c:171) — la feuille, créée en second, est donc
  // détruite la PREMIÈRE, avec le clavier dans ses enfants. Le rappel différé du clavier
  // s'exécute ensuite sur un objet déjà libéré : Guru Meditation (LoadProhibited) garanti.
  // La feuille emporte le clavier ; son rappel LV_EVENT_DELETE remet s_kb à zéro.
  sheetClose();
  char m[64];
  snprintf(m, sizeof(m), "Connexion \xC3\xA0 %s en cours\xE2\x80\xA6", s_selSsid);
  toast("Wi-Fi", m, T_ACCENT, &ic_wifi_16);
}

static void pickNetwork(const char * ssid, bool secure) {
  strncpy(s_selSsid, ssid, sizeof(s_selSsid) - 1);
  s_selSsid[sizeof(s_selSsid) - 1] = 0;
  lv_label_set_text_fmt(s_wSel, "R\xC3\xA9seau : %s", s_selSsid);
  lv_obj_clear_flag(s_wSel, LV_OBJ_FLAG_HIDDEN);
  if (!secure) { lv_textarea_set_text(s_wTa, ""); doConnect(); return; }
  lv_obj_clear_flag(s_wTa, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_state(s_wTa, LV_STATE_FOCUSED);
  lv_obj_add_flag(s_wList, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_wHead, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(lv_obj_get_parent(s_wTa), 0, LV_ANIM_OFF);
  lv_textarea_set_text(s_wTa, strcmp(s_selSsid, g_set.ssid) == 0 ? g_set.pass : "");
    kbClose();
  // Clavier dans le panneau du sheet : supprimé avec lui
  lv_obj_t * sheet = lv_obj_get_parent(lv_obj_get_parent(s_wTa));
  s_kb = lv_keyboard_create(sheet);
  lv_obj_set_size(s_kb, CONTENT_W - 10, 126);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -(250 - (272 - 38)));
  lv_obj_add_event_cb(s_kb, [](lv_event_t *) { s_kb = nullptr; }, LV_EVENT_DELETE, nullptr);
  lv_obj_set_style_bg_color(s_kb, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_kb, lv_color_white(), 0);
  lv_obj_set_style_border_opa(s_kb, OPA_BORDER_STRONG, 0);
  lv_obj_set_style_border_side(s_kb, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_pad_all(s_kb, 4, 0);
  lv_obj_set_style_pad_gap(s_kb, 4, 0);
  lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_14, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_kb, C(HX_S3), LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_kb, C(HX_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_radius(s_kb, 6, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_kb, C(HX_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(s_kb, C(HX_S2), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_keyboard_set_textarea(s_kb, s_wTa);
  lv_obj_add_event_cb(s_kb, [](lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) doConnect();
    else if (c == LV_EVENT_CANCEL) {
      kbClose();
      lv_obj_clear_flag(s_wList, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_wHead, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_wTa, LV_OBJ_FLAG_HIDDEN);
    }
  }, LV_EVENT_ALL, nullptr);
}

static void netClick(lv_event_t * e) {
  lv_obj_t * r = lv_event_get_current_target(e);
  lv_obj_t * nm = lv_obj_get_child(r, 1);
  bool secure = (bool)(intptr_t)lv_obj_get_user_data(r);
  pickNetwork(lv_label_get_text(nm), secure);
}

static void scanPoll(lv_timer_t *) {
  if (sheetToken() != s_wToken) {        // sheet fermé (le clavier est parti avec lui)
    if (s_wTimer) { lv_timer_del(s_wTimer); s_wTimer = nullptr; }
    return;
  }
  int count = 0;
  if (!net::scanDone(count)) return;
  lv_timer_del(s_wTimer);
  s_wTimer = nullptr;
  lv_obj_clean(s_wList);
  if (count < 0) { lv_label_set_text(s_wStatus, "\xC3\x89" "chec de la recherche"); return; }
  static vn::WifiList list;                // 40 lignes : hors de la pile LVGL
  net::scanResults(list);
  const vn::WifiNet * items = list.net;
  int n = list.n;
  // Les réseaux masqués n'ont pas de nom à toucher : on les compte dans l'en-tête,
  // jamais en ligne vide. Les écartés (au-delà de VN_SCAN_MAX) sont les plus faibles.
  char extra[48] = "";
  if (list.hidden) snprintf(extra, sizeof(extra), " \xC2\xB7 %d masqu\xC3\xA9%s", list.hidden, list.hidden > 1 ? "s" : "");
  if (list.dropped) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " \xC2\xB7 +%d plus faible%s", list.dropped, list.dropped > 1 ? "s" : "");
  if (!n) lv_label_set_text_fmt(s_wStatus, "Aucun r\xC3\xA9seau trouv\xC3\xA9%s", extra);
  else lv_label_set_text_fmt(s_wStatus, "%d r\xC3\xA9seau%s \xC3\xA0 port\xC3\xA9" "e%s", n, n > 1 ? "x" : "", extra);
  for (int i = 0; i < n; i++) {
    lv_obj_t * r = row(s_wList, 10);
    lv_obj_set_size(r, LV_PCT(100), 38);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, C(HX_S2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    if (i) {
      lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
      lv_obj_set_style_border_color(r, lv_color_white(), 0);
      lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
      lv_obj_set_style_border_width(r, 1, 0);
    }
    int q = items[i].rssi >= -55 ? 3 : items[i].rssi >= -67 ? 2 : items[i].rssi >= -78 ? 1 : 0;
    icon(r, &ic_wifi_16, q >= 2 ? HX_GOOD : q == 1 ? HX_WARN : HX_BAD);
    lv_obj_t * nm = label(r, &vn_inter_13s, HX_TEXT, items[i].ssid);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_t * rs = label(r, &vn_mono_10, HX_TEXT3, "");
    lv_label_set_text_fmt(rs, "%d dBm", items[i].rssi);
    if (items[i].secure) icon(r, &ic_lock_12, HX_TEXT3);
    lv_obj_set_user_data(r, (void *)(intptr_t)items[i].secure);
    lv_obj_add_event_cb(r, netClick, LV_EVENT_CLICKED, nullptr);
  }
}

void openWifi() {
  lv_obj_t * body = sheetOpen("R\xC3\xA9seau Wi-Fi");
  s_wToken = sheetToken();
  s_selSsid[0] = 0;
  s_kb = nullptr;
  const lv_coord_t BW = CONTENT_W - 8 - 24;
  lv_obj_t * hr = row(body, 8);
  s_wHead = hr;
  lv_obj_set_width(hr, BW);
  s_wStatus = label(hr, &vn_inter_12, HX_TEXT2, "Recherche des r\xC3\xA9seaux\xE2\x80\xA6");
  lv_obj_set_flex_grow(s_wStatus, 1);
  lv_obj_t * rb = button(hr, "Actualiser", &ic_refresh_16, B_SECONDARY, true);
  lv_obj_add_event_cb(rb, [](lv_event_t *) {
    if (s_wTimer) return;
    lv_obj_clean(s_wList);
    lv_label_set_text(s_wStatus, "Recherche des r\xC3\xA9seaux\xE2\x80\xA6");
    net::requestScan();
    s_wTimer = lv_timer_create(scanPoll, 300, nullptr);
  }, LV_EVENT_CLICKED, nullptr);
  s_wSel = label(body, &vn_inter_13s, HX_ACCENT, "");
  lv_obj_add_flag(s_wSel, LV_OBJ_FLAG_HIDDEN);
  s_wTa = lv_textarea_create(body);
  lv_textarea_set_one_line(s_wTa, true);
  lv_textarea_set_password_mode(s_wTa, false);
  lv_textarea_set_placeholder_text(s_wTa, "Mot de passe");
  lv_obj_set_width(s_wTa, BW);
  lv_obj_set_style_bg_color(s_wTa, C(HX_S2), 0);
  lv_obj_set_style_border_color(s_wTa, C(HX_ACCENT), 0);
  lv_obj_set_style_border_opa(s_wTa, 180, 0);
  lv_obj_set_style_text_color(s_wTa, C(HX_TEXT), 0);
  lv_obj_set_style_text_font(s_wTa, &vn_inter_13s, 0);
  lv_obj_set_style_radius(s_wTa, 8, 0);
  lv_obj_add_flag(s_wTa, LV_OBJ_FLAG_HIDDEN);
  s_wList = card(body, 0);
  lv_obj_set_width(s_wList, BW);
  lv_obj_set_flex_flow(s_wList, LV_FLEX_FLOW_COLUMN);
  net::requestScan();
  if (s_wTimer) lv_timer_del(s_wTimer);
  s_wTimer = lv_timer_create(scanPoll, 300, nullptr);
}

} // namespace ui
