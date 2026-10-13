// ── Tâche réseau (cœur 0) : Wi-Fi, NTP, flux GBFS, météo, sauvegardes ─
#ifdef ARDUINO
#include "net.h"
#include "../config.h"
#include "../core/model.h"
#include "../core/settings.h"
#include "../core/gbfs.h"
#include "../core/weather.h"
#include "../platform/platform.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

using vn::g_model;
using vn::g_set;

namespace net {

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Status s_status;
static volatile bool s_refreshReq = false, s_scanReq = false, s_saveReq = false, s_connectReq = false;
static char s_newSsid[33], s_newPass[65];
static volatile int s_scanCount = -2;            // ‑2 : rien, ‑1 : échec, ≥ 0 : terminé
static ScanItem s_scan[16];

static char * s_buf = nullptr;                   // réponse HTTP (PSRAM)
static const size_t BUF_CAP = 160 * 1024;
static vn::InfoRec * s_info = nullptr;
static vn::StatusRec * s_stat = nullptr;

static void setStage(const char * st) { portENTER_CRITICAL(&s_mux); s_status.stage = st; portEXIT_CRITICAL(&s_mux); }
static void setFetching(bool f) { portENTER_CRITICAL(&s_mux); s_status.fetching = f; portEXIT_CRITICAL(&s_mux); }

Status status() {
  Status s;
  portENTER_CRITICAL(&s_mux);
  s = s_status;
  portEXIT_CRITICAL(&s_mux);
  return s;
}

void requestRefresh() { s_refreshReq = true; }
void requestScan() { s_scanCount = -2; s_scanReq = true; }
bool scanDone(int & count) { int c = s_scanCount; if (c == -2) return false; count = c; return true; }
int scanResults(ScanItem * out, int max) {
  int n = s_scanCount < 0 ? 0 : s_scanCount;
  if (n > max) n = max;
  memcpy(out, s_scan, sizeof(ScanItem) * n);
  return n;
}
void connect(const char * ssid, const char * pass) {
  strncpy(s_newSsid, ssid, sizeof(s_newSsid) - 1);
  s_newSsid[sizeof(s_newSsid) - 1] = 0;
  strncpy(s_newPass, pass ? pass : "", sizeof(s_newPass) - 1);
  s_newPass[sizeof(s_newPass) - 1] = 0;
  s_connectReq = true;
}
void requestSave() { s_saveReq = true; }

// ── HTTP GET → s_buf (HTTP/1.0 : pas d'encodage « chunked ») ──────────
static int httpGetOnce(const char * url, size_t & len) {
  len = 0;
  WiFiClientSecure cli;
  cli.setInsecure();           // données publiques en lecture seule (voir README)
  HTTPClient http;
  http.useHTTP10(true);
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setUserAgent("VelohNavOps/" VN_VERSION " (ESP32-S3)");
  if (!http.begin(cli, url)) return -1;
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return code; }
  int total = http.getSize();
  WiFiClient * st = http.getStreamPtr();
  uint32_t last = millis();
  while (len < BUF_CAP - 1) {
    size_t av = st->available();
    if (av) {
      size_t want = av < BUF_CAP - 1 - len ? av : BUF_CAP - 1 - len;
      len += st->readBytes(s_buf + len, want);
      last = millis();
      if (total > 0 && len >= (size_t)total) break;
    } else {
      if (!st->connected()) break;
      if (millis() - last > 10000) break;
      delay(4);
    }
  }
  s_buf[len] = 0;
  http.end();
  return (total > 0 && len < (size_t)total) ? -2 : 200;
}

// cyclocity peut couper la connexion TCP : 3 tentatives espacées
static bool httpGet(const char * url, size_t & len) {
  for (int a = 0; a < 3; a++) {
    int c = httpGetOnce(url, len);
    if (c == 200 && len > 0) return true;
    plat::logf("[net] %s → %d (tentative %d)", url, c, a + 1);
    delay(800 * (a + 1));
  }
  return false;
}

// ── Cycle de récupération ────────────────────────────────────────────
static int64_t s_infoAt = 0, s_typesAt = 0, s_wxAt = 0;
static vn::ElecTypes s_elec;

