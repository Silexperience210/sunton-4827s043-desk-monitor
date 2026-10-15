// ── Coquille : en-tête, rail, écrans, bottom sheet, toasts, accueil ──
#include "ui.h"
#include "ui_internal.h"
#include "../net/net.h"
#include "../hal/touch_api.h"
#include "../platform/platform.h"
#include "../config.h"
#include "../core/ops.h"
#include <stdio.h>
#include <string.h>

using vn::g_model;
using vn::g_set;

namespace ui {

// Défini par les écrans
void listSetSort(int mode);
void mapScreenFocus(int st);

static const ScreenDef * DEFS[TAB_COUNT] = { &SCR_DASH, &SCR_MAP, &SCR_LIST, &SCR_ALERTS, &SCR_SERVICE, &SCR_SETTINGS };

static lv_obj_t * s_content, * s_screens[TAB_COUNT];
static lv_obj_t * s_tabIcon[TAB_COUNT], * s_tabLabel[TAB_COUNT], * s_ind, * s_alertBadge, * s_alertBadgeLbl;
static lv_obj_t * s_sub, * s_dataPill, * s_dataDot, * s_dataIc, * s_dataLbl, * s_refreshIc;
static lv_obj_t * s_wxIc, * s_wxLbl, * s_clock;
static lv_obj_t * s_offline, * s_offlineLbl;
static lv_obj_t * s_splash, * s_splashStage, * s_splashBar;
static lv_obj_t * s_scrim, * s_sheet, * s_sheetBody;
static lv_obj_t * s_toast, * s_toastIcBox, * s_toastIc, * s_toastTitle, * s_toastMsg;
static lv_timer_t * s_toastTimer;
static int s_tab = -1;
static bool s_built[TAB_COUNT];
static uint32_t s_seenVersion = 0;
static int s_dataTone = -1;
static bool s_spinning = false;
static int s_token = 0, s_curToken = -1;

int64_t now() { return plat::now(); }

const lv_img_dsc_t * wxIcon16(int code) {
  switch (vn::wxIcon(code)) {
    case vn::WX_SUN: return &ic_sun_16;
    case vn::WX_PARTLY: return &ic_cloud_sun_16;
    case vn::WX_FOG: return &ic_fog_16;
    case vn::WX_RAIN: return &ic_rain_16;
    case vn::WX_SNOW: return &ic_snow_16;
    case vn::WX_STORM: return &ic_storm_16;
    default: return &ic_cloud_16;
  }
}
const lv_img_dsc_t * wxIcon12(int code) {
  switch (vn::wxIcon(code)) {
    case vn::WX_SUN: return &ic_sun_12;
    case vn::WX_PARTLY: return &ic_cloud_sun_12;
    case vn::WX_FOG: return &ic_fog_12;
    case vn::WX_RAIN: return &ic_rain_12;
    case vn::WX_SNOW: return &ic_snow_12;
    case vn::WX_STORM: return &ic_storm_12;
    default: return &ic_cloud_12;
  }
}

// ── Toast ────────────────────────────────────────────────────────────
static void toastHide(lv_timer_t *) {
  s_toastTimer = nullptr;
  if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

void toast(const char * title, const char * msg, Tone tone, const lv_img_dsc_t * ic) {
  if (!s_toast) {
    s_toast = row(lv_layer_top(), 10);
    lv_obj_set_width(s_toast, 300);
    lv_obj_set_style_min_height(s_toast, 44, 0);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    lv_obj_set_style_pad_left(s_toast, 10, 0);
    lv_obj_set_style_bg_color(s_toast, C(HX_S2), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_toast, OPA_BORDER_STRONG, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_shadow_width(s_toast, 24, 0);
    lv_obj_set_style_shadow_ofs_y(s_toast, 8, 0);
    lv_obj_set_style_shadow_opa(s_toast, 140, 0);
    lv_obj_set_style_shadow_color(s_toast, lv_color_black(), 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, RAIL_W / 2, HEADER_H + 6);
    s_toastIcBox = box(s_toast);
    lv_obj_set_size(s_toastIcBox, 28, 28);
    lv_obj_set_style_radius(s_toastIcBox, 6, 0);
    lv_obj_set_style_bg_opa(s_toastIcBox, LV_OPA_COVER, 0);
    s_toastIc = lv_img_create(s_toastIcBox);
    lv_obj_center(s_toastIc);
    lv_obj_t * c = col(s_toast, 1);
    lv_obj_set_flex_grow(c, 1);
    s_toastTitle = label(c, &vn_inter_13s, HX_TEXT, "");
    s_toastMsg = label(c, &vn_inter_12, HX_TEXT2, "");
    oneLine(s_toastMsg, 240);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toast, [](lv_event_t *) { lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, nullptr);
  }
  lv_color_t tc = toneColor(tone);
  lv_obj_set_style_bg_color(s_toastIcBox, tone == T_NEUTRAL ? C(HX_S3) : tc, 0);
  lv_obj_set_style_bg_opa(s_toastIcBox, tone == T_NEUTRAL ? LV_OPA_COVER : 36, 0);
  lv_img_set_src(s_toastIc, ic ? ic : &ic_info_16);
  iconColor(s_toastIc, tc);
  lv_label_set_text(s_toastTitle, title);
  lv_label_set_text(s_toastMsg, msg ? msg : "");
  lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast);
  fadeIn(s_toast, 260, -8);
  if (s_toastTimer) lv_timer_del(s_toastTimer);
  s_toastTimer = lv_timer_create(toastHide, 3800, nullptr);
  lv_timer_set_repeat_count(s_toastTimer, 1);
}

// ── Bottom sheet ─────────────────────────────────────────────────────
bool sheetIsOpen() { return s_sheet != nullptr; }
int sheetToken() { return s_sheet ? s_curToken : -1; }

void sheetClose() {
  s_curToken = -1;
  if (s_scrim) { lv_obj_del_async(s_scrim); s_scrim = nullptr; }
  if (s_sheet) { lv_obj_del_async(s_sheet); s_sheet = nullptr; s_sheetBody = nullptr; }
}

lv_obj_t * sheetOpen(const char * title) {
  sheetClose();
  s_curToken = ++s_token;
  s_scrim = box(lv_layer_top());
  lv_obj_set_size(s_scrim, 480, 272);
  lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_scrim, 115, 0);
  lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_scrim, [](lv_event_t *) { sheetClose(); }, LV_EVENT_CLICKED, nullptr);
  fadeIn(s_scrim, 180, 0);

