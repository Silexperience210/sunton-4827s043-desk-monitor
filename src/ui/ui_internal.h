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

extern const ScreenDef SCR_DASH, SCR_MAP, SCR_LIST, SCR_ALERTS, SCR_SERVICE, SCR_SETTINGS;

enum Tab { TAB_DASH, TAB_MAP, TAB_LIST, TAB_ALERTS, TAB_SERVICE, TAB_SETTINGS, TAB_COUNT };

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

// Bandeau d'avertissement (style « hors ligne ») : icône, titre, message (*msg)
lv_obj_t * warnStrip(lv_obj_t * parent, const lv_img_dsc_t * ic, const char * title, lv_obj_t ** titleOut, lv_obj_t ** msgOut);
// Fraîcheur des données : true si station_status ou la météo dépasse son seuil
// (title / msg remplis pour le bandeau ; minutes d'âge dans *key pour éviter les redessins)
bool freshness(int64_t now, char * title, size_t tn, char * msg, size_t mn, int * key);
void dashFreshTick();                          // bandeau du tableau de bord (appelé par la coquille)

// Feuilles d'exploitation (sheet_ops.cpp)
enum ProvGroup { PG_BIKES, PG_INUSE, PG_DOCKS, PG_DISABLED, PG_WEATHER, PG_COUNT };
void openProvenance(int group);                // d'où vient ce chiffre (tuiles du tableau de bord)
void openIngest();                             // tableau d'ingestion (Réglages)
void openForecast24(int st);                   // prévision 24 h d'une station (fiche station)
void opsSheetRefresh();                        // mise à jour de la feuille d'exploitation ouverte

} // namespace ui
