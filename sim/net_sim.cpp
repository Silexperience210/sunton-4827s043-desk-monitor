// ── Réseau « simulateur » : état Wi-Fi factice, scan fictif ──────────
#include "../src/net/net.h"
#include <string.h>
#include <stdio.h>

namespace sim {
extern uint32_t g_ms; bool g_fetching = false; bool g_wifiUp = true;
// scénario « démarrage » : état réseau imposé (‑1 = comportement par défaut)
int g_wifiState = -1; bool g_timeOk = true; const char * g_stage = nullptr;
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
  strcpy(s.ssid, "VELOH-OPS");
  strcpy(s.ip, "192.168.20.41");
  s.rssi = -58;
  s.fetching = sim::g_fetching;
  s.timeOk = sim::g_timeOk;
  s.timeSyncAt = sim::g_timeOk ? sim::g_timeSyncAt : 0;
  memcpy(s.feed, sim::g_feed, sizeof(s.feed));
  s.stage = sim::g_stage ? sim::g_stage : "Chargement des stations Vel'OH!\xE2\x80\xA6";
  return s;
}

void requestRefresh() {}
void requestScan() { s_scanReq = true; s_scanAt = sim::g_ms; }
bool scanDone(int & count) {
  if (!s_scanReq || sim::g_ms - s_scanAt < 400) return false;
  count = 5;
  return true;
}
int scanResults(ScanItem * out, int max) {
  static const ScanItem L[5] = {
    { "VELOH-OPS", -52, true }, { "JCDecaux-Depot", -61, true }, { "Ville-Lux-Guest", -70, false },
    { "Bureau-Regul-2", -74, true }, { "FreeWifi_Gare", -83, false } };
  int n = max < 5 ? max : 5;
  memcpy(out, L, sizeof(ScanItem) * n);
  return n;
}
void connect(const char * ssid, const char *) { printf("[sim] connect %s\n", ssid); }
void requestSave() {}

} // namespace net
