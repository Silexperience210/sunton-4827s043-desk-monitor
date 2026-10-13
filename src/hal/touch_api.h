// ── Tactile : échantillon brut + étalonnage (partagé appareil / simulateur) ─
#pragma once
#include <stdint.h>
#include "../core/settings.h"

namespace hal {

struct TouchSample {
  int16_t  rx, ry;        // coordonnées brutes du contrôleur (avant étalonnage)
  int16_t  x, y;          // coordonnées écran (après étalonnage)
  bool     pressed;
  uint32_t seq;           // incrémenté à chaque lecture appuyée
};

// Fournis par le pilote (touch.cpp) ou le simulateur
TouchSample  touchLast();            // dernier échantillon lu par le pilote LVGL
const char * touchName();            // « GT911 @0x5D 480x272 », « XPT2046 »…
bool         touchIsResistive();
void         touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap);

// Communs (touch_cal.cpp)
void touchToScreen(int rx, int ry, int16_t & x, int16_t & y);   // brut → pixels (étalonnage courant)
// Résout l'étalonnage à partir de 4 cibles (haut-gauche, haut-droite, bas-droite, bas-gauche)
bool touchSolveCal(const int16_t rx[4], const int16_t ry[4], const int16_t tx[4], const int16_t ty[4],
                   vn::TouchCal & out);
// Erreur (px) d'un point brut converti avec un étalonnage donné
int  touchCalError(const vn::TouchCal & c, int rx, int ry, int tx, int ty);

} // namespace hal
