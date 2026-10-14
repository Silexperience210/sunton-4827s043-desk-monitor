// ── Modèle du réseau : stations, historique, profils, prévisions ─────
// Une seule instance (g_model), partagée entre la tâche réseau (écriture,
// sous verrou) et l'UI (lecture, sous verrou). Les gros tableaux vivent en
// PSRAM. Rien ici ne dépend d'Arduino : testé tel quel dans le simulateur.
#pragma once
#include <stdint.h>
#include "../config.h"
#include "gbfs.h"
#include "weather.h"

namespace vn {

// Axe de remplissage : manque de vélos (rouge/ambre) … manque de bornes (bleu/violet)
enum StStatus : uint8_t { ST_EMPTY, ST_LOW, ST_OK, ST_HIGH, ST_FULL, ST_CLOSED, ST_COUNT };

enum AlertType : uint8_t {
  AL_EMPTY, AL_FULL, AL_SOON_EMPTY, AL_SOON_FULL,
  AL_MAINT_BIKES, AL_MAINT_DOCKS, AL_FROZEN, AL_CLOSED,
  AL_UNUSUAL,                  // écart au profil appris (voir Model::recompute)
  AL_TYPE_COUNT
};

// État de la comparaison « valeur courante ↔ profil appris » d'une station
enum AnomState : uint8_t {
  AN_CLOSED,                   // station fermée ou sans relevé : non évaluée
  AN_OFF_HOURS,                // hors heures d'activité (avant 7 h, à partir de 22 h)
  AN_NO_PROFILE,               // aucun profil pour ce créneau
  AN_YOUNG,                    // profil trop jeune (confiance insuffisante)
  AN_NORMAL,                   // dans la tolérance
  AN_HIGH, AN_LOW              // inhabituel : au-dessus / en dessous de l'attendu
};

// ── Leviers de prévision ─────────────────────────────────────────────
// Un interrupteur par levier, pour que sim/bench_prevision mesure chacun SÉPARÉMENT
// (docs/mesures-prevision.md). Les valeurs par défaut sont celles de la carte.
struct PredictOpts {
  // (étape 1 — tolérance météo — mesurée et non livrée : voir le commit 739b5cf)
};
extern PredictOpts g_pred;

// Disponibilité de la météo pour le modèle (affichée : la carte dit quand elle s'en passe)
enum WxUse : uint8_t {
  WXU_NONE,                    // jamais reçue : comportement sans météo
  WXU_STALE,                   // reçue mais plus vieille que 3 périodes (45 min) : ignorée
  WXU_OK                       // utilisée
};

struct Station {
  // ── statique (station_information)
  char     sid[8];
  char     name[48];
  char     addr[64];
  float    lat, lon;
  uint16_t cap;
  bool     active;           // présente dans le dernier station_information
  bool     live;             // au moins un statut reçu
  // ── temps réel (station_status)
  uint16_t bikes, elec, docks, bikesDis, docksDis;
  bool     installed, renting, returning;
  int64_t  lastReported;
  // ── suivi
  int64_t  lastChange;       // dernière variation détectée (vélos ou bornes)
  int64_t  emptySince, fullSince;
  // ── dérivés
  uint8_t  status;           // StStatus
  bool     frozen;           // aucun mouvement depuis g_set.frozenHours (heures actives)
  int16_t  etaEmpty, etaFull;      // minutes, ‑1 = pas dans l'horizon
  int8_t   need;             // vélos à apporter (+) / retirer (‑) pour viser 50 % à +1 h
  uint8_t  risk;             // 0…100 (priorité d'intervention)
  uint8_t  conf;             // 0…100 (confiance du profil appris)
  int16_t  delta30;          // variation des vélos sur 30 min
  float    fc[VN_FC_STEPS];  // vélos prévus 0…120 min (pas 5 min)
  // Écart au profil (calculé par recompute) : attendu, tolérance ±, confiance du profil
  uint8_t  anom;             // AnomState
  uint8_t  anomConf;         // 0…100
  int16_t  anomExp;          // vélos attendus (profil arrondi), ‑1 = inconnu
  int16_t  anomTol;          // tolérance en vélos, 0 = non évaluée

