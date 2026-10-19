// ── Tâche réseau (cœur 0) : Wi-Fi, NTP, flux GBFS, météo, sauvegardes ─
#ifdef ARDUINO
#include "net.h"
#include "../config.h"
#include "../core/model.h"
#include "../core/settings.h"
#include "../core/gbfs.h"
#include "../core/weather.h"
#include "../core/wifiscan.h"
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
static volatile bool s_refreshReq = false, s_scanReq = false, s_connectReq = false;
static char s_newSsid[33], s_newPass[65];
static volatile int s_scanCount = -2;            // ‑2 : rien, ‑1 : échec, ≥ 0 : terminé
// Le réseau saisi à l'écran n'est enregistré qu'une fois l'association RÉUSSIE : l'écrire
// d'emblée faisait d'une faute de frappe le réseau du démarrage, et la carte bouclait en
// association sans pouvoir en découvrir un autre (voir connect()).
static volatile bool s_persistReq = false;
static vn::WifiList s_scan;                      // écrit par doScan() AVANT s_scanCount

static char * s_buf = nullptr;                   // réponse HTTP (PSRAM)
static const size_t BUF_CAP = 160 * 1024;
static vn::InfoRec * s_info = nullptr;
static vn::StatusRec * s_stat = nullptr;

static void setStage(const char * st) { portENTER_CRITICAL(&s_mux); s_status.stage = st; portEXIT_CRITICAL(&s_mux); }

// Compteurs d'ingestion : tenus ici (tâche réseau seule), publiés par simple recopie
// d'octets sous le verrou — aucun appel bloquant dans la section critique.
static vn::FeedStat s_feed[vn::FEED_COUNT];
static void feedDone(int f, bool ok, int code, size_t bytes, uint32_t t0) {
  uint32_t ms = millis() - t0;
  if (ok) vn::ingestOk(s_feed[f], plat::now(), (uint32_t)bytes, ms);
  else vn::ingestErr(s_feed[f], plat::now(), code, (uint32_t)bytes, ms);
  portENTER_CRITICAL(&s_mux);
  memcpy(&s_status.feed[f], &s_feed[f], sizeof(s_feed[f]));
  portEXIT_CRITICAL(&s_mux);
  if (!ok) plat::logf("[net] %s : \xC3\xA9" "chec %d (%s), %u octets, %u ms", vn::feedName(f), code,
                      vn::ingestErrText(code), (unsigned)bytes, (unsigned)ms);
}
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
void scanResults(vn::WifiList & out) {
  if (s_scanCount < 0) { out.n = out.hidden = out.dropped = 0; return; }
  out = s_scan;
}
void connect(const char * ssid, const char * pass) {
  strncpy(s_newSsid, ssid, sizeof(s_newSsid) - 1);
  s_newSsid[sizeof(s_newSsid) - 1] = 0;
  strncpy(s_newPass, pass ? pass : "", sizeof(s_newPass) - 1);
  s_newPass[sizeof(s_newPass) - 1] = 0;
  s_persistReq = true;            // enregistré seulement si l'association réussit
  s_connectReq = true;
}

// ── HTTP GET → s_buf (HTTP/1.0 : pas d'encodage « chunked ») ──────────
static int httpGetOnce(const char * url, size_t & len) {
  len = 0;
  WiFiClientSecure cli;
  cli.setInsecure();           // données publiques en lecture seule (voir README)
  // Négociation TLS : 120 s par défaut dans le framework, sans un mot au port série
  // (3 tentatives = 6 min de silence). 15 s suffisent largement à cyclocity.
  cli.setHandshakeTimeout(15);
  HTTPClient http;
  http.useHTTP10(true);
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setUserAgent("VelohNavOps/" VN_VERSION " (ESP32-S3)");
  if (!http.begin(cli, url)) return vn::ING_E_BEGIN;
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
  return (total > 0 && len < (size_t)total) ? vn::ING_E_TRUNC : 200;
}

static void serviceScan();