static void fetchCycle() {
  setFetching(true);
  uint32_t t0 = millis();
  size_t len;
  int64_t now = plat::now();
  char url[160];
  bool ok = true;

  if (!s_typesAt || now - s_typesAt > VN_TYPES_REFRESH_S) {
    snprintf(url, sizeof(url), "%s/vehicle_types.json", VN_GBFS_BASE);
    if (httpGet(url, len) && vn::gbfsParseTypes(s_buf, len, s_elec)) s_typesAt = now;
  }
  if (!s_infoAt || now - s_infoAt > VN_INFO_REFRESH_S) {
    setStage("Chargement des stations Vel'OH!\xE2\x80\xA6");
    snprintf(url, sizeof(url), "%s/station_information.json", VN_GBFS_BASE);
    int n = -1;
    if (httpGet(url, len)) n = vn::gbfsParseInfo(s_buf, len, s_info, VN_MAX_ST);
    if (n > 0) {
      plat::LockGuard g;
      g_model.applyInfo(s_info, n, now);
      s_infoAt = now;
    } else ok = false;
  }
  if (s_infoAt) {
    setStage("Disponibilit\xC3\xA9s en temps r\xC3\xA9" "el\xE2\x80\xA6");
    snprintf(url, sizeof(url), "%s/station_status.json", VN_GBFS_BASE);
    int64_t lastUpd = 0;
    int n = -1;
    if (httpGet(url, len)) n = vn::gbfsParseStatus(s_buf, len, s_stat, VN_MAX_ST, s_elec, &lastUpd);
    if (n > 0) {
      plat::LockGuard g;
      g_model.elec = s_elec;
      g_model.applyStatus(s_stat, n, plat::now());
      g_model.feedUpdated = lastUpd;
      g_model.fetchErrors = 0;
      g_model.fetchMs = millis() - t0;
    } else ok = false;
  }
  if (!ok) { plat::LockGuard g; g_model.fetchErrors++; g_model.version++; }
  setFetching(false);
}

static bool fetchWeather() {
  size_t len;
  if (!httpGet(VN_WX_URL, len)) return false;
  vn::Weather w;
  if (!vn::wxParse(s_buf, len, w)) return false;
  w.fetchedAt = plat::now();
  plat::LockGuard g;
  g_model.wx = w;
  g_model.recompute(plat::now());      // le modèle se recalcule aussi pour mettre l'UI à jour
  return true;
}

static void saveHistory() {
  vn::Model::Blob b;
  bool built;
  {
    plat::LockGuard g;
    built = g_model.serialize(b);
    if (built) g_model.dirty = false;
  }
  if (built) {
    uint32_t t0 = millis();
    bool ok = vn::Model::writeSaved(b);
    plat::logf("[fs] historique sauvegard\xC3\xA9 (%s, %u ms)", ok ? "ok" : "\xC3\xA9" "chec", (unsigned)(millis() - t0));
  }
}

static void doScan() {
  int n = WiFi.scanNetworks(false, false);
  if (n < 0) { s_scanCount = -1; return; }
  int k = 0;
  for (int i = 0; i < n && k < 16; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;
    for (int j = 0; j < k; j++) if (strcmp(s_scan[j].ssid, ss.c_str()) == 0) { dup = true; break; }
    if (dup) continue;
    strncpy(s_scan[k].ssid, ss.c_str(), sizeof(s_scan[k].ssid) - 1);
    s_scan[k].ssid[sizeof(s_scan[k].ssid) - 1] = 0;
    s_scan[k].rssi = WiFi.RSSI(i);
    s_scan[k].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    k++;
  }
  WiFi.scanDelete();
  // Tri par puissance décroissante
  for (int a = 1; a < k; a++) {
    ScanItem v = s_scan[a]; int b = a - 1;
    while (b >= 0 && s_scan[b].rssi < v.rssi) { s_scan[b + 1] = s_scan[b]; b--; }
    s_scan[b + 1] = v;
  }
  s_scanCount = k;
}

static void wifiStart(const char * ssid, const char * pass) {
  WiFi.disconnect(false, true);
  delay(100);
  if (ssid && *ssid) {
    WiFi.begin(ssid, pass);
    portENTER_CRITICAL(&s_mux);
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    s_status.wifi = W_CONNECTING;
    portEXIT_CRITICAL(&s_mux);
  }
}

