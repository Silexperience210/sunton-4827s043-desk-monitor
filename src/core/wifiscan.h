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
//  ‑1 (WIFI_SCAN_RUNNING) : un balayage précédent n'a pas fini ; ‑2 (WIFI_SCAN_FAILED) :
//        le pilote a refusé — typiquement parce que la station était en train de
//        s'associer (ESP_ERR_WIFI_STATE, « wifi still connecting »). Les deux se
//        réessaient tant que le budget de tentatives n'est pas épuisé, puis SCAN_FAIL.
// attempt : numéro de la tentative qui vient de rendre ce code (0 = la première).
enum ScanStep : uint8_t { SCAN_OK, SCAN_RETRY, SCAN_FAIL };
ScanStep scanVerdict(int code, int attempt);

// Faut-il suspendre l'association avant de balayer, et la relancer ensuite ?
// Le pilote REFUSE tout balayage pendant qu'une association est en cours, et le framework
// relance `disconnect + begin` à chaque raison reconnectable : quand le réseau enregistré
// est injoignable (ou dès qu'un mot de passe a été saisi de travers), la station passe son
// temps à s'associer et le balayage échoue à chaque fois — la carte ne peut alors plus
// découvrir un autre réseau, donc plus en changer. On met donc l'association en pause le
// temps du balayage ; une déconnexion volontaire donne ASSOC_LEAVE, que le framework ne
// reconnecte pas.
//   connected = la carte est associée (WL_CONNECTED) : le balayage d'arrière-plan est
//               accepté tel quel, rien à toucher ;
//   hasSsid   = un réseau est enregistré : il faut le relancer après le balayage.
struct ScanPlan {
  bool suspend;   // couper l'auto-reconnexion et déconnecter avant de balayer
  bool resume;    // relancer l'association après le balayage
};
ScanPlan scanPlan(bool connected, bool hasSsid);

} // namespace vn
