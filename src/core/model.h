// ── Modèle du réseau : stations, historique, profils, prévisions ─────
// Une seule instance (g_model), partagée entre la tâche réseau (écriture,
// sous verrou) et l'UI (lecture, sous verrou). Les gros tableaux vivent en
// PSRAM. Rien ici ne dépend d'Arduino : testé tel quel dans le simulateur.
#pragma once
#include <stdint.h>
#include "../config.h"
#include "gbfs.h"
#include "weather.h"
#include "days.h"

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
// Ce qu'un jour férié verse au profil
enum HolLearn : uint8_t {
  HL_DAY,                      // la case de son jour de semaine (comportement de la v1.0.8)
  HL_SUNDAY,                   // la case du dimanche (étape 5a-ii)
  HL_SKIP                      // rien : le jour férié n'est pas appris
};
// Un interrupteur par levier, pour que sim/bench_prevision mesure chacun SÉPARÉMENT
// (docs/mesures-prevision.md). Les valeurs par défaut sont celles de la carte.
struct PredictOpts {
  // (étape 1 — tolérance météo — mesurée et non livrée : voir le commit 739b5cf)
  bool wxFlow = true;          // étape 3a : mouvement prévu du profil × (1 − α·(x − x̄ₕ)), α global appris
  // (étape 3b — α par station — mesurée et non livrée : voir le commit a913bcb)
  bool wxForecast = true;      // étape 4 : au-delà de 2 h, x = météo PRÉVUE à t+h (≤ 2 h : la météo actuelle)
  bool holidaySunday = true;   // étape 5a-ii : un jour férié se prévoit (et s'évalue) avec le profil du dimanche
  // Ce qu'un jour férié verse au profil (levier « fériés » (a), mesuré : docs/mesures-prevision.md)
  uint8_t holidayLearn = HL_SUNDAY;
  // Constante de temps du profil, en relevés de la cellule (3 par semaine) : au-delà de la montée
  // (moyenne simple des premiers relevés), a ← a + (x − a) / profDecayN, soit une décroissance
  // exponentielle de facteur λ = 1 − 1/profDecayN par relevé (24 = 8 semaines, levier « récence »).
  int profDecayN = VN_PROFILE_DECAY_N;
  // Rajeunissement ciblé (mesuré, non livré : 0) : quand station_information annonce une nouvelle capacité
  // (écart ≥ 2 bornes et ≥ 10 %), les cellules de CETTE station repartent avec n ≤ rejuvN — elles
  // réapprennent à la vitesse de la montée, le reste du parc garde sa mémoire.
  int rejuvN = 0;
};
extern PredictOpts g_pred;
// Poids du relevé suivant dans une cellule qui en a déjà compté n (n saturé à 31) : 1/(n + 1) pendant la
// montée (moyenne simple), puis 1/N (décroissance exponentielle, λ = 1 − 1/N). N < 2 est traité comme 2.
float profWeight(int n, int N);
int   profCountMax(int N);         // plafond de n : min(N, 31)

// (Alerte « inhabituel » : leviers 1a — tolérance tirée de la dispersion —, 1b — inertie —, témoin et
// fériés (c) mesurés sans gagnant puis retirés du code : docs/mesures-prevision.md, « alerte calme ».)

// Disponibilité de la météo pour le modèle (affichée : la carte dit quand elle s'en passe)
enum WxUse : uint8_t {
  WXU_NONE,                    // jamais reçue : comportement sans météo
  WXU_STALE,                   // reçue mais plus vieille que 3 périodes (45 min) : ignorée
  WXU_OK                       // utilisée
};

// Origine du coefficient météo (affichée avec la part de la prévision qu'il explique)
enum WxCoefSrc : uint8_t {
  WXC_DEFAULT,                 // trop peu de pluie observée pour juger : α = 0, aucune correction
  WXC_NULL,                    // assez de pluie observée, effet non distinguable du bruit : α = 0
  WXC_LEARNED                  // appris, au-delà de 3 erreurs-types (WX_SIGNIF)
};

