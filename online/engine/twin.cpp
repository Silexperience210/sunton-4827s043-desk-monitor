// ── Jumeau d'écran : l'UI LVGL du firmware, rendue dans le navigateur ─
// Même code que la carte (src/ui) : l'écran 480×272 est rendu dans une image RGB565 que la page
// recopie dans un <canvas> ; le doigt est un pointeur. La tâche réseau est remplacée par l'hôte :
// le serveur en ligne relève les flux, le jumeau rejoue les mêmes appels (engine.cpp).
#include "lvgl.h"
#include "../../src/ui/ui.h"
#include "../../src/ui/ui_internal.h"
#include "../../src/net/net.h"
#include "../../src/hal/touch_api.h"
#include "../../src/core/model.h"
#include "../../src/core/settings.h"
#include "../../src/core/sync.h"
#include "../../src/platform/platform.h"
#include "vn_lv_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT(name) extern "C" __attribute__((export_name(#name), used))

namespace eng {
extern double g_ms;
extern vn::FeedStat g_feed[vn::FEED_COUNT];
extern bool g_fetching;
}

// ── Crochets LVGL (mémoire, horloge, assertion) ──────────────────────
extern "C" {
void * vn_lv_malloc(size_t n) { return malloc(n); }
void   vn_lv_free(void * p) { free(p); }
void * vn_lv_realloc(void * p, size_t n) { return realloc(p, n); }
uint32_t vn_lv_tick(void) { return (uint32_t)(uint64_t)eng::g_ms; }
void   vn_lv_assert_fail(const char * file, int line) { plat::logf("[lvgl] assertion %s:%d", file, line); abort(); }
}

// ── Affichage ────────────────────────────────────────────────────────
static uint16_t s_fb[480 * 272];
static lv_color_t s_buf[480 * 40];                 // comme sur la carte : 40 lignes
static int s_dx0 = 480, s_dy0 = 272, s_dx1 = -1, s_dy1 = -1;   // zone modifiée depuis la dernière lecture

static void flush(lv_disp_drv_t * d, const lv_area_t * a, lv_color_t * px) {
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) s_fb[y * 480 + x] = (px++)->full;
  if (a->x1 < s_dx0) s_dx0 = a->x1;
  if (a->y1 < s_dy0) s_dy0 = a->y1;
  if (a->x2 > s_dx1) s_dx1 = a->x2;
  if (a->y2 > s_dy1) s_dy1 = a->y2;
  lv_disp_flush_ready(d);
}

