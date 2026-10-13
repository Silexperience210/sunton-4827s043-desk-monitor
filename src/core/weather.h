// ── Météo Open-Meteo + note « conditions vélo » ──────────────────────
// Portage de src/ui/weather.js (VelohNav) : mêmes pénalités nommées
// (pluie maintenant / prévue pondérée, vent > 20 km/h, froid, neige, orage).
// Pour l'exploitation, la note sert d'indicateur de demande attendue.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vn {

enum WxIcon : uint8_t { WX_SUN, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM };

struct WxHour {
  int8_t  h;          // +h heures
  float   temp, rain, wind;
  int16_t prob;       // % (‑1 si inconnu)
  int16_t code;
};

struct Weather {
  bool    valid = false;
  int64_t fetchedAt = 0;
  float   temp = 0, rain = 0, wind = 0;
  int     windDir = 0, code = 0;
  WxHour  fc[6];
  int     nfc = 0;
  float   score = 0;           // 0…10
  char    reason[56] = "";     // raison principale (la plus pénalisante)
};

bool        wxParse(const char * buf, size_t len, Weather & w);
const char* wxLabel(int code);          // libellé FR (groupes du dépôt)
WxIcon      wxIcon(int code);
void        wxScore(Weather & w);       // calcule score + reason

} // namespace vn