// Statistiques suffisantes de l'apprentissage météo (décroissance exponentielle, ~60 jours).
// Par fenêtre de 30 min et par station : d = mouvement du profil, r = mouvement observé − d,
// z = (x − x̄ₕ)·d, x̄ₕ = pénalité moyenne à cette heure de la journée (ce que le profil moyenne).
// Régression r = β·d − α·z : β absorbe le biais du profil (lissage, jeunesse) quel que soit le
// temps, α est l'effet de la météo. Erreur-type robuste par grappe (une fenêtre = une grappe pour
// le parc : les stations d'une même fenêtre partagent la même météo).
struct WxStats {
  float sdd, sdz, szz;         // Σd², Σd·z, Σz²
  float sdr, szr;              // Σd·r, Σz·r
  float sgg;                   // Σ (Σ_grappe z·r)²
  float sn;                    // observations (fenêtres × stations)
  float nRain;                 // fenêtres pluvieuses (x > 0,05)
};
struct WxClimate { float sx[24], sw[24]; };  // pénalité moyenne par heure locale (x̄ₕ)

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
  float    fcWx[VN_FC_STEPS];// dont part due à la météo (0 sans correction météo)
  // Écart au profil (calculé par recompute) : attendu, tolérance ±, confiance du profil
  uint8_t  anom;             // AnomState
  uint8_t  anomConf;         // 0…100
  int16_t  anomExp;          // vélos attendus (profil arrondi), ‑1 = inconnu
  int16_t  anomTol;          // tolérance en vélos, 0 = non évaluée
  // Écart habituel du créneau (dispersion apprise), affiché pour information : il ne décide de rien
  bool     anomDispOk;       // dispersion du créneau connue (assez de relevés)
  uint8_t  anomDisp10;       // dispersion habituelle du créneau, dixièmes de vélo
  uint16_t anomDispDays;     // jours de relevés sur lesquels la dispersion a été mesurée

  uint16_t capEff() const { return (uint16_t)(bikes + docks); }
  float    fill() const { int c = bikes + docks; return c ? (float)bikes / c : 0.f; }
};

struct NetSample {             // un point par tranche de 5 min
  int64_t  t;                  // début de tranche (epoch)
  uint16_t avail, docks, dis, bikesRaw;   // 0xFFFF = pas de donnée
  uint16_t dep, ret;           // retraits / retours observés dans la tranche
  uint8_t  empty, full;
  // Météo au moment de la tranche (dernier relevé Open-Meteo, s'il est frais : WXU_OK).
  // Inconnue (WX_NA / WX_TEMP_NA) si la météo manquait, pour une tranche comblée après un
  // trou, ou pour un historique d'avant le format v2 de /vn_hist.bin.
  int8_t   wxTemp;             // °C arrondi (‑127…127)
  uint8_t  wxRain;             // précipitation actuelle, dixièmes de mm (plafond 25,4 mm)
  uint8_t  wxProb;             // probabilité de pluie de l'heure en cours, %
  uint8_t  wxWind;             // vent, km/h (plafond 254)
  uint8_t  wxCode;             // code WMO
};
static const uint8_t WX_NA = 255;
static const int8_t  WX_TEMP_NA = -128;
inline void netWxUnknown(NetSample & ns) {
  ns.wxTemp = WX_TEMP_NA; ns.wxRain = ns.wxProb = ns.wxWind = ns.wxCode = WX_NA;
}
inline bool netWxKnown(const NetSample & ns) { return ns.wxTemp != WX_TEMP_NA && ns.wxRain != WX_NA; }

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

