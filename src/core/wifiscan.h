// ── Recherche Wi-Fi : des résultats bruts de la radio à la liste affichée ─
// La radio rend une ligne par BORNE (BSSID), dans un ordre que la documentation
// d'ESP-IDF ne garantit pas. L'écran veut une ligne par RÉSEAU (SSID) :
//  1. les réseaux masqués (SSID vide) sont comptés à part — jamais une ligne vide,
//     et on ne peut pas s'y connecter en touchant une ligne sans connaître le nom ;
//  2. un SSID vu par plusieurs bornes (maillage) ne garde que la plus forte ;
//  3. le tri par puissance décroissante précède la troncature : si la liste
//     déborde, ce sont les plus faibles qui partent, et on dit combien.
// Portable (aucun Arduino) : testé tel quel par sim/test_core.cpp.
#pragma once
#include <stdint.h>
#include "../config.h"

namespace vn {

// Une borne telle que la radio la rapporte.
struct WifiRaw {
  char    ssid[33];        // "" = réseau masqué
  int16_t rssi;            // dBm
  uint8_t channel;
  bool    secure;          // false = réseau ouvert
};

// Un réseau tel que l'écran l'affiche.
struct WifiNet {
  char    ssid[33];        // jamais vide
  int16_t rssi;            // borne la plus forte de ce SSID
  uint8_t channel;         // canal de cette borne
  bool    secure;
  uint8_t aps;             // nombre de bornes vues sous ce nom (plafonné à 255)
};

struct WifiList {
  WifiNet net[VN_SCAN_MAX];
  int     n;               // lignes à afficher, triées par RSSI décroissant
  int     hidden;          // bornes à SSID masqué (non listées)
  int     dropped;         // réseaux nommés écartés faute de place (les plus faibles)
};

// raw : résultats bruts (nRaw bornes, dans n'importe quel ordre). Ne lit pas au-delà
// de nRaw ; nRaw ≤ 0 donne une liste vide.
void buildWifiList(const WifiRaw * raw, int nRaw, WifiList & out);

// Que faire du code rendu par WiFi.scanNetworks() / scanComplete() ?
//  ≥ 0 : nombre de bornes → SCAN_OK ;
//  ‑1 (WIFI_SCAN_RUNNING) : un balayage précédent n'a pas fini → SCAN_RETRY tant
//        que attempt + 1 < VN_SCAN_TRIES, puis SCAN_FAIL ;
//  ‑2 (WIFI_SCAN_FAILED) ou autre négatif : la radio a refusé → SCAN_FAIL tout de suite.
// attempt : numéro de la tentative qui vient de rendre ce code (0 = la première).
enum ScanStep : uint8_t { SCAN_OK, SCAN_RETRY, SCAN_FAIL };
ScanStep scanVerdict(int code, int attempt);

} // namespace vn
