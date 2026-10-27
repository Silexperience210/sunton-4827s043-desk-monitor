// ── Écran « Réglages » : réseau, seuils, affichage, modèle, système ───
#include "ui_internal.h"
#include "../net/net.h"
#include "../hal/touch_api.h"
#include "../platform/platform.h"
#include "../config.h"
#include "../core/ops.h"
#include "../core/sync.h"
#include "../core/regul.h"
#include <stdio.h>
#include <string.h>

using vn::g_model;
using vn::g_set;

namespace ui {

static lv_obj_t * s_wifiSub, * s_srcSub, * s_srcBadge, * s_refSub, * s_learnSub, * s_learnBar, * s_confSub;
static lv_obj_t * s_memSub, * s_fsSub, * s_upSub, * s_resetBtn, * s_touchSub;
static lv_obj_t * s_freshSub, * s_ingSub;
static lv_obj_t * s_syncUrlSub, * s_syncSub, * s_devSub;
static uint32_t s_resetArmed = 0;

static lv_obj_t * section(lv_obj_t * parent, const char * title) {
  lv_obj_t * t = eyebrow(parent, title);
  lv_obj_set_style_pad_top(t, 6, 0);
  lv_obj_set_style_pad_left(t, 2, 0);
  lv_obj_t * c = card(parent, 0);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  return c;
}

// Ligne .vn-row : icône, titre, sous-titre, contrôle à droite (renvoie la ligne ; *sub = sous-titre)
static lv_obj_t * srow(lv_obj_t * sec, const lv_img_dsc_t * ic, const char * title, const char * sub, lv_obj_t ** subOut,
                       lv_coord_t subW = 210) {
  lv_obj_t * r = row(sec, 10);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_style_min_height(r, 46, 0);
  lv_obj_set_style_pad_hor(r, 10, 0);
  lv_obj_set_style_pad_ver(r, 6, 0);
  if (lv_obj_get_child_cnt(sec) > 1) {
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(r, lv_color_white(), 0);
    lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
    lv_obj_set_style_border_width(r, 1, 0);
  }
  lv_obj_t * ib = box(r);
  lv_obj_set_size(ib, 26, 26);
  lv_obj_set_style_radius(ib, 6, 0);
  lv_obj_set_style_bg_color(ib, C(HX_S2), 0);
  lv_obj_set_style_bg_opa(ib, LV_OPA_COVER, 0);
  lv_obj_t * i = icon(ib, ic, HX_TEXT2);
  lv_obj_center(i);
  lv_obj_t * c = col(r, 1);
  lv_obj_set_flex_grow(c, 1);
  label(c, &vn_inter_13s, HX_TEXT, title);
  lv_obj_t * s = label(c, &vn_inter_11, HX_TEXT2, sub ? sub : "");
  lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s, subW);
  if (subOut) *subOut = s;
  return r;
}

// Pas-à-pas [−] valeur [+] lié à un champ uint8_t des réglages
struct Stepper { uint8_t * field; uint8_t lo, hi; lv_obj_t * val; const char * unit; };
static Stepper s_steps[6];
static int s_nSteps = 0;

static void stepApply(Stepper & s, int delta) {
  int v = *s.field + delta;
  if (v < s.lo) v = s.lo;
  if (v > s.hi) v = s.hi;
  *s.field = (uint8_t)v;
  lv_label_set_text_fmt(s.val, "%d%s", v, s.unit);
  vn::settingsSave();
  g_model.recompute(now());
}

