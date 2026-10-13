// ── Réseau : Wi-Fi, heure, récupération des flux (tâche dédiée) ──────
// Implémenté par net_esp32.cpp sur l'appareil, sim/net_sim.cpp au simulateur.
// L'UI ne fait que lire l'état et poser des demandes (aucun appel bloquant).
#pragma once
#include <stdint.h>

namespace net {

enum WifiState : uint8_t { W_NONE, W_CONNECTING, W_CONNECTED, W_FAILED };

struct Status {
  WifiState wifi;
  char      ssid[33];
  char      ip[16];
  int       rssi;
  bool      fetching;      // cycle de récupération en cours
  bool      timeOk;        // heure NTP synchronisée
  const char * stage;      // étape de démarrage (écran d'accueil)
};

struct ScanItem { char ssid[33]; int rssi; bool secure; };

void   begin();                                  // démarre la tâche réseau
Status status();
void   requestRefresh();                         // rafraîchissement immédiat
void   requestScan();
bool   scanDone(int & count);                    // count = ‑1 si échec
int    scanResults(ScanItem * out, int max);
void   connect(const char * ssid, const char * pass);
void   requestSave();                            // sauvegarde historique / profils

} // namespace net