// Cellule de profil (jour × quart d'heure) : moyenne ×256 et n = relevés comptés, plafonné à
// min(g_pred.profDecayN, 31) (5 bits) — voir profWeight,
// dispersion d = écart absolu moyen au profil ×16 (plafond 15,9 vélos) et son compte dn (3 bits,
// plafonné à 7 : au-delà, d suit la même moyenne mobile que a). d n'est pas utilisé tant que dn = 0.
struct ProfCell { uint16_t a; uint8_t n : 5; uint8_t dn : 3; uint8_t d; } __attribute__((packed));

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
  int64_t * dispSince = nullptr;         // [VN_MAX_ST] premier relevé versé à la dispersion (0 = jamais)
  // Écart du relevé au profil, mis en attente : versé à la dispersion à la tranche suivante, pour que
  // le relevé soit toujours jugé contre une dispersion qui ne le contient pas (un incident ne se masque pas)
  float *   dispPendDev = nullptr;         // [VN_MAX_ST]
  int16_t * dispPendCell = nullptr;        // [VN_MAX_ST] cellule + 1 (0 = rien en attente)
  int64_t   learnSince = 0;
  uint32_t  learnSamples = 0;
  // Résumés par jour (vue Service) : alimentés à chaque tranche de 5 min relevée
  DayBook   days;
  // Dérivés
  Totals    tot{};
  float     netFc[VN_FC_STEPS] = {};
  int       netConf = 0;
  uint8_t   wxUse = WXU_NONE;            // WxUse, évaluée par recompute
  // Coefficient météo appris (étape 3) — sur le mouvement net, jamais sur le niveau constaté
  WxStats   wxG{};                       // parc entier
  WxClimate wxClim{};                    // x̄ₕ
  WxStats * wxS = nullptr;               // [VN_MAX_ST] par station (appris et sauvegardé ; non appliqué,
                                         // voir docs/mesures-prevision.md, étape 3b)
  float     wxAlpha = 0;                 // α global appliqué (0 si non significatif)
  float     wxAlphaRaw = 0, wxAlphaSE = 0; // estimation brute et son erreur-type
  float     wxXbar = 0;                  // x̄ₕ de l'heure courante (affichage)
  uint8_t   wxSrc = WXC_DEFAULT;         // WxCoefSrc
  float     wxNow = 0;                   // pénalité météo actuelle x (0…1), 0 sans météo
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
  WxUse wxUseAt(int64_t now) const;      // météo utilisable à l'instant now ?
  int  find(const char * sid) const;
  void applyInfo(const InfoRec * r, int n, int64_t now);
  void applyStatus(const StatusRec * r, int n, int64_t now);
  int64_t ruptureStart(int i, bool empty, int64_t now) const;
  void recompute(int64_t now);           // prévisions, alertes, totaux (aussi après réglages)

  // Profil appris : moyenne attendue des vélos à l'instant t (lissée ±15 min, pool semaine/week-end)
  bool  profileAt(int s, int64_t t, float & avg, float & nEff) const;
  // Vrai si l'instant t est un jour férié prévu avec le profil du dimanche (étape 5a-ii) : provenance affichable
  bool  holidayProfile(int64_t t) const;
  // Dispersion habituelle (écart absolu moyen au profil, vélos) à l'instant t, lissée comme le profil ;
  // cnt = relevés qui la fondent. false si aucun relevé.
  bool  dispersionAt(int s, int64_t t, float & d, float & cnt) const;
  // Projection d'une station sur nSteps pas de stepMin minutes à partir de now (point 0 = actuel),
  // avec la MÊME règle que la prévision 2 h (profil + tendance amortie). conf[k] = 0…1 par pas
  // (confiance du profil sur ce créneau). Renvoie false si la station est inconnue.
  bool  project(int s, int64_t now, int stepMin, int nSteps, float * out, float * conf, float * wxPart = nullptr) const;
  // Pénalité météo attendue x(τ) pour un instant τ ≥ now ; false si inconnue (pas de correction)
  bool  wxPenaltyAt(int64_t now, int64_t tau, float & x) const;
  float wxClimAt(int64_t t) const;       // x̄ₕ de l'heure locale de t (0 si inconnue)
  void  learnWeather(int64_t slotT);     // une fenêtre de 30 min (appelée par applyStatus)
  void  learnDispersion(int s, int cell, int64_t t);   // une tranche de 5 min, avant la moyenne (applyStatus)
  void  updateWxCoef();                  // α global depuis les statistiques
  float learnPct() const;                // 0…100
  int   ringIndex(int back) const;       // index du point « back » tranches en arrière (‑1)

  // Sauvegarde : serialize() sous verrou (rapide), puis writeSaved() hors verrou (flash lente)
  struct Blob { uint8_t * prof = nullptr; size_t profLen = 0; uint8_t * hist = nullptr; size_t histLen = 0;
                uint8_t * days = nullptr; size_t daysLen = 0; };
  bool serialize(Blob & out) const;
  static bool writeSaved(Blob & b);
  bool load(int64_t now);
  void resetHistory();
};

extern Model g_model;

const char * statusShort(uint8_t status);        // « VIDE »… (badges)
const char * holidayNameAt(int64_t t);           // nom du jour férié légal de l'instant t (heure locale), nullptr sinon
float haversineM(float lat1, float lon1, float lat2, float lon2);

} // namespace vn