  uint16_t capEff() const { return (uint16_t)(bikes + docks); }
  float    fill() const { int c = bikes + docks; return c ? (float)bikes / c : 0.f; }
};

struct NetSample {             // un point par tranche de 5 min
  int64_t  t;                  // début de tranche (epoch)
  uint16_t avail, docks, dis, bikesRaw;   // 0xFFFF = pas de donnée
  uint16_t dep, ret;           // retraits / retours observés dans la tranche
  uint8_t  empty, full;
};

struct Totals {
  int stations, open, closed;
  int bikes, elec, docks, bikesDis, docksDis, cap, bikesRaw;
  int byStatus[ST_COUNT];
  int frozen, maint;
  int fleet;                   // parc estimé (max 24 h des vélos en station)
  int inUse;                   // vélos en circulation (estimation)
  bool inUseValid;
  int64_t fleetAt;             // instant du maximum retenu (creux de nuit observé)
  int dep1h, ret1h;            // activité sur 60 min glissantes
  bool actValid;               // au moins 1 h de relevés (sinon les compteurs sont vides)
  int delta1h;                 // variation des vélos dispo sur 1 h (‑ = sorties nettes)
  bool deltaValid;             // au moins 1 h de recul pour la comparer
  float fillPct, balancePct;
  int need, surplus;           // somme des besoins / excédents (stations en tension)
  int nRenting, nReturning;    // stations ouvertes sommées dans bikes / docks
  int histSlots;               // profondeur d'historique 5 min parcourue pour le parc
};

struct Alert {
  uint8_t type, sev;           // sev : 3 critique, 2 haute, 1 moyenne, 0 info
  int16_t st;                  // index station
  int16_t minutes;             // depuis (vide/pleine) ou dans (prévision) ; ‑1
  int16_t value;               // n vélos / bornes HS…
};

struct Transfer { int16_t from, to; uint8_t n; uint16_t distM; };

struct ProfCell { uint16_t a; uint8_t n; } __attribute__((packed));   // moyenne ×256, n plafonné

struct NetEvent { int16_t st; uint8_t type; };   // nouvelle rupture (toast UI)

struct Model {
  int       n = 0;                       // emplacements utilisés
  Station * st = nullptr;                // [VN_MAX_ST]
  // Historique 24 h (pas 5 min) — ring partagé par toutes les stations
  int64_t * ringT = nullptr;             // [VN_RING_SLOTS]
  uint8_t * ring = nullptr;              // [VN_RING_SLOTS][VN_MAX_ST] vélos, 255 = absent
  NetSample* net = nullptr;              // [VN_RING_SLOTS]
  int       head = -1, count = 0;
  // Récent (pas ~1 min) pour la tendance
  int64_t * recT = nullptr;              // [VN_RECENT_SLOTS]
  uint8_t * rec = nullptr;               // [VN_RECENT_SLOTS][VN_MAX_ST]
  int       rHead = -1, rCount = 0;
  // Profils appris (jour de semaine × quart d'heure)
  ProfCell* prof = nullptr;              // [VN_MAX_ST][7][96]
  uint8_t * cellN = nullptr;             // [7][96] échantillons réseau (progression)
  int64_t   learnSince = 0;
  uint32_t  learnSamples = 0;
  // Dérivés
  Totals    tot{};
  float     netFc[VN_FC_STEPS] = {};
  int       netConf = 0;
  uint8_t   wxUse = WXU_NONE;            // WxUse, évaluée par recompute
  Alert *   alerts = nullptr;            // [VN_MAX_ALERTS]
  int       nAlerts = 0;
  int       alertCount[AL_TYPE_COUNT] = {};
  Transfer  tr[VN_MAX_TRANSFERS];
  int       nTr = 0;
  // Évènements pour l'UI (ruptures nouvelles)
  NetEvent  ev[8];
  int       nEv = 0;
  // Météo + état des flux
  Weather   wx;
  ElecTypes elec;
  int64_t   lastUpdate = 0;              // dernier station_status réussi (horloge locale)
  int64_t   feedUpdated = 0;             // last_updated du flux
  int64_t   infoAt = 0;
  int       fetchErrors = 0;             // échecs consécutifs
  uint32_t  fetchMs = 0;                 // durée du dernier cycle
  uint32_t  version = 0;                 // incrémenté à chaque mise à jour
  bool      dirty = false;               // à sauvegarder

  bool init();
  int  find(const char * sid) const;
  void applyInfo(const InfoRec * r, int n, int64_t now);
  void applyStatus(const StatusRec * r, int n, int64_t now);
  int64_t ruptureStart(int i, bool empty, int64_t now) const;
  void recompute(int64_t now);           // prévisions, alertes, totaux (aussi après réglages)

  // Profil appris : moyenne attendue des vélos à l'instant t (lissée ±15 min, pool semaine/week-end)
  bool  profileAt(int s, int64_t t, float & avg, float & nEff) const;
  // Projection d'une station sur nSteps pas de stepMin minutes à partir de now (point 0 = actuel),
  // avec la MÊME règle que la prévision 2 h (profil + tendance amortie). conf[k] = 0…1 par pas
  // (confiance du profil sur ce créneau). Renvoie false si la station est inconnue.
  bool  project(int s, int64_t now, int stepMin, int nSteps, float * out, float * conf) const;
  float learnPct() const;                // 0…100
  int   ringIndex(int back) const;       // index du point « back » tranches en arrière (‑1)

  // Sauvegarde : serialize() sous verrou (rapide), puis writeSaved() hors verrou (flash lente)
  struct Blob { uint8_t * prof = nullptr; size_t profLen = 0; uint8_t * hist = nullptr; size_t histLen = 0; };
  bool serialize(Blob & out) const;
  static bool writeSaved(Blob & b);
  bool load(int64_t now);
  void resetHistory();
};

extern Model g_model;

const char * statusShort(uint8_t status);        // « VIDE »… (badges)
float haversineM(float lat1, float lon1, float lat2, float lon2);

} // namespace vn
