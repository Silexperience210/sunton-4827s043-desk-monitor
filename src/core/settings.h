// ── Réglages persistants (fichier /settings.bin) ─────────────────────
#pragma once
#include <stdint.h>

namespace vn {

// Étalonnage tactile : valeurs brutes du contrôleur aux bords de l'écran (x = 0 / 479,
// y = 0 / 271). Un intervalle inversé (x0 > x1) traduit un axe en miroir.
struct TouchCal {
  uint8_t valid;          // 0 = valeurs par défaut du pilote
  uint8_t swap;           // axes X/Y échangés
  int16_t x0, x1, y0, y1;
};

struct Settings {
  uint32_t magic;
  uint16_t version;
  char     ssid[33];
  char     pass[65];
  uint16_t refreshSec;    // période de rafraîchissement station_status
  uint8_t  lowBikes;      // « faible » : vélos ≤ seuil
  uint8_t  lowDocks;      // « presque pleine » : bornes libres ≤ seuil
  uint8_t  maintBikes;    // alerte maintenance : vélos HS ≥ seuil
  uint8_t  maintDocks;    // alerte maintenance : bornes HS ≥ seuil
  uint8_t  frozenHours;   // alerte « aucun mouvement » (7 h–22 h)
  uint8_t  brightness;    // 5…100 %
  uint8_t  nightDim;      // atténuation 22 h–6 h
  uint8_t  alertToasts;   // notifications à l'écran des nouvelles ruptures
  TouchCal tcal;          // étalonnage tactile (Réglages → Tester le tactile)
  // Seuil du bandeau « données vieilles » en minutes ; 0 = automatique
  // (3 × refreshSec, au moins 3 min). Pris sur la réserve : taille et
  // décalages inchangés, un ancien fichier (réserve à zéro) donne « auto ».
  uint8_t  staleMin;
  uint8_t  reserved[24 - sizeof(TouchCal) - 1];
};
static_assert(sizeof(TouchCal) == 10, "TouchCal");
static_assert(sizeof(Settings) == 140, "Settings : la taille de /settings.bin ne doit pas changer");

extern Settings g_set;

void settingsDefaults(Settings & s);
bool settingsLoad();
bool settingsSave();
uint8_t effectiveBrightness(int64_t now);   // tient compte de l'atténuation 22 h – 6 h

} // namespace vn