static void stepper(lv_obj_t * parent, uint8_t * field, uint8_t lo, uint8_t hi, const char * unit) {
  Stepper & s = s_steps[s_nSteps];
  s.field = field; s.lo = lo; s.hi = hi; s.unit = unit;
  lv_obj_t * r = row(parent, 2);
  lv_obj_set_style_bg_color(r, C(HX_S2), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(r, 8, 0);
  lv_obj_set_style_border_color(r, lv_color_white(), 0);
  lv_obj_set_style_border_opa(r, OPA_BORDER, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_t * m = iconButton(r, &ic_minus_12, false, 28);
  s.val = label(r, &vn_mono_12b, HX_TEXT, "");
  lv_obj_set_width(s.val, 32);
  lv_obj_set_style_text_align(s.val, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text_fmt(s.val, "%d%s", *field, unit);
  lv_obj_t * p = iconButton(r, &ic_plus_12, false, 28);
  lv_obj_add_event_cb(m, [](lv_event_t * e) { stepApply(s_steps[(int)(intptr_t)lv_event_get_user_data(e)], -1); },
                      LV_EVENT_CLICKED, (void *)(intptr_t)s_nSteps);
  lv_obj_add_event_cb(p, [](lv_event_t * e) { stepApply(s_steps[(int)(intptr_t)lv_event_get_user_data(e)], +1); },
                      LV_EVENT_CLICKED, (void *)(intptr_t)s_nSteps);
  s_nSteps++;
}

static void refresh() {
  char b[128], a[32], c2[32];
  net::Status ns = net::status();
  int64_t t = now();
  if (ns.wifi == net::W_CONNECTED) snprintf(b, sizeof(b), "Connect\xC3\xA9 \xC3\xA0 %s \xC2\xB7 %d dBm \xC2\xB7 %s", ns.ssid, ns.rssi, ns.ip);
  else if (ns.wifi == net::W_CONNECTING) snprintf(b, sizeof(b), "Connexion \xC3\xA0 %s\xE2\x80\xA6", ns.ssid[0] ? ns.ssid : g_set.ssid);
  else if (!g_set.ssid[0]) snprintf(b, sizeof(b), "Non configur\xC3\xA9 \xE2\x80\x94 touchez pour choisir un r\xC3\xA9seau");
  else snprintf(b, sizeof(b), "D\xC3\xA9" "connect\xC3\xA9 (%s)", g_set.ssid);
  lv_label_set_text(s_wifiSub, b);

  fmtAgo(a, sizeof(a), g_model.lastUpdate, t);
  fmtClock(c2, sizeof(c2), g_model.feedUpdated);
  snprintf(b, sizeof(b), "GBFS v3 \xC2\xB7 api.cyclocity.fr \xC2\xB7 flux de %s, re\xC3\xA7u %s", c2, a);
  lv_label_set_text(s_srcSub, b);
  bool live = g_model.lastUpdate && !vn::isStale(g_model.lastUpdate, t, vn::staleThresholdS(g_set));
  badgeSet(s_srcBadge, live ? "LIVE" : (g_model.fetchErrors ? "ERREUR" : "ATTENTE"), live ? T_GOOD : (g_model.fetchErrors ? T_BAD : T_WARN));
  snprintf(b, sizeof(b), "Flux station_status toutes les %d s \xC2\xB7 dernier cycle %u ms", g_set.refreshSec, (unsigned)g_model.fetchMs);
  lv_label_set_text(s_refSub, b);
  fmtDur(a, sizeof(a), vn::staleThresholdS(g_set) / 60);
  snprintf(b, sizeof(b), "Bandeau si station_status a plus de %s%s", a,
           g_set.staleMin ? "" : " (auto : 3 \xC3\x97 la p\xC3\xA9riode, 3 min au moins)");
  lv_label_set_text(s_freshSub, b);
  {
    int bad = 0, worst = -1;
    for (int f = 0; f < vn::FEED_COUNT; f++) if (ns.feed[f].fails) { bad++; if (worst < 0 || ns.feed[f].fails > ns.feed[worst].fails) worst = f; }
    if (!bad) snprintf(b, sizeof(b), "4 flux suivis \xC2\xB7 aucun \xC3\xA9" "chec en cours \xC2\xB7 NTP %s", ns.timeOk ? "synchronis\xC3\xA9" : "non synchronis\xC3\xA9");
    else snprintf(b, sizeof(b), "%s : %u \xC3\xA9" "chec%s cons\xC3\xA9" "cutif%s", vn::feedName(worst), (unsigned)ns.feed[worst].fails,
                  ns.feed[worst].fails > 1 ? "s" : "", ns.feed[worst].fails > 1 ? "s" : "");
    lv_label_set_text(s_ingSub, b);
    lv_obj_set_style_text_color(s_ingSub, C(bad ? HX_WARN : HX_TEXT2), 0);
  }

  int days = g_model.learnSince ? (int)((t - g_model.learnSince) / 86400) : 0;
  fmtInt(a, sizeof(a), (int)g_model.learnSamples);
  snprintf(b, sizeof(b), "Profil hebdomadaire couvert \xC3\xA0 %.0f %% \xC2\xB7 %d j \xC2\xB7 %s relev\xC3\xA9s", g_model.learnPct(), days, a);
  lv_label_set_text(s_learnSub, b);
  lv_bar_set_value(s_learnBar, (int32_t)g_model.learnPct(), LV_ANIM_OFF);
  snprintf(b, sizeof(b), "%d %% \xC3\xA0 +1 h \xC2\xB7 profil + tendance 30 min amortie", g_model.netConf);
  lv_label_set_text(s_confSub, b);

  snprintf(b, sizeof(b), "%s \xC2\xB7 %s", hal::touchName(),
           g_set.tcal.valid ? "\xC3\xA9talonnage enregistr\xC3\xA9" : "r\xC3\xA9glage d'usine");
  lv_label_set_text(s_touchSub, b);
  // Synchro en ligne
  if (vn::g_sync.url[0]) snprintf(b, sizeof(b), "%s%s", vn::g_sync.url, vn::g_sync.token[0] ? " \xC2\xB7 jeton enregistr\xC3\xA9" : "");
  else snprintf(b, sizeof(b), "Aucun \xE2\x80\x94 touchez pour saisir l'adresse (http://umbrel.local:8790)");
  lv_label_set_text(s_syncUrlSub, b);
  {
    char line[160];
    vn::syncStatusLine(ns.sync, t, line, sizeof(line));
    lv_label_set_text(s_syncSub, line);
    lv_obj_set_style_text_color(s_syncSub, C(ns.sync.state == vn::SS_FAIL ? HX_WARN : HX_TEXT2), 0);
  }
  {
    char line[200], d[240];
    if (vn::g_regCfg.role == vn::ROLE_REGUL) {
      vn::regStatusLine(vn::g_reg, vn::g_sync.url[0] != 0, t, line, sizeof(line));
      snprintf(d, sizeof(d), "%s \xC2\xB7 %s", net::deviceId()[0] ? net::deviceId() : "\xE2\x80\x94", line);
    } else snprintf(d, sizeof(d), "%s \xC2\xB7 nom sous lequel le serveur voit ce Sunton", net::deviceId()[0] ? net::deviceId() : "\xE2\x80\x94");
    lv_label_set_text(s_devSub, d);
  }

  plat::SysInfo si;
  plat::sysInfo(si);
  fmtInt(a, sizeof(a), (int)(si.heapFree / 1024));
  fmtFloat1(c2, sizeof(c2), si.psramFree / 1048576.f);
  snprintf(b, sizeof(b), "RAM %s Ko libres \xC2\xB7 PSRAM %s Mo libres \xC2\xB7 %s", a, c2, si.chip);
  lv_label_set_text(s_memSub, b);
  snprintf(b, sizeof(b), "LittleFS %u Ko / %u Ko \xC2\xB7 historique 24 h + profils", (unsigned)(si.fsUsed / 1024), (unsigned)(si.fsTotal / 1024));
  lv_label_set_text(s_fsSub, b);
  fmtDur(a, sizeof(a), (int)(si.uptimeS / 60));
  snprintf(b, sizeof(b), "Allum\xC3\xA9 depuis %s \xC2\xB7 heure %s \xC2\xB7 v" VN_VERSION, a, ns.timeOk ? "NTP synchronis\xC3\xA9" "e" : "non synchronis\xC3\xA9" "e");
  lv_label_set_text(s_upSub, b);
}

static void build(lv_obj_t * root) {
  lv_obj_t * sc = col(root, 4);
  lv_obj_set_size(sc, CONTENT_W, CONTENT_H);
  lv_obj_set_style_pad_all(sc, 8, 0);
  lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_OFF);

  // Réseau
  lv_obj_t * s1 = section(sc, "R\xC3\x89SEAU & DONN\xC3\x89" "ES");
  lv_obj_t * r = srow(s1, &ic_wifi_16, "Wi-Fi", "", &s_wifiSub);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  icon(r, &ic_chevron_right_16, HX_TEXT3);
  lv_obj_add_event_cb(r, [](lv_event_t *) { openWifi(); }, LV_EVENT_CLICKED, nullptr);
  r = srow(s1, &ic_globe_16, "Source", "", &s_srcSub);
  s_srcBadge = badge(r, "ATTENTE", T_WARN);
  r = srow(s1, &ic_refresh_16, "Rafra\xC3\xAE" "chissement", "", &s_refSub, 150);
  static const char * RL[4] = { "30 s", "1 min", "2 min", "5 min" };
  static const uint16_t RV[4] = { 30, 60, 120, 300 };
  int cur = 1;
  for (int k = 0; k < 4; k++) if (RV[k] == g_set.refreshSec) cur = k;
  lv_obj_t * seg = segmented(r, RL, 4, cur, [](int i) {
    static const uint16_t V[4] = { 30, 60, 120, 300 };
    g_set.refreshSec = V[i];
    vn::settingsSave();
    refresh();
  }, 30);
  lv_obj_set_width(seg, 176);
  segSet(seg, cur, false);
  // Seuil du bandeau « données vieilles » (0 = auto)
  r = srow(s1, &ic_clock_16, "Alerte de fra\xC3\xAE" "cheur", "", &s_freshSub, 150);
  static const char * FL[4] = { "Auto", "5 min", "10 min", "30 min" };
  static const uint8_t FV[4] = { 0, 5, 10, 30 };
  int fcur = 0;
  for (int k = 0; k < 4; k++) if (FV[k] == g_set.staleMin) fcur = k;
  seg = segmented(r, FL, 4, fcur, [](int i) {
    static const uint8_t V[4] = { 0, 5, 10, 30 };
    g_set.staleMin = V[i];
    vn::settingsSave();
    refresh();
  }, 30);
  lv_obj_set_width(seg, 176);
  segSet(seg, fcur, false);
  // Tableau d'ingestion (mesures de la tâche réseau)
  r = srow(s1, &ic_activity_16, "Ingestion des flux", "", &s_ingSub, 300);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  icon(r, &ic_chevron_right_16, HX_TEXT3);
  lv_obj_add_event_cb(r, [](lv_event_t *) { openIngest(); }, LV_EVENT_CLICKED, nullptr);

  // Synchro en ligne (core/sync.h, online/server.mjs)
  lv_obj_t * s6 = section(sc, "SYNCHRO EN LIGNE");
  r = srow(s6, &ic_globe_16, "Serveur", "", &s_syncUrlSub, 300);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  icon(r, &ic_chevron_right_16, HX_TEXT3);
  lv_obj_add_event_cb(r, [](lv_event_t *) { openSyncUrl(); }, LV_EVENT_CLICKED, nullptr);
  r = srow(s6, &ic_refresh_16, "Synchroniser", "", &s_syncSub, 210);
  lv_obj_t * sb = button(r, "Maintenant", &ic_refresh_16, B_SECONDARY, true);
  lv_obj_add_event_cb(sb, [](lv_event_t *) {
    if (!vn::g_sync.url[0]) { toast("Aucun serveur", "Saisir son adresse dans \xC2\xAB Serveur \xC2\xBB", T_WARN, &ic_globe_16); return; }
    net::requestSync();
    toast("Synchronisation", "La carte red\xC3\xA9marre si elle adopte un fichier", T_ACCENT, &ic_refresh_16);
  }, LV_EVENT_CLICKED, nullptr);
  r = srow(s6, &ic_power_16, "Au d\xC3\xA9marrage", "Reprendre l'\xC3\xA9tat du serveur avant de relire l'historique", nullptr, 230);
  toggle(r, vn::g_sync.atBoot, [](lv_event_t * e) {
    vn::g_sync.atBoot = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    vn::syncCfgSave();
  }, nullptr);
  r = srow(s6, &ic_layers_16, "Partager l'apprentissage", "Envoyer au serveur les profils et les jours mieux connus ici", nullptr, 230);
  toggle(r, vn::g_sync.push, [](lv_event_t * e) {
    vn::g_sync.push = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    vn::syncCfgSave();
  }, nullptr);

  // Rôle (core/regul.h) : poste de supervision, ou Sunton du camion (écran « Tournée »)
  lv_obj_t * s7 = section(sc, "R\xC3\x94LE DE CE SUNTON");
  r = srow(s7, &ic_truck_16, "R\xC3\xB4le", "Poste : supervision du parc \xC2\xB7 R\xC3\xA9gulateur : tourn\xC3\xA9" "e du camion en premier onglet (red\xC3\xA9marre)",
           nullptr, 190);
  static const char * ROLES[2] = { "Poste", "R\xC3\xA9gulateur" };
  seg = segmented(r, ROLES, 2, vn::g_regCfg.role == vn::ROLE_REGUL ? 1 : 0, [](int i) {
    uint8_t role = i == 1 ? vn::ROLE_REGUL : vn::ROLE_POSTE;
    if (role == vn::g_regCfg.role) return;
    vn::g_regCfg.role = role;
    if (!vn::regCfgSave()) { toast("R\xC3\xB4le non enregistr\xC3\xA9", "\xC3\x89" "criture en flash impossible", T_BAD, &ic_alert_16); return; }
    toast("Changement de r\xC3\xB4le", role == vn::ROLE_REGUL ? "Le Sunton red\xC3\xA9marre en r\xC3\xA9gulateur (onglet Tourn\xC3\xA9" "e)"
                                                              : "Le Sunton red\xC3\xA9marre en poste de supervision", T_ACCENT, &ic_truck_16);
    lv_timer_t * tm = lv_timer_create([](lv_timer_t *) {
      vn::Model::Blob blob;                                   // comme « Redémarrer » : état sauvegardé d'abord
      if (g_model.serialize(blob)) vn::Model::writeSaved(blob);
      plat::restart();
    }, 1500, nullptr);
    lv_timer_set_repeat_count(tm, 1);
  }, 30);
  lv_obj_set_width(seg, 170);
  segSet(seg, vn::g_regCfg.role == vn::ROLE_REGUL ? 1 : 0, false);
  srow(s7, &ic_key_16, "Appareil", "", &s_devSub, 300);

  // Seuils
  lv_obj_t * s2 = section(sc, "SEUILS D'ALERTE");
  r = srow(s2, &ic_bike_16, "V\xC3\xA9los faibles", "Station \xC2\xAB faible \xC2\xBB \xC3\xA0 \xE2\x89\xA4 n v\xC3\xA9los", nullptr, 180);
  stepper(r, &g_set.lowBikes, 0, 9, "");
  r = srow(s2, &ic_dock_16, "Bornes faibles", "Station \xC2\xAB charg\xC3\xA9" "e \xC2\xBB \xC3\xA0 \xE2\x89\xA4 n bornes libres", nullptr, 180);
  stepper(r, &g_set.lowDocks, 0, 9, "");
  r = srow(s2, &ic_wrench_16, "V\xC3\xA9los hors service", "Alerte maintenance \xC3\xA0 \xE2\x89\xA5 n v\xC3\xA9los HS", nullptr, 180);
  stepper(r, &g_set.maintBikes, 1, 20, "");
  r = srow(s2, &ic_wrench_16, "Bornes hors service", "Alerte maintenance \xC3\xA0 \xE2\x89\xA5 n bornes HS", nullptr, 180);
  stepper(r, &g_set.maintDocks, 1, 20, "");
  r = srow(s2, &ic_clock_16, "Inactivit\xC3\xA9", "Aucun mouvement pendant n h (7 h\xE2\x80\x93" "22 h)", nullptr, 180);
  stepper(r, &g_set.frozenHours, 1, 12, "h");
  r = srow(s2, &ic_bell_16, "Notifications de rupture", "Toast \xC3\xA0 chaque station qui se vide ou sature", nullptr, 230);
  toggle(r, g_set.alertToasts, [](lv_event_t * e) {
    g_set.alertToasts = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    vn::settingsSave();
  }, nullptr);

  // Affichage
  lv_obj_t * s3 = section(sc, "AFFICHAGE");
  r = srow(s3, &ic_sun_16, "Luminosit\xC3\xA9", "R\xC3\xA9tro\xC3\xA9" "clairage de la dalle", nullptr, 120);
  lv_obj_t * sl = lv_slider_create(r);
  lv_obj_set_size(sl, 150, 6);
  lv_slider_set_range(sl, 5, 100);
  lv_slider_set_value(sl, g_set.brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, 30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C(HX_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_color(sl, C(HX_ACCENT_HOT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_dir(sl, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C(HX_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 5, LV_PART_KNOB);
  lv_obj_set_ext_click_area(sl, 14);
  lv_obj_add_event_cb(sl, [](lv_event_t * e) {
    g_set.brightness = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    plat::setBacklight(vn::effectiveBrightness(now()));
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sl, [](lv_event_t *) { vn::settingsSave(); }, LV_EVENT_RELEASED, nullptr);
  r = srow(s3, &ic_moon_16, "Att\xC3\xA9nuation nocturne", "23 h \xE2\x80\x93 4 h : luminosit\xC3\xA9 r\xC3\xA9" "duite \xC3\xA0 20 %", nullptr, 230);
  toggle(r, g_set.nightDim, [](lv_event_t * e) {
    g_set.nightDim = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    vn::settingsSave();
    plat::setBacklight(vn::effectiveBrightness(now()));
  }, nullptr);

  // Prédiction
  lv_obj_t * s4 = section(sc, "PR\xC3\x89" "DICTION");
  r = srow(s4, &ic_history_16, "Apprentissage", "", &s_learnSub, 300);
  lv_obj_t * body = lv_obj_get_child(r, 1);
  s_learnBar = lv_bar_create(body);
  lv_obj_clear_flag(s_learnBar, LV_OBJ_FLAG_CLICKABLE);     // indicateur, pas une commande
  lv_obj_set_size(s_learnBar, 300, 4);
  lv_bar_set_range(s_learnBar, 0, 100);
  lv_obj_set_style_bg_color(s_learnBar, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_learnBar, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_learnBar, C(HX_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_color(s_learnBar, C(HX_ACCENT_HOT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_dir(s_learnBar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
  r = srow(s4, &ic_gauge_16, "Confiance du mod\xC3\xA8le", "", &s_confSub, 300);
  r = srow(s4, &ic_x_16, "R\xC3\xA9initialiser", "Efface l'historique 24 h et les profils appris", nullptr, 180);
  s_resetBtn = button(r, "Effacer", &ic_x_16, B_DANGER, true);
  lv_obj_add_event_cb(s_resetBtn, [](lv_event_t *) {
    if (s_resetArmed && lv_tick_elaps(s_resetArmed) < 4000) {
      g_model.resetHistory();
      g_model.recompute(now());
      s_resetArmed = 0;
      buttonText(s_resetBtn, "Effacer");
      toast("Historique effac\xC3\xA9", "L'apprentissage repart de z\xC3\xA9ro", T_WARN, &ic_history_16);
    } else {
      s_resetArmed = lv_tick_get();
      if (!s_resetArmed) s_resetArmed = 1;
      buttonText(s_resetBtn, "Confirmer ?");
    }
  }, LV_EVENT_CLICKED, nullptr);

  // Système
  lv_obj_t * s5 = section(sc, "SYST\xC3\x88ME");
  r = srow(s5, &ic_target_16, "Tactile", "", &s_touchSub, 220);
  lv_obj_t * tb = button(r, "Tester", &ic_target_16, B_SECONDARY, true);
  lv_obj_add_event_cb(tb, [](lv_event_t *) { openTouchTest(false); }, LV_EVENT_CLICKED, nullptr);
  srow(s5, &ic_cpu_16, "M\xC3\xA9moire", "", &s_memSub, 300);
  srow(s5, &ic_layers_16, "Stockage", "", &s_fsSub, 300);
  r = srow(s5, &ic_power_16, "Fonctionnement", "", &s_upSub, 220);
  lv_obj_t * rb = button(r, "Red\xC3\xA9marrer", nullptr, B_SECONDARY, true);
  lv_obj_add_event_cb(rb, [](lv_event_t *) {
    // L'UI détient déjà le verrou du modèle : sérialisation + écriture avant de redémarrer
    vn::Model::Blob blob;
    if (g_model.serialize(blob)) vn::Model::writeSaved(blob);
    plat::restart();
  }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t * about = label(sc, &vn_inter_11, HX_TEXT3,
    "Donn\xC3\xA9" "es Vel'OH! (JCDecaux) via le flux public GBFS v3 cyclocity \xC2\xB7 m\xC3\xA9t\xC3\xA9o Open-Meteo \xC2\xB7 "
    "trac\xC3\xA9 du tram T1 : ATP / data.public.lu (CC BY 4.0). Interface d\xC3\xA9riv\xC3\xA9" "e de VelohNav (Silexperience). "
    "Les pr\xC3\xA9visions sont statistiques : elles s'affinent avec les jours d'historique.");
  lv_obj_set_width(about, CONTENT_W - 20);
  lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_top(about, 6, 0);
}

const ScreenDef SCR_SETTINGS = { "R\xC3\xA9glages", "R\xC3\x89GLAGES", &ic_sliders_20, build, refresh, refresh };

} // namespace ui
