// ── Carte du réseau (dessinée, sans tuiles) ──────────────────────────
// Projection locale équirectangulaire, quadrillage métrique, tram T1,
// quartiers, stations colorées selon l'axe de remplissage — à l'instant
// présent ou à l'horizon de prévision choisi (+30 min, +1 h).
// Glisser = déplacer · double-tap = zoom · tap = fiche station.
#pragma once
#include "lvgl.h"
#include <stdint.h>

namespace ui {

enum MapFilter : uint8_t { MF_ALL, MF_EMPTY, MF_FULL, MF_TENSION };

typedef void (*MapTapCb)(int station);

lv_obj_t * mapCreate(lv_obj_t * parent, lv_coord_t w, lv_coord_t h, MapTapCb cb);
void       mapSetLayer(lv_obj_t * map, int fcIndex);      // 0 = maintenant, 6 = +30 min, 12 = +1 h
void       mapSetFilter(lv_obj_t * map, MapFilter f);
void       mapZoom(lv_obj_t * map, float factor);
void       mapFit(lv_obj_t * map, bool wholeNetwork);
void       mapSelect(lv_obj_t * map, int station);
void       mapCenter(lv_obj_t * map, float lat, float lon, float scale);
void       mapRefresh(lv_obj_t * map);

uint8_t    statusAt(int station, int fcIndex);            // statut courant ou prévu
bool       filterMatch(uint8_t status, MapFilter f);

} // namespace ui