static void task(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("velohnav-ops");
  configTzTime(VN_TZ, VN_NTP1, VN_NTP2);
  if (g_set.ssid[0]) { setStage("Connexion Wi-Fi\xE2\x80\xA6"); wifiStart(g_set.ssid, g_set.pass); }
  else setStage("Wi-Fi \xC3\xA0 configurer");

  bool loaded = false;
  uint32_t lastFetch = 0, lastSave = millis(), wifiSince = millis();
  uint8_t lastBl = 255;
  for (;;) {
    if (s_connectReq) {
      s_connectReq = false;
      setStage("Connexion Wi-Fi\xE2\x80\xA6");
      wifiStart(s_newSsid, s_newPass);
      wifiSince = millis();
    }
    if (s_scanReq) { s_scanReq = false; doScan(); }

    bool up = WiFi.status() == WL_CONNECTED;
    bool timeOk = plat::timeValid();
    // Les accesseurs Wi-Fi (SSID, IP, RSSI) descendent dans un ioctl qui prend
    // des sémaphores et alloue des String. Les appeler sous portENTER_CRITICAL
    // laissait ce cœur en attente, verrou pris et interruptions coupées, pendant
    // que le cœur 1 tournait en rond sur le même verrou : chien de garde
    // d'interruption, puis redémarrage en boucle (dès que le Wi-Fi se connecte).
    // On lit donc hors section critique ; le verrou ne couvre que la recopie.
    char ssid[33] = {0}, ip[16] = {0};
    int rssi = 0;
    if (up) {
      strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
      strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1);
      rssi = WiFi.RSSI();
    }
    portENTER_CRITICAL(&s_mux);
    if (up) {
      s_status.wifi = W_CONNECTED;
      strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
      s_status.ssid[sizeof(s_status.ssid) - 1] = 0;
      strncpy(s_status.ip, ip, sizeof(s_status.ip) - 1);
      s_status.ip[sizeof(s_status.ip) - 1] = 0;
      s_status.rssi = rssi;
    } else if (s_status.wifi == W_CONNECTING && millis() - wifiSince > 20000) {
      s_status.wifi = W_FAILED;
    } else if (s_status.wifi == W_CONNECTED) {
      s_status.wifi = W_CONNECTING;
      wifiSince = millis();
    }
    s_status.timeOk = timeOk;
    portEXIT_CRITICAL(&s_mux);

    // Historique / profils : chargés une fois l'heure connue
    if (timeOk && !loaded) {
      plat::LockGuard g;
      bool any = g_model.load(plat::now());
      loaded = true;
      plat::logf("[fs] historique %s", any ? "restaur\xC3\xA9" : "vierge");
    }

    if (up && !timeOk) setStage("Synchronisation de l'heure\xE2\x80\xA6");
    if (up && timeOk && loaded) {
      bool due = s_refreshReq || !lastFetch || millis() - lastFetch >= (uint32_t)g_set.refreshSec * 1000;
      if (due) {
        s_refreshReq = false;
        lastFetch = millis();
        fetchCycle();
      }
      if (!s_wxAt || plat::now() - s_wxAt >= VN_WX_REFRESH_S) {
        s_wxAt = plat::now();
        if (!fetchWeather()) s_wxAt -= VN_WX_REFRESH_S - 120;   // nouvel essai dans 2 min
      }
    }
    if (loaded && (s_saveReq || millis() - lastSave >= VN_SAVE_PERIOD_S * 1000UL)) {
      s_saveReq = false;
      lastSave = millis();
      if (g_model.dirty) saveHistory();
    }
    uint8_t bl = vn::effectiveBrightness(plat::now());
    if (bl != lastBl) { plat::setBacklight(bl); lastBl = bl; }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void begin() {
  memset(&s_status, 0, sizeof(s_status));
  s_status.stage = "D\xC3\xA9marrage\xE2\x80\xA6";
  s_buf = (char *)heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_info = (vn::InfoRec *)plat::psAlloc(sizeof(vn::InfoRec) * VN_MAX_ST);
  s_stat = (vn::StatusRec *)plat::psAlloc(sizeof(vn::StatusRec) * VN_MAX_ST);
  xTaskCreatePinnedToCore(task, "vn_net", 16384, nullptr, 3, nullptr, 0);
}

} // namespace net
#endif // ARDUINO
