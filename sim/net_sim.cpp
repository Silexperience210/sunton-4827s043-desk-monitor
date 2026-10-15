// ── Réseau « simulateur » : état Wi-Fi factice, scan fictif ──────────
#include "../src/net/net.h"
#include "../src/core/settings.h"
#include <string.h>
#include <stdio.h>

namespace sim {
extern uint32_t g_ms; bool g_fetching = false; bool g_wifiUp = true;
// scénario « démarrage » : état réseau imposé (‑1 = comportement par défaut)
int g_wifiState = -1; bool g_timeOk = true; const char * g_stage = nullptr; int g_wifiReason = 0;
// compteurs d'ingestion : remplis par sim_main avec ce qu'il a réellement lu (taille
// des fichiers, durée d'analyse) — le simulateur ne fait aucune requête réseau
vn::FeedStat g_feed[vn::FEED_COUNT]; int64_t g_timeSyncAt = 0;
}

namespace net {

static bool s_scanReq = false;
static uint32_t s_scanAt = 0;

void begin() {}

Status status() {
  Status s{};
  s.wifi = sim::g_wifiState >= 0 ? (WifiState)sim::g_wifiState : (sim::g_wifiUp ? W_CONNECTED : W_FAILED);
  snprintf(s.ssid, sizeof(s.ssid), "%s", vn::g_set.ssid[0] ? vn::g_set.ssid : "VELOH-OPS");
  strcpy(s.ip, "192.168.20.41");
  s.rssi = -58;
  s.wifiReason = (uint8_t)sim::g_wifiReason;
  s.fetching = sim::g_fetching;
  s.timeOk = sim::g_timeOk;
  s.timeSyncAt = sim::g_timeOk ? sim::g_timeSyncAt : 0;
  memcpy(s.feed, sim::g_feed, sizeof(s.feed));
  s.stage = sim::g_stage ? sim::g_stage : "Chargement des stations Vel'OH!\xE2\x80\xA6";
  return s;
}

void requestRefresh() {}
void requestScan() { s_scanReq = true; s_scanAt = sim::g_ms; }
// Bornes brutes factices, passées par la même construction que sur l'appareil :
// VELOH-OPS vu par deux bornes (maillage), une borne masquée.
static const vn::WifiRaw RAW[7] = {
  { "Ville-Lux-Guest", -70, 6, false }, { "VELOH-OPS", -66, 1, true }, { "", -60, 11, true },
  { "JCDecaux-Depot", -61, 6, true }, { "VELOH-OPS", -52, 11, true }, { "Bureau-Regul-2", -74, 1, true },
  { "FreeWifi_Gare", -83, 13, false } };
bool scanDone(int & count) {
  if (!s_scanReq || sim::g_ms - s_scanAt < 400) return false;
  vn::WifiList l;
  vn::buildWifiList(RAW, 7, l);
  count = l.n;
  return true;
}
void scanResults(vn::WifiList & out) { vn::buildWifiList(RAW, 7, out); }
void connect(const char * ssid, const char *) { printf("[sim] connect %s\n", ssid); }
void requestSave() {}

} // namespace net