// cyclocity peut couper la connexion TCP : 3 tentatives espacées.
// code = statut de la dernière tentative (200, code HTTP, HTTPC_ERROR_* ou ING_E_*).
static bool httpGet(const char * url, size_t & len, int & code) {
  for (int a = 0; a < 3; a++) {
    serviceScan();             // feuille Wi-Fi ouverte : on n'attend pas la fin du cycle (voir serviceScan)
    code = httpGetOnce(url, len);
    if (code == 200 && len > 0) return true;
    if (code == 200) code = vn::ING_E_EMPTY;          // 200 sans corps
    plat::logf("[net] %s \xE2\x86\x92 %d (tentative %d)", url, code, a + 1);
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

  int code;
  if (!s_typesAt || now - s_typesAt > VN_TYPES_REFRESH_S) {
    uint32_t f0 = millis();
    snprintf(url, sizeof(url), "%s/vehicle_types.json", VN_GBFS_BASE);
    bool got = httpGet(url, len, code);
    bool parsed = got && vn::gbfsParseTypes(s_buf, len, s_elec);
    if (parsed) s_typesAt = now;
    feedDone(vn::FEED_TYPES, parsed, got ? vn::ING_E_PARSE : code, len, f0);
  }
  if (!s_infoAt || now - s_infoAt > VN_INFO_REFRESH_S) {
    setStage("Chargement des stations Vel'OH!\xE2\x80\xA6");
    snprintf(url, sizeof(url), "%s/station_information.json", VN_GBFS_BASE);
    int n = -1;
    uint32_t f0 = millis();
    bool got = httpGet(url, len, code);
    if (got) n = vn::gbfsParseInfo(s_buf, len, s_info, VN_MAX_ST);
    if (n > 0) {
      plat::LockGuard g;
      g_model.applyInfo(s_info, n, now);
      s_infoAt = now;
    } else ok = false;
    feedDone(vn::FEED_INFO, n > 0, !got ? code : n == 0 ? vn::ING_E_EMPTY : vn::ING_E_PARSE, len, f0);
  }
  if (s_infoAt) {
    setStage("Disponibilit\xC3\xA9s en temps r\xC3\xA9" "el\xE2\x80\xA6");
    snprintf(url, sizeof(url), "%s/station_status.json", VN_GBFS_BASE);
    int64_t lastUpd = 0;
    int n = -1, bad = 0;
    uint32_t f0 = millis();
    bool got = httpGet(url, len, code);
    if (got) n = vn::gbfsParseStatus(s_buf, len, s_stat, VN_MAX_ST, s_elec, &lastUpd, &bad);
    feedDone(vn::FEED_STATUS, n > 0, !got ? code : n == 0 ? vn::ING_E_EMPTY : vn::ING_E_PARSE, len, f0);
    if (n > 0) {
      int bikes;
      {
        plat::LockGuard g;
        g_model.elec = s_elec;
        g_model.applyStatus(s_stat, n, plat::now());
        g_model.feedUpdated = lastUpd;
        g_model.fetchErrors = 0;
        g_model.fetchMs = millis() - t0;
        bikes = g_model.tot.bikes;
      }
      // Un relevé réussi le dit aussi : un port série muet ne doit plus être ambigu
      plat::logf("[net] relev\xC3\xA9 ok : %d stations, %d v\xC3\xA9los dispo, %u octets, %u ms", n, bikes,
                 (unsigned)len, (unsigned)(millis() - f0));
      static int s_badSeen = -1;                 // la source en publie en permanence : une ligne par changement
      if (bad != s_badSeen) {
        s_badSeen = bad;
        if (bad) plat::logf("[net] station_status : %d horodatage(s) last_reported invalide(s), ignor\xC3\xA9(s)", bad);
      }
    } else ok = false;
  }
  if (!ok) { plat::LockGuard g; g_model.fetchErrors++; g_model.version++; }
  setFetching(false);
}

static bool fetchWeather() {
  size_t len;
  int code;
  uint32_t f0 = millis();
  if (!httpGet(VN_WX_URL, len, code)) { feedDone(vn::FEED_WEATHER, false, code, len, f0); return false; }
  vn::Weather w;
  bool parsed = vn::wxParse(s_buf, len, w);
  feedDone(vn::FEED_WEATHER, parsed, vn::ING_E_PARSE, len, f0);
  if (!parsed) return false;
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

// ── Recherche des réseaux ────────────────────────────────────────────
// Le pilote REFUSE tout balayage pendant qu'une association est en cours
// (ESP_ERR_WIFI_STATE, « wifi still connecting »), et l'auto-reconnexion du framework
// relance `disconnect + begin` à chaque raison reconnectable : tant que le réseau
// enregistré est injoignable (borne absente, mot de passe erroné), la station passe son
// temps à s'associer et la recherche échouait à chaque fois — la carte ne pouvait plus
// découvrir un autre réseau, donc plus en changer. On suspend donc l'association le temps
// du balayage (voir vn::scanPlan) : une déconnexion volontaire donne ASSOC_LEAVE, que le
// framework ne reconnecte pas, et la reprise se fait juste après.
// show_hidden = true : les bornes masquées remontent avec un SSID vide ; elles sont
// comptées, pas listées (voir core/wifiscan.h).
static void doScan() {
  uint32_t t0 = millis();
  vn::ScanPlan plan = vn::scanPlan(WiFi.status() == WL_CONNECTED, g_set.ssid[0] != 0);
  if (plan.suspend) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);        // interrompt la tentative, garde la configuration
    delay(VN_SCAN_SETTLE_MS);             // la radio doit rendre la main avant le balayage
  }
  int tries = 1;
  int n = WiFi.scanNetworks(false, true);
  // ‑1 = un balayage précédent n'a pas rendu la main (l'attente bloquante de 10 s du
  // framework a expiré, son drapeau « en cours » est resté levé) ; ‑2 = le pilote a
  // refusé. Les deux se réessaient : scanComplete() rend les résultats s'ils sont
  // arrivés, ou abaisse le drapeau périmé et rend ‑2, et on relance.
  for (int a = 0; vn::scanVerdict(n, a) == vn::SCAN_RETRY; a++) {
    delay(n == WIFI_SCAN_RUNNING ? VN_SCAN_RETRY_MS : 200);
    tries++;
    n = WiFi.scanComplete();
    if (n < 0) n = WiFi.scanNetworks(false, true);
  }
  if (plan.suspend) {
    WiFi.setAutoReconnect(true);          // rendue à l'auto-reconnexion dans tous les cas : à froid
                                          // (aucun réseau) il n'y a rien à relancer, mais la carte ne
                                          // doit pas rester sans reconnexion une fois un réseau choisi
    if (plan.resume) WiFi.begin();        // reprend l'association avec la configuration du pilote
  }
  if (n < 0) {
    plat::logf("[wifi] recherche : \xC3\xA9" "chec %d apr\xC3\xA8s %d tentative(s), %u ms%s", n, tries,
               (unsigned)(millis() - t0), plan.suspend ? ", association suspendue" : "");
    s_scanCount = -1;
    return;
  }
  if (n > 255) n = 255;                          // les accesseurs du framework prennent un index uint8_t
  vn::WifiRaw * raw = nullptr;
  if (n) {
    raw = (vn::WifiRaw *)heap_caps_malloc(sizeof(vn::WifiRaw) * n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) { WiFi.scanDelete(); s_scanCount = -1; return; }
  }
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    strncpy(raw[i].ssid, ss.c_str(), sizeof(raw[i].ssid) - 1);
    raw[i].ssid[sizeof(raw[i].ssid) - 1] = 0;
    raw[i].rssi = WiFi.RSSI(i);
    raw[i].channel = WiFi.channel(i);
    raw[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  vn::buildWifiList(raw, n, s_scan);
  if (raw) heap_caps_free(raw);
  // Chiffres à relever sur la carte : bornes brutes, réseaux, masqués, durée, tentatives
  plat::logf("[wifi] recherche : %d bornes, %d r\xC3\xA9seaux list\xC3\xA9s, %d masqu\xC3\xA9" "es, %d \xC3\xA9" "cart\xC3\xA9s, %u ms, %d tentative(s), %s%s",
             n, s_scan.n, s_scan.hidden, s_scan.dropped, (unsigned)(millis() - t0), tries,
             WiFi.status() == WL_CONNECTED ? "associ\xC3\xA9" : "non associ\xC3\xA9",
             plan.suspend ? (plan.resume ? ", association suspendue puis reprise" : ", association suspendue") : "");
  s_scanCount = s_scan.n;
}

// Appelée par la boucle de la tâche ET entre deux requêtes HTTP : un cycle GBFS +
// météo peut durer de longues secondes (3 tentatives par flux) ; sans ce point de
// passage, la feuille Wi-Fi restait sur « Recherche… » jusqu'à la fin du cycle.
// Jamais pendant une requête : la connexion HTTP en cours n'est pas interrompue.
static void serviceScan() {
  if (!s_scanReq) return;
  s_scanReq = false;
  doScan();
}

// ── Association : raison de la dernière déconnexion ──────────────────
// L'évènement arrive sur la tâche d'évènements Wi-Fi : on ne fait que noter le code,
// la tâche réseau le journalise (le framework, lui, le dit en « warning », filtré).
static volatile uint8_t s_discReason = 0;
static volatile uint32_t s_discCount = 0;
static void onDisconnect(WiFiEvent_t, WiFiEventInfo_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r == WIFI_REASON_ASSOC_LEAVE) return;      // notre propre WiFi.disconnect() (wifiStart)
  s_discReason = r;
  s_discCount++;
}

