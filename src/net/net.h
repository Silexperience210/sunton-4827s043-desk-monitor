// ── Réseau : Wi-Fi, heure, récupération des flux (tâche dédiée) ──────
// Implémenté par net_esp32.cpp sur l'appareil, sim/net_sim.cpp au simulateur.
// L'UI ne fait que lire l'état et poser des demandes (aucun appel bloquant).
#pragma once
#include <stdint.h>
#include "../core/ops.h"
#include "../core/wifiscan.h"
#include "../core/sync.h"

namespace net {

enum WifiState : uint8_t { W_NONE, W_CONNECTING, W_CONNECTED, W_FAILED };   // valeurs reprises par vn::offlineCause

struct Status {
  WifiState wifi;
  char      ssid[33];
  char      ip[16];
  int       rssi;
  uint8_t   wifiReason;    // dernière raison de déconnexion (wifi_err_reason_t), 0 = aucune
  bool      fetching;      // cycle de récupération en cours
  bool      timeOk;        // heure NTP synchronisée
  int64_t   timeSyncAt;    // première synchronisation NTP constatée (epoch), 0 = jamais
  const char * stage;      // étape de démarrage (écran d'accueil)
  vn::FeedStat feed[vn::FEED_COUNT];   // compteurs d'ingestion, tenus par la tâche réseau
  vn::SyncStatus sync;     // dernière synchronisation avec le serveur en ligne (core/sync.h)
};

void   begin();                                  // démarre la tâche réseau
Status status();
void   requestRefresh();                         // rafraîchissement immédiat
void   requestScan();
bool   scanDone(int & count);                    // count = ‑1 si échec, sinon lignes à afficher
void   scanResults(vn::WifiList & out);          // copie de la dernière liste (vide si échec)
void   connect(const char * ssid, const char * pass);
// Synchronisation avec le serveur en ligne (g_sync.url) : la tâche réseau sauvegarde l'état, compare,
// adopte ce qui est meilleur et, si un fichier a été adopté, redémarre pour le relire.
void   requestSync();
// Identifiant sous lequel le serveur voit cette carte (« sunton-a1b2c3 », d'après l'adresse MAC)
const char * deviceId();
// Rôle « régulateur » (core/regul.h) : échange immédiat avec le serveur — un geste vient d'être rangé dans la
// file, ou l'écran veut le plan à jour. Sinon la tâche réseau échange toutes les 30 s (10 s si la file attend).
void   requestReg();

} // namespace net