// ── Tactile : un pointeur, coordonnées écran = brutes (comme le GT911) ─
static hal::TouchSample s_ts = { 0, 0, 0, 0, false, 0 };
static bool s_pressed = false;
static int s_px = 0, s_py = 0;
namespace hal {
TouchSample touchLast() { return s_ts; }
const char * touchName() { return "Pointeur (jumeau en ligne)"; }
bool touchIsResistive() { return false; }
void touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap) {
  x0 = 0; x1 = 479; y0 = 0; y1 = 271; swap = false;
}
}
static void readTouch(lv_indev_drv_t *, lv_indev_data_t * d) {
  if (s_pressed) {
    s_ts.rx = (int16_t)s_px; s_ts.ry = (int16_t)s_py;
    hal::touchToScreen(s_px, s_py, s_ts.x, s_ts.y);
    s_ts.seq++;
  }
  s_ts.pressed = s_pressed;
  d->point.x = s_ts.x; d->point.y = s_ts.y;
  d->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ── Tâche réseau : l'état vient de l'hôte ────────────────────────────
static net::WifiState s_wifi = net::W_CONNECTING;
static char s_ssid[33] = "serveur en ligne", s_stage[96] = "Connexion au serveur\xE2\x80\xA6";
static int s_rssi = -50;
static int64_t s_timeSyncAt = 0;
static bool s_refreshReq = false, s_syncReq = false;
static vn::SyncStatus s_syncStat = {};
static bool s_scanReq = false;
static double s_scanAt = 0;

namespace net {
void begin() {}
Status status() {
  Status s;
  memset(&s, 0, sizeof(s));
  s.wifi = s_wifi;
  snprintf(s.ssid, sizeof(s.ssid), "%s", s_ssid);
  snprintf(s.ip, sizeof(s.ip), "%s", "en ligne");
  s.rssi = s_rssi;
  s.fetching = eng::g_fetching;
  s.timeOk = plat::timeValid();
  s.timeSyncAt = s_timeSyncAt;
  s.stage = s_stage;
  memcpy(s.feed, eng::g_feed, sizeof(s.feed));
  s.sync = s_syncStat;
  return s;
}
void requestRefresh() { s_refreshReq = true; }
void requestSync() { s_syncReq = true; }
void requestScan() { s_scanReq = true; s_scanAt = eng::g_ms; }
static const vn::WifiRaw RAW[1] = { { "VelohNav Ops en ligne", -48, 1, true } };
bool scanDone(int & count) {
  if (!s_scanReq || eng::g_ms - s_scanAt < 400) return false;
  vn::WifiList l;
  vn::buildWifiList(RAW, 1, l);
  count = l.n;
  return true;
}
void scanResults(vn::WifiList & out) { vn::buildWifiList(RAW, 1, out); }
void connect(const char *, const char *) {}
} // namespace net

// ── API du jumeau ────────────────────────────────────────────────────
EXPORT(vn_ui_init) int vn_ui_init() {
  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 40);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);
  ui::init(disp);
  return 1;
}
EXPORT(vn_ui_tick) uint32_t vn_ui_tick() { return lv_timer_handler(); }
EXPORT(vn_ui_touch) void vn_ui_touch(int x, int y, int pressed) {
  s_px = x < 0 ? 0 : x > 479 ? 479 : x;
  s_py = y < 0 ? 0 : y > 271 ? 271 : y;
  s_pressed = pressed != 0;
}
EXPORT(vn_ui_fb) const uint16_t * vn_ui_fb() { return s_fb; }
// Zone modifiée depuis le dernier appel : x0 | y0 << 16 et x1 | y1 << 16 (‑1 = rien), remise à zéro
static int32_t s_dirty[4];
EXPORT(vn_ui_dirty) const int32_t * vn_ui_dirty() {
  s_dirty[0] = s_dx0; s_dirty[1] = s_dy0; s_dirty[2] = s_dx1; s_dirty[3] = s_dy1;
  s_dx0 = 480; s_dy0 = 272; s_dx1 = -1; s_dy1 = -1;
  return s_dirty;
}
// Réseau vu par le jumeau : 0 aucun, 1 connexion, 2 associé (flux du serveur reçu), 3 échec
EXPORT(vn_ui_net) void vn_ui_net(int wifi, int rssi, const char * name, const char * stage) {
  s_wifi = (net::WifiState)wifi;
  s_rssi = rssi;
  if (name) snprintf(s_ssid, sizeof(s_ssid), "%s", name);
  if (stage) snprintf(s_stage, sizeof(s_stage), "%s", stage);
  if (wifi == net::W_CONNECTED && !s_timeSyncAt) s_timeSyncAt = plat::now();
}
// Demandes de l'écran à l'hôte : bit 0 = actualiser (pastille LIVE touchée), bit 1 = synchroniser
// (Réglages → Synchro en ligne : le jumeau reprend l'état du serveur)
EXPORT(vn_ui_requests) int vn_ui_requests() {
  int r = (s_refreshReq ? 1 : 0) | (s_syncReq ? 2 : 0);
  s_refreshReq = s_syncReq = false;
  return r;
}
// Adresse du serveur et dernière reprise d'état, affichées par Réglages → Synchro en ligne
EXPORT(vn_ui_sync) void vn_ui_sync(const char * url, int state, double at, const char * text) {
  if (url) snprintf(vn::g_sync.url, sizeof(vn::g_sync.url), "%s", url);
  vn::g_sync.atBoot = true;
  vn::g_sync.push = false;
  s_syncStat.state = (int8_t)state;
  s_syncStat.at = (int64_t)at;
  if (text) snprintf(s_syncStat.text, sizeof(s_syncStat.text), "%s", text);
}
EXPORT(vn_ui_settings_save) int vn_ui_settings_save() { return vn::settingsSave(); }