static void wifiStart(const char * ssid, const char * pass) {
  s_discReason = 0;
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
  WiFi.onEvent(onDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  configTzTime(VN_TZ, VN_NTP1, VN_NTP2);
  if (g_set.ssid[0]) {
    setStage("Connexion Wi-Fi\xE2\x80\xA6");
    plat::logf("[wifi] association \xC2\xAB %s \xC2\xBB\xE2\x80\xA6", g_set.ssid);
    wifiStart(g_set.ssid, g_set.pass);
  } else {
    setStage("Wi-Fi \xC3\xA0 configurer");
    plat::logf("[wifi] aucun r\xC3\xA9seau enregistr\xC3\xA9 : rien ne sera r\xC3\xA9" "cup\xC3\xA9r\xC3\xA9");
  }

  bool loaded = false, wasUp = false, timeLogged = false;
  uint32_t lastFetch = 0, lastSave = millis(), wifiSince = millis(), lastWait = 0, discSeen = 0, discLogAt = 0;
  uint8_t lastBl = 255;
  for (;;) {
    if (s_connectReq) {
      s_connectReq = false;
      setStage("Connexion Wi-Fi\xE2\x80\xA6");
      plat::logf("[wifi] association \xC2\xAB %s \xC2\xBB\xE2\x80\xA6 (demand\xC3\xA9" "e depuis l'\xC3\xA9" "cran)", s_newSsid);
      wifiStart(s_newSsid, s_newPass);
      wifiSince = millis();
    }
    serviceScan();

    bool up = WiFi.status() == WL_CONNECTED;
    bool timeOk = plat::timeValid();
    int64_t now = timeOk ? plat::now() : 0;      // lu hors section critique
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
    // Journal d'association : ok, refus (avec la raison), échec définitif, perte
    uint8_t reason = s_discReason;
    uint32_t disc = s_discCount;
    WifiState before = s_status.wifi;              // écrit par cette seule tâche : lecture sans verrou
    if (up && !wasUp) {
      plat::logf("[wifi] association ok : \xC2\xAB %s \xC2\xBB, IP %s, %d dBm, canal %d, en %u ms", ssid, ip, rssi,
                 (int)WiFi.channel(), (unsigned)(millis() - wifiSince));
      // Réseau choisi à l'écran : c'est seulement maintenant qu'il est enregistré.
      if (s_persistReq) {
        s_persistReq = false;
        snprintf(g_set.ssid, sizeof(g_set.ssid), "%s", s_newSsid);
        snprintf(g_set.pass, sizeof(g_set.pass), "%s", s_newPass);
        bool ok = vn::settingsSave();
        plat::logf("[wifi] r\xC3\xA9seau enregistr\xC3\xA9 : \xC2\xAB %s \xC2\xBB (%s)", g_set.ssid, ok ? "ok" : "\xC3\xA9" "chec");
      }
    }
    else if (!up && wasUp)
      plat::logf("[wifi] d\xC3\xA9" "connexion de \xC2\xAB %s \xC2\xBB : %s (code %d)", s_status.ssid,
                 vn::assocCauseText(vn::assocCause(reason)), reason);
    else if (!up && disc != discSeen && (!discLogAt || millis() - discLogAt >= 30000)) {
      // L'auto-reconnexion du framework réessaie sans fin : une ligne toutes les 30 s au plus
      plat::logf("[wifi] association \xC2\xAB %s \xC2\xBB refus\xC3\xA9" "e : %s (code %d, %u refus)", s_status.ssid,
                 vn::assocCauseText(vn::assocCause(reason)), reason, (unsigned)disc);
      discLogAt = millis();
    }
    discSeen = disc;
    wasUp = up;

    portENTER_CRITICAL(&s_mux);
    s_status.wifiReason = reason;
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
    if (timeOk && !s_status.timeSyncAt) s_status.timeSyncAt = now;
    WifiState after = s_status.wifi;
    portEXIT_CRITICAL(&s_mux);
    if (after == W_FAILED && before != W_FAILED) {
      plat::logf("[wifi] association \xC2\xAB %s \xC2\xBB \xC3\xA9" "chou\xC3\xA9" "e apr\xC3\xA8s 20 s : %s (code %d) \xE2\x80\x94 aucune r\xC3\xA9" "cup\xC3\xA9ration tant que le Wi-Fi manque",
                 s_status.ssid, vn::assocCauseText(vn::assocCause(reason)), reason);
      if (s_persistReq)
        plat::logf("[wifi] r\xC3\xA9seau \xC2\xAB %s \xC2\xBB NON enregistr\xC3\xA9 : l'ancien r\xC3\xA9seau reste celui du d\xC3\xA9marrage", s_newSsid);
    }
    if (timeOk && !timeLogged) {
      timeLogged = true;
      plat::logf("[net] heure NTP synchronis\xC3\xA9" "e");
    }

    // Historique / profils : chargés une fois l'heure connue
    if (timeOk && !loaded) {
      plat::LockGuard g;
      bool any = g_model.load(plat::now());
      loaded = true;
      plat::logf("[fs] historique %s", any ? "restaur\xC3\xA9" : "vierge");
    }

    if (up && !timeOk) setStage("Synchronisation de l'heure\xE2\x80\xA6");
    // Aucune récupération possible : on le dit toutes les 30 s (avant, le port série se taisait)
    if (!(up && timeOk && loaded) && (!lastWait || millis() - lastWait >= 30000)) {
      lastWait = millis();
      char why[112];
      vn::FeedStat st;
      portENTER_CRITICAL(&s_mux);
      WifiState w = s_status.wifi;
      st = s_status.feed[vn::FEED_STATUS];
      portEXIT_CRITICAL(&s_mux);
      vn::offlineCause(why, sizeof(why), w, s_status.ssid, reason, timeOk, st);
      plat::logf("[net] en attente, aucune r\xC3\xA9" "cup\xC3\xA9ration : %s", why);
    }
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
    if (loaded && millis() - lastSave >= VN_SAVE_PERIOD_S * 1000UL) {
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