  s_sheet = box(lv_layer_top());
  lv_obj_set_pos(s_sheet, RAIL_W + 4, 38);
  lv_obj_set_size(s_sheet, CONTENT_W - 8, 250);
  lv_obj_set_style_bg_color(s_sheet, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_sheet, lv_color_white(), 0);
  lv_obj_set_style_border_opa(s_sheet, OPA_BORDER_STRONG, 0);
  lv_obj_set_style_border_width(s_sheet, 1, 0);
  lv_obj_set_style_radius(s_sheet, 16, 0);
  lv_obj_set_style_shadow_width(s_sheet, 30, 0);
  lv_obj_set_style_shadow_ofs_y(s_sheet, -8, 0);
  lv_obj_set_style_shadow_color(s_sheet, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_sheet, 150, 0);
  lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);   // absorbe les taps (ne ferme pas)
  lv_obj_t * grab = box(s_sheet);
  lv_obj_set_size(grab, 36, 4);
  lv_obj_set_style_radius(grab, 2, 0);
  lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(grab, 46, 0);
  lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 6);
  // Fermer
  lv_obj_t * x = iconButton(s_sheet, &ic_x_16, false, 32);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_add_event_cb(x, [](lv_event_t *) { sheetClose(); }, LV_EVENT_CLICKED, nullptr);

  s_sheetBody = col(s_sheet, 8);
  lv_obj_set_pos(s_sheetBody, 0, 14);
  lv_obj_set_size(s_sheetBody, CONTENT_W - 8, 234 - 14 - 6);
  lv_obj_set_style_pad_hor(s_sheetBody, 12, 0);
  lv_obj_set_style_pad_bottom(s_sheetBody, 14, 0);
  lv_obj_add_flag(s_sheetBody, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(s_sheetBody, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_sheetBody, LV_SCROLLBAR_MODE_OFF);
  lv_obj_move_foreground(x);
  if (title && *title) {
    lv_obj_t * t = label(s_sheetBody, &vn_inter_15b, HX_TEXT, title);
    lv_obj_set_style_pad_right(t, 30, 0);
  }
  fadeIn(s_sheet, 320, 24);
  if (s_toast) lv_obj_move_foreground(s_toast);
  return s_sheetBody;
}

