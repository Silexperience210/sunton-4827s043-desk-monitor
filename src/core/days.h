// ── Résumés par jour : ce que la vue « Service » lit sur des semaines ──
// La carte ne garde que 24 h d'historique au pas de 5 min ; pour parler de semaines, chaque tranche
// de 5 min relevée est aussi versée dans un résumé du jour (VN_DAYS jours, anneau en PSRAM, sauvegardé
// dans /vn_days.bin). Ce qui est stocké, exactement (docs/vue-service.md) :
//
//  • Heures de service évaluées : [7 h, 22 h) heure locale, découpées en 15 créneaux d'une heure
//    (les mêmes heures que l'alerte « inhabituel » et « aucun mouvement »). Hors de ces heures,
//    rien n'est compté (sauf le statut de la tranche précédente, pour les entrées en rupture).
//  • Une tranche = un relevé station_status retenu par l'historique 5 min (Model::applyStatus).
//    Une station y est « observée » si elle est publiée (station_information) et a déjà un relevé.
//  • État d'une station observée dans la tranche — le statut de l'axe de remplissage (classify) :
//      hors service : non installée, ou ni location ni retour (ST_CLOSED) ;
//      vide         : ouverte, aucun vélo louable (ST_EMPTY ; location suspendue = vide) ;
//      pleine       : ouverte, aucune borne libre (ST_FULL ; retours suspendus = pleine) ;
//      disponible   : tout le reste — ouverte, ≥ 1 vélo louable ET ≥ 1 borne libre.
//    Un écart au profil (« inhabituel ») = l'état AN_HIGH / AN_LOW calculé pour ce relevé.
//  • Réseau, par jour et par créneau : tranches relevées, station-tranches observées, hors service,
//    vides, pleines, et entrées en rupture / saturation : passage à vide (à plein) entre deux
//    tranches CONSÉCUTIVES relevées (5 min d'écart) — une station déjà vide au premier relevé après
//    un trou n'est pas comptée comme une entrée (début inconnu).
//  • Station, par jour : tranches observées / hors service / vides / pleines sur les heures de service,
//    et quatre masques de 15 bits (un bit par créneau) : créneau relevé (≥ DAY_SEEN_SLOTS tranches
//    observées dans l'heure), créneau touché par le vide, par le plein, par un écart au profil
//    (≥ DAY_HIT_SLOTS tranches dans cet état). C'est sur ces masques que se lit le récurrent.
//  • Un jour férié légal (core/calendar) est marqué ; la vue Service le range avec le week-end.
// Portable (aucun Arduino) : testé tel quel par sim/test_core.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../config.h"

namespace vn {

struct Model;

static const int DAY_H0 = 7, DAY_H1 = 22;          // heures de service [7 h, 22 h)
static const int DAY_NH = DAY_H1 - DAY_H0;         // 15 créneaux d'une heure
static const int DAY_SLOTS_H = 3600 / VN_RING_STEP_S;   // 12 tranches de 5 min par heure
static const int DAY_SEEN_SLOTS = 6;               // créneau « relevé » : ≥ 30 min observées dans l'heure
static const int DAY_HIT_SLOTS = 3;                // créneau « touché » : ≥ 15 min dans l'état (vide, plein, écart)

enum : uint8_t { DF_HOLIDAY = 1 };

// Réseau, un jour (204 octets)
struct DayNet {
  int32_t  day;                  // jour local (jours depuis le 01/01/1970), 0 = case vide
  uint8_t  dow;                  // 0 = dimanche … 6 = samedi
  uint8_t  flags;                // DF_HOLIDAY
  uint8_t  slots[DAY_NH];        // tranches relevées dans le créneau (≤ 12)
  uint8_t  pad;
  uint16_t obs[DAY_NH];          // station-tranches observées
  uint16_t closed[DAY_NH];       //   dont hors service
  uint16_t empty[DAY_NH];        //   dont vides
  uint16_t full[DAY_NH];         //   dont pleines
  uint16_t evEmpty[DAY_NH];      // entrées en rupture (passage à vide) constatées
  uint16_t evFull[DAY_NH];       // entrées en saturation (passage à plein) constatées
  uint16_t rsv;                  // réservé (0) : taille multiple de 4, explicite
};

// Une station, un jour (12 octets)
struct DaySt {
  uint8_t  obs, closed, empty, full;   // tranches de 5 min sur les heures de service (≤ 180)
  uint16_t seen;                       // bit h : créneau h relevé
  uint16_t hitE, hitF, hitA;           // bit h : créneau h touché par le vide / le plein / un écart au profil
};

// Anneau des résumés + l'état de la tranche en cours
struct DayBook {
  DayNet * net = nullptr;        // [VN_DAYS], net[head] = jour le plus récent (en cours)
  DaySt *  st = nullptr;         // [VN_DAYS][VN_MAX_ST]
  int      head = -1, count = 0;
  uint8_t * hc = nullptr;        // [VN_MAX_ST][4] compteurs du créneau en cours : observée, vide, pleine, écart
  int32_t  hcKey = 0;            // jour × 100 + heure de ces compteurs (0 = aucun)
  uint8_t * prev = nullptr;      // [VN_MAX_ST] statut à la tranche précédente (255 = inconnu)
  int64_t  prevT = 0;            // tranche précédente versée (0 = aucune)
  uint32_t version = 0;          // incrémenté à chaque tranche versée

  bool init();
  void clear();
  // Une tranche de 5 min (début slotT) : états des stations du modèle À CET INSTANT
  // (appelé par Model::applyStatus après recompute, une fois par tranche réellement relevée)
  void addSlot(const Model & m, int64_t slotT);
  int  index(int back) const;                  // index du jour « back » jours stockés en arrière (‑1)
  const DayNet * dayAt(int back) const;        // nullptr si absent
  const DaySt *  stAt(int back) const;         // ligne de stations du même jour
  int  find(int32_t day) const;                // back du jour `day`, ‑1 s'il n'est pas stocké

  // Sauvegarde /vn_days.bin (sid des stations du modèle pour les réassocier au chargement)
  bool   serialize(const Model & m, uint8_t *& out, size_t & len) const;
  // Relit un blob ; false (et rien de modifié) s'il est illisible. Les stations sont
  // réassociées par sid via `map` (index de station du fichier → index du modèle, ‑1 = ignorée).
  bool   load(const uint8_t * b, size_t len, const int16_t * map, int nStFile, int64_t now);
};

// ── Dates locales ────────────────────────────────────────────────────
int32_t localDay(int64_t t);                       // jour local de l'instant t
int     localHour(int64_t t);
void    dayCivil(int32_t day, int & y, int & m, int & d);
int32_t civilDay(int y, int m, int d);
inline int dayDow(int32_t day) { return (int)(((day % 7) + 11) % 7); }   // 01/01/1970 = jeudi
bool    dayHoliday(int32_t day);

// En-tête de /vn_days.bin (exposé pour les tests de migration)
struct DaysFileHdr {
  uint32_t magic;                // "VND1"
  uint16_t ver;                  // version majeure (1)
  uint16_t nSt;                  // stations du fichier (sid à la suite)
  uint16_t nDays;                // jours stockés, du plus ancien au plus récent
  uint16_t netSz, stSz;          // taille d'un DayNet / d'un DaySt dans CE fichier
  uint16_t pad;
  int32_t  hcKey;
  int32_t  pad2;
  int64_t  prevT;
  int64_t  savedAt;
};
static const uint32_t DAYS_MAGIC = 0x31444E56;     // "VND1"
static const uint16_t DAYS_VER = 1;
static const char * const DAYS_PATH = "/vn_days.bin";

} // namespace vn
