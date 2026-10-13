// ── Coquille UI : partagé entre les écrans ───────────────────────────
#pragma once
#include "lvgl.h"
#include "theme.h"
#include "../core/model.h"
#include "../core/settings.h"

namespace ui {

// Gabarit 480×272 paysage : en-tête 36 px, rail de navigation 60 px à gauche
static const lv_coord_t HEADER_H = 36;
static const lv_coord_t RAIL_W = 60;
static const lv_coord_t CONTENT_W = 480 - RAIL_W;   // 420
static const lv_coord_t CONTENT_H = 272 - HEADER_H; // 236

struct ScreenDef {
  const char * label;            // libellé du rail
  const char * sub;              // sous-titre de l'en-tête
  const lv_img_dsc_t * icon;     // icône 20 px
  void (*build)(lv_obj_t * root);
  void (*refresh)();             // données changées (écran visible)
  void (*shown)();               // écran affiché
};

extern const ScreenDef SCR_DASH, SCR_MAP, SCR_LIST, SCR_ALERTS, SCR_SETTINGS;

enum Tab { TAB_DASH, TAB_MAP, TAB_LIST, TAB_ALERTS, TAB_SETTINGS, TAB_COUNT };

void showTab(int tab);
void openStation(int st);                      // fiche station (bottom sheet)
void openTransfers();                          // plan de régulation
void openWifi();                               // configuration Wi-Fi
void openStationsFiltered(int sortMode);       // onglet Stations avec tri
void mapFocus(int st);                         // onglet Carte centré sur une station
void toast(const char * title, const char * msg, Tone tone, const lv_img_dsc_t * ic);
void openTouchTest(bool calibrate);            // test / étalonnage du tactile (plein écran)
bool touchTestOpen();

// Bottom sheet générique : renvoie le corps (défilant) ; un seul à la fois
lv_obj_t * sheetOpen(const char * title);
void       sheetClose();
bool       sheetIsOpen();
int        sheetToken();                     // identifiant du sheet ouvert (‑1 si fermé)

// Utilitaires de données
int64_t now();                                 // heure courante (epoch)
const lv_img_dsc_t * wxIcon16(int code);
const lv_img_dsc_t * wxIcon12(int code);

// Rafraîchissement de la fiche station ouverte (appelé par la coquille)
void stationSheetRefresh();

} // namespace ui