// ── En-tête ──────────────────────────────────────────────────────────
static void buildHeader(lv_obj_t * scr) {
  lv_obj_t * h = box(scr);
  lv_obj_set_size(h, 480, HEADER_H);
  lv_obj_set_style_bg_color(h, C(HX_BG), 0);
  lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(h, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(h, lv_color_white(), 0);
  lv_obj_set_style_border_opa(h, OPA_BORDER, 0);
  lv_obj_set_style_border_width(h, 1, 0);

  lv_obj_t * logo = lv_img_create(h);
  lv_img_set_src(logo, &img_logo_22);
  lv_obj_align(logo, LV_ALIGN_LEFT_MID, 12, 0);

  lv_obj_t * brand = lv_label_create(h);
  lv_obj_set_style_text_font(brand, &vn_mono_12b, 0);
  lv_obj_set_style_text_color(brand, C(HX_TEXT), 0);
  lv_obj_set_style_text_letter_space(brand, 2, 0);
  lv_label_set_recolor(brand, true);
  lv_label_set_text(brand, "VELOH#F5820D NAV#");
  lv_obj_set_pos(brand, 41, 5);
  s_sub = eyebrow(h, "OPS");
  lv_obj_set_pos(s_sub, 41, 20);

  lv_obj_t * right = row(h, 6);
  lv_obj_align(right, LV_ALIGN_RIGHT_MID, -10, 0);

  // Pastille données (tap = rafraîchir)
  s_dataPill = pill(right);
  lv_obj_add_flag(s_dataPill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(s_dataPill, C(HX_S3), LV_STATE_PRESSED);
  s_dataDot = dot(s_dataPill, 6, C(HX_GOOD));
  s_dataIc = icon(s_dataPill, &ic_wifi_off_12, HX_WARN);
  lv_obj_add_flag(s_dataIc, LV_OBJ_FLAG_HIDDEN);
  s_dataLbl = label(s_dataPill, &vn_mono_10, HX_GOOD, "SYNC");
  lv_obj_set_style_text_letter_space(s_dataLbl, 1, 0);
  s_refreshIc = icon(s_dataPill, &ic_refresh_12, HX_TEXT3);
  lv_img_set_pivot(s_refreshIc, 6, 6);
  lv_obj_add_event_cb(s_dataPill, [](lv_event_t *) {
    net::requestRefresh();
    toast("Actualisation", "Interrogation des flux Vel'OH! en cours", T_ACCENT, &ic_refresh_16);
  }, LV_EVENT_CLICKED, nullptr);

  // Pastille météo
  lv_obj_t * wx = pill(right);
  lv_obj_add_flag(wx, LV_OBJ_FLAG_CLICKABLE);
  s_wxIc = icon(wx, &ic_cloud_12, HX_TEXT2);
  s_wxLbl = label(wx, &vn_mono_10, HX_TEXT2, "--\xC2\xB0");
  lv_obj_add_event_cb(wx, [](lv_event_t *) {
    const vn::Weather & w = g_model.wx;
    if (!w.valid) { toast("M\xC3\xA9t\xC3\xA9o indisponible", "Open-Meteo n'a pas encore r\xC3\xA9pondu", T_WARN, &ic_cloud_16); return; }
    char t[48], m[96], sc[8];
    fmtFloat1(sc, sizeof(sc), w.score);
    snprintf(t, sizeof(t), "%s \xC2\xB7 %.0f\xC2\xB0", vn::wxLabel(w.code), w.temp);
    snprintf(m, sizeof(m), "Indice v\xC3\xA9lo %s/10 \xC2\xB7 %s", sc, w.reason);
    toast(t, m, w.score >= 7 ? T_GOOD : w.score >= 4 ? T_WARN : T_BAD, wxIcon16(w.code));
  }, LV_EVENT_CLICKED, nullptr);

  s_clock = label(right, &vn_mono_12b, HX_TEXT, "--:--");
  lv_obj_set_style_pad_left(s_clock, 4, 0);
}

static void spinExec(void * o, int32_t v) { lv_img_set_angle((lv_obj_t *)o, (int16_t)v); }

static void updateHeader() {
  net::Status ns = net::status();
  int64_t t = now();
  char b[24];
  fmtClock(b, sizeof(b), t);
  lv_label_set_text(s_clock, b);

  int tone;   // 0 live, 1 cache, 2 hors ligne, 3 sync
  if (ns.wifi != net::W_CONNECTED) tone = 2;
  else if (!g_model.lastUpdate) tone = 3;
  else if (!vn::isStale(g_model.lastUpdate, t, vn::staleThresholdS(g_set))) tone = 0;   // même seuil que le bandeau
  else tone = 1;
  if (tone != s_dataTone) {
    s_dataTone = tone;
    static const char * L[] = { "LIVE", "RETARD", "HORS LIGNE", "SYNC" };
    static const uint32_t COL[] = { HX_GOOD, HX_ACCENT, HX_WARN, HX_ACCENT };
    lv_label_set_text(s_dataLbl, L[tone]);
    lv_obj_set_style_text_color(s_dataLbl, C(COL[tone]), 0);
    lv_obj_set_style_bg_color(s_dataDot, C(COL[tone]), 0);
    lv_anim_del(s_dataDot, nullptr);
    lv_obj_set_style_shadow_opa(s_dataDot, 0, 0);
    if (tone == 0) pulse(s_dataDot, C(HX_GOOD));
    if (tone == 2) { lv_obj_add_flag(s_dataDot, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_dataIc, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_clear_flag(s_dataDot, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_dataIc, LV_OBJ_FLAG_HIDDEN); }
  }
  if (ns.fetching != s_spinning) {
    s_spinning = ns.fetching;
    lv_anim_del(s_refreshIc, spinExec);
    if (s_spinning) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, s_refreshIc);
      lv_anim_set_values(&a, 0, 3600);
      lv_anim_set_time(&a, 900);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_exec_cb(&a, spinExec);
      lv_anim_start(&a);
      iconColor(s_refreshIc, C(HX_ACCENT));
    } else {
      lv_img_set_angle(s_refreshIc, 0);
      iconColor(s_refreshIc, C(HX_TEXT3));
    }
  }
  if (g_model.wx.valid) {
    lv_img_set_src(s_wxIc, wxIcon12(g_model.wx.code));
    snprintf(b, sizeof(b), "%.0f\xC2\xB0", g_model.wx.temp);
    lv_label_set_text(s_wxLbl, b);
  }
  // Bandeau hors ligne (autres onglets : le tableau de bord a le sien, dans le flux, avec la cause)
  bool off = ns.wifi != net::W_CONNECTED && g_model.lastUpdate && s_tab != TAB_DASH;
  if (off) {
    char ago[32], hm[8], m[64];
    fmtAgo(ago, sizeof(ago), g_model.lastUpdate, t);
    fmtClock(hm, sizeof(hm), g_model.lastUpdate);
    snprintf(m, sizeof(m), "depuis %s \xC2\xB7 donn\xC3\xA9" "es %s", hm, ago);
    lv_label_set_text(s_offlineLbl, m);
    lv_obj_clear_flag(s_offline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_offline);
  } else lv_obj_add_flag(s_offline, LV_OBJ_FLAG_HIDDEN);
}

// ── Bandeaux d'avertissement ─────────────────────────────────────────
lv_obj_t * warnStrip(lv_obj_t * parent, const lv_img_dsc_t * ic, const char * title, lv_obj_t ** titleOut, lv_obj_t ** msgOut) {
  lv_obj_t * s = row(parent, 8);
  lv_obj_set_height(s, 26);
  lv_obj_set_style_pad_hor(s, 10, 0);
  lv_obj_set_style_radius(s, 8, 0);
  lv_obj_set_style_bg_color(s, C(HX_WARN_BG), 0);
  lv_obj_set_style_bg_opa(s, 245, 0);
  lv_obj_set_style_border_color(s, C(HX_WARN), 0);
  lv_obj_set_style_border_opa(s, 90, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  icon(s, ic, HX_WARN);
  lv_obj_t * t = label(s, &vn_inter_13s, HX_WARN, title);
  lv_obj_t * m = label(s, &vn_inter_12, HX_TEXT2, "");
  lv_obj_set_flex_grow(m, 1);
  lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
  lv_obj_set_height(m, lv_font_get_line_height(&vn_inter_12));
  if (titleOut) *titleOut = t;
  if (msgOut) *msgOut = m;
  return s;
}

// État des chiffres (core/ops : la règle et ses tests) : aucune donnée, hors ligne,
// relevé vieux, météo vieille. Le titre et le message disent la cause, jamais un 0.
bool freshness(int64_t t, char * title, size_t tn, char * msg, size_t mn, int * key, const lv_img_dsc_t ** ic) {
  net::Status ns = net::status();
  int thr = vn::staleThresholdS(g_set), wthr = vn::wxStaleThresholdS();
  vn::DataState ds = vn::dataState(g_model.lastUpdate, t, thr, ns.wifi == net::W_CONNECTED);
  bool st = ds == vn::DS_STALE;
  bool wx = g_model.wx.valid && vn::isStale(g_model.wx.fetchedAt, t, wthr);
  int am = st ? (int)(vn::dataAgeS(g_model.lastUpdate, t) / 60) : -1;
  int wm = wx ? (int)(vn::dataAgeS(g_model.wx.fetchedAt, t) / 60) : -1;
  if (ic) *ic = &ic_clock_16;
  char a[24], b[24];
  if (ds == vn::DS_NONE || ds == vn::DS_OFFLINE) {
    if (ds == vn::DS_NONE) snprintf(title, tn, "Aucune donn\xC3\xA9" "e");
    else { fmtClock(a, sizeof(a), g_model.lastUpdate); snprintf(title, tn, "Hors ligne depuis %s", a); }
    vn::offlineCause(msg, mn, ns.wifi, ns.ssid, ns.wifiReason, ns.timeOk, ns.feed[vn::FEED_STATUS]);
    if (ic && ns.wifi != net::W_CONNECTED) *ic = &ic_wifi_off_16;
    if (key) {                                       // le texte change → on redessine
      uint32_t h = 2166136261u;
      for (const char * p = title; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
      for (const char * p = msg; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
      *key = (int)(h & 0x7FFFFFFF) | 0x40000000;
    }
    return true;
  }
  if (key) *key = (am + 1) * 100000 + (wm + 1) + thr;
  if (!st && !wx) return false;
  if (st) {
    fmtDur(a, sizeof(a), am);
    snprintf(title, tn, "Donn\xC3\xA9" "es vieilles de %s", a);
    fmtDur(a, sizeof(a), thr / 60);
    if (wx) { fmtDur(b, sizeof(b), wm); snprintf(msg, mn, "seuil %s \xC2\xB7 m\xC3\xA9t\xC3\xA9o : %s", a, b); }
    else snprintf(msg, mn, "station_status \xC2\xB7 seuil %s", a);
  } else {
    fmtDur(a, sizeof(a), wm);
    snprintf(title, tn, "M\xC3\xA9t\xC3\xA9o vieille de %s", a);
    fmtDur(a, sizeof(a), wthr / 60);
    snprintf(msg, mn, "Open-Meteo \xC2\xB7 seuil %s", a);
  }
  return true;
}

// ── Rail ─────────────────────────────────────────────────────────────
static lv_coord_t tabY(int i) { return HEADER_H + i * (CONTENT_H / TAB_COUNT); }

static void buildRail(lv_obj_t * scr) {
  lv_obj_t * r = box(scr);
  lv_obj_set_pos(r, 0, HEADER_H);
  lv_obj_set_size(r, RAIL_W, CONTENT_H);
  lv_obj_set_style_bg_color(r, C(HX_BG), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(r, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_color(r, lv_color_white(), 0);
  lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_coord_t th = CONTENT_H / TAB_COUNT;
  for (int i = 0; i < TAB_COUNT; i++) {
    lv_obj_t * t = box(r);
    lv_obj_set_pos(t, 0, i * th);
    lv_obj_set_size(t, RAIL_W - 1, th);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t, C(HX_S2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
    s_tabIcon[i] = icon(t, DEFS[i]->icon, HX_TEXT3);
    lv_obj_align(s_tabIcon[i], LV_ALIGN_TOP_MID, 0, 3);       // six onglets de 39 px : icône et libellé serrés
    s_tabLabel[i] = label(t, &vn_inter_11, HX_TEXT3, DEFS[i]->label);
    lv_obj_align(s_tabLabel[i], LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_event_cb(t, [](lv_event_t * e) { showTab((int)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (i == TAB_ALERTS) {
      s_alertBadge = box(t);
      lv_obj_set_height(s_alertBadge, 14);
      lv_obj_set_style_min_width(s_alertBadge, 14, 0);
      lv_obj_set_style_pad_hor(s_alertBadge, 3, 0);
      lv_obj_set_style_radius(s_alertBadge, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(s_alertBadge, C(HX_BAD), 0);
      lv_obj_set_style_bg_opa(s_alertBadge, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(s_alertBadge, C(HX_BG), 0);
      lv_obj_set_style_border_width(s_alertBadge, 2, 0);
      s_alertBadgeLbl = label(s_alertBadge, &vn_mono_10, 0xFFFFFF, "0");
      lv_obj_center(s_alertBadgeLbl);
      lv_obj_align(s_alertBadge, LV_ALIGN_TOP_MID, 11, 2);
      lv_obj_add_flag(s_alertBadge, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Indicateur lumineux (vn-tabbar__ind, version verticale)
  s_ind = box(scr);
  lv_obj_set_size(s_ind, 3, 26);
  lv_obj_set_style_radius(s_ind, 2, 0);
  lv_obj_set_style_bg_color(s_ind, C(HX_ACCENT), 0);
  lv_obj_set_style_bg_opa(s_ind, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(s_ind, C(HX_ACCENT), 0);
  lv_obj_set_style_shadow_width(s_ind, 12, 0);
  lv_obj_set_style_shadow_opa(s_ind, 170, 0);
  lv_obj_set_pos(s_ind, 0, tabY(0) + (th - 26) / 2);
}

static void updateRailBadge() {
  int crit = g_model.alertCount[vn::AL_EMPTY] + g_model.alertCount[vn::AL_FULL];
  if (crit > 0) {
    lv_label_set_text_fmt(s_alertBadgeLbl, "%d", crit > 99 ? 99 : crit);
    lv_obj_clear_flag(s_alertBadge, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(s_alertBadge, LV_OBJ_FLAG_HIDDEN);
}

void showTab(int tab) {
  if (tab < 0 || tab >= TAB_COUNT) return;
  if (tab == s_tab) return;
  if (!s_built[tab]) {
    s_screens[tab] = box(s_content);
    lv_obj_set_size(s_screens[tab], CONTENT_W, CONTENT_H);
    DEFS[tab]->build(s_screens[tab]);
    s_built[tab] = true;
  }
  for (int i = 0; i < TAB_COUNT; i++) {
    if (s_screens[i]) {
      if (i == tab) lv_obj_clear_flag(s_screens[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(s_screens[i], LV_OBJ_FLAG_HIDDEN);
    }
    iconColor(s_tabIcon[i], C(i == tab ? HX_ACCENT : HX_TEXT3));
    lv_obj_set_style_text_color(s_tabLabel[i], C(i == tab ? HX_TEXT : HX_TEXT3), 0);
  }
  lv_coord_t th = CONTENT_H / TAB_COUNT;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_ind);
  lv_anim_set_values(&a, lv_obj_get_y(s_ind), tabY(tab) + (th - 26) / 2);
  lv_anim_set_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void * o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, (lv_coord_t)v); });
  lv_anim_start(&a);
  char sub[48];
  snprintf(sub, sizeof(sub), "OPS \xC2\xB7 %s", DEFS[tab]->sub);
  lv_label_set_text(s_sub, sub);
  s_tab = tab;
  if (DEFS[tab]->shown) DEFS[tab]->shown();
  if (DEFS[tab]->refresh) DEFS[tab]->refresh();
  fadeIn(s_screens[tab], 260, 6);
}

void openStationsFiltered(int sortMode) { showTab(TAB_LIST); listSetSort(sortMode); }
void mapFocus(int st) { showTab(TAB_MAP); mapScreenFocus(st); }

// ── Écran d'accueil ──────────────────────────────────────────────────
static void buildSplash() {
  s_splash = box(lv_layer_top());
  lv_obj_set_size(s_splash, 480, 272);
  lv_obj_set_style_bg_color(s_splash, C(HX_BG), 0);
  lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_splash, LV_OBJ_FLAG_CLICKABLE);
  // halo radial discret derrière le logo
  lv_obj_t * glow = box(s_splash);
  lv_obj_set_size(glow, 120, 120);
  lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(glow, C(HX_ACCENT), 0);
  lv_obj_set_style_bg_opa(glow, 18, 0);
  lv_obj_set_style_shadow_color(glow, C(HX_ACCENT), 0);
  lv_obj_set_style_shadow_width(glow, 60, 0);
  lv_obj_set_style_shadow_opa(glow, 40, 0);
  lv_obj_align(glow, LV_ALIGN_CENTER, 0, -44);
  lv_obj_t * logo = lv_img_create(s_splash);
  lv_img_set_src(logo, &img_logo_56);
  lv_obj_align(logo, LV_ALIGN_CENTER, 0, -44);
  lv_obj_t * brand = lv_label_create(s_splash);
  lv_obj_set_style_text_font(brand, &vn_mono_20b, 0);
  lv_obj_set_style_text_color(brand, C(HX_TEXT), 0);
  lv_obj_set_style_text_letter_space(brand, 4, 0);
  lv_label_set_recolor(brand, true);
  lv_label_set_text(brand, "VELOH#F5820D NAV#");
  lv_obj_align(brand, LV_ALIGN_CENTER, 0, 6);
  lv_obj_t * sub = eyebrow(s_splash, "OPS \xC2\xB7 SUPERVISION DU PARC VEL'OH!");
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);
  lv_obj_t * track = box(s_splash);
  lv_obj_set_size(track, 180, 4);
  lv_obj_set_style_radius(track, 2, 0);
  lv_obj_set_style_bg_color(track, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(track, 20, 0);
  lv_obj_set_style_clip_corner(track, true, 0);
  lv_obj_align(track, LV_ALIGN_CENTER, 0, 52);
  s_splashBar = box(track);
  lv_obj_set_size(s_splashBar, 63, 4);
  lv_obj_set_style_radius(s_splashBar, 2, 0);
  lv_obj_set_style_bg_color(s_splashBar, C(HX_ACCENT), 0);
  lv_obj_set_style_bg_grad_color(s_splashBar, C(HX_ACCENT_HOT), 0);
  lv_obj_set_style_bg_grad_dir(s_splashBar, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_bg_opa(s_splashBar, LV_OPA_COVER, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_splashBar);
  lv_anim_set_values(&a, -63, 180);
  lv_anim_set_time(&a, 1200);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, [](void * o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, (lv_coord_t)v); });
  lv_anim_start(&a);
  s_splashStage = label(s_splash, &vn_inter_12, HX_TEXT2, "D\xC3\xA9marrage\xE2\x80\xA6");
  lv_obj_align(s_splashStage, LV_ALIGN_CENTER, 0, 72);
  lv_obj_t * ver = label(s_splash, &vn_mono_10, HX_TEXT3, "v" VN_VERSION " \xC2\xB7 GBFS v3 \xC2\xB7 ESP32-4827S043");
  lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void splashClose() {
  if (!s_splash) return;
  lv_anim_del(s_splashBar, nullptr);
  lv_obj_del(s_splash);
  s_splash = nullptr;
  s_splashBar = nullptr;
}

// ── Boucle UI ────────────────────────────────────────────────────────
static void afterSplash() {
  // Tactile résistif jamais étalonné : étalonnage d'abord (il conditionne tout le reste)
  if (hal::touchIsResistive() && !g_set.tcal.valid) openTouchTest(true);
}

static void tick(lv_timer_t *) {
  net::Status ns = net::status();
  if (s_splash) {
    lv_label_set_text(s_splashStage, ns.stage ? ns.stage : "");
    if (g_model.lastUpdate) { splashClose(); showTab(s_tab < 0 ? TAB_DASH : s_tab); afterSplash(); }
    else if (!g_set.ssid[0] && lv_tick_get() > 1500) { splashClose(); showTab(TAB_DASH); openWifi(); afterSplash(); }
    else if (ns.wifi == net::W_FAILED) {
      // Réseau enregistré mais injoignable : ne pas rester bloqué sur l'accueil
      splashClose(); showTab(TAB_DASH); openWifi(); afterSplash();
      toast("Wi-Fi injoignable", "V\xC3\xA9rifier le r\xC3\xA9seau ou le mot de passe", T_WARN, &ic_wifi_off_16);
    } else if (lv_tick_get() > 45000) {
      // Toujours rien après 45 s (flux indisponible…) : interface accessible, bandeau « hors ligne »
      splashClose(); showTab(TAB_DASH); afterSplash();
    }
  }
  // Appui continu de 6 s n'importe où : test du tactile (secours si le tactile est décalé)
  {
    static uint32_t pressSince = 0;
    hal::TouchSample ts = hal::touchLast();
    if (ts.pressed && !touchTestOpen()) {
      if (!pressSince) pressSince = lv_tick_get();
      else if (lv_tick_elaps(pressSince) > 6000) { pressSince = 0; openTouchTest(false); }
    } else pressSince = 0;
  }
  updateHeader();
  if (s_built[TAB_DASH]) dashFreshTick();
  // Nouvelles ruptures → toast (regroupées)
  if (g_model.nEv) {
    if (g_set.alertToasts && !s_splash && g_model.ev[0].st >= 0 && g_model.ev[0].st < g_model.n) {
      const vn::Station & s = g_model.st[g_model.ev[0].st];
      bool empty = g_model.ev[0].type == vn::AL_EMPTY;
      char t[64], m[80];
      snprintf(t, sizeof(t), "%s", s.name);
      if (g_model.nEv > 1) snprintf(m, sizeof(m), "%s \xC2\xB7 et %d autre%s rupture%s", empty ? "Vient de se vider" : "Vient de saturer",
                                   g_model.nEv - 1, g_model.nEv > 2 ? "s" : "", g_model.nEv > 2 ? "s" : "");
      else snprintf(m, sizeof(m), "%s", empty ? "Vient de se vider \xE2\x80\x94 plus aucun v\xC3\xA9lo" : "Vient de saturer \xE2\x80\x94 plus aucune borne");
      toast(t, m, empty ? T_BAD : T_TRANSIT, empty ? &ic_alert_16 : &ic_dock_16);
    }
    g_model.nEv = 0;
  }
  if (g_model.version != s_seenVersion) {
    s_seenVersion = g_model.version;
    updateRailBadge();
    if (s_tab >= 0 && DEFS[s_tab]->refresh) DEFS[s_tab]->refresh();
    stationSheetRefresh();
    opsSheetRefresh();
  }
}

#ifdef VN_SIM
lv_obj_t * debugToast() { return s_toast; }
int debugTab() { return s_tab; }
#endif

void init(lv_disp_t * disp) {
  themeInit(disp);
  lv_obj_t * scr = lv_disp_get_scr_act(disp);
  lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_SCROLLABLE);
  buildHeader(scr);
  buildRail(scr);
  s_content = box(scr);
  lv_obj_set_pos(s_content, RAIL_W, HEADER_H);
  lv_obj_set_size(s_content, CONTENT_W, CONTENT_H);
  // Bandeau « hors ligne » (vn-strip warn)
  s_offline = warnStrip(scr, &ic_wifi_off_16, "Hors ligne", nullptr, &s_offlineLbl);
  lv_obj_set_pos(s_offline, RAIL_W + 8, HEADER_H + 6);
  lv_obj_set_width(s_offline, CONTENT_W - 16);
  lv_obj_add_flag(s_offline, LV_OBJ_FLAG_HIDDEN);

  buildSplash();
  lv_timer_create(tick, 200, nullptr);       // la sauvegarde périodique est faite par la tâche réseau
}

} // namespace ui
