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
  AL_FLEET_MAINT,              // part de vélos hors service sur l'ENSEMBLE du parc (st = ‑1)
  AL_UNUSUAL,                  // écart au profil appris (voir Model::recompute)
  AL_FEED_FROZEN,              // aucune station n'a bougé : le flux est figé (st = ‑1, réseau)
  AL_TYPE_COUNT
};
// Seuil réseau : les stations sont déjà signalées une par une (AL_MAINT_BIKES) ; ici on parle du
// parc entier. 15 % = un vélo sur sept non louable, déjà anormal pour un service public.
static const int FLEET_MAINT_PCT = 15;
static const int FLEET_MAINT_PCT_HIGH = 30;   // sévérité haute : un vélo sur trois

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
  HL_DAY,                      // la case de son jour de semaine (comportement de la v1.0.8, référence du banc)
  HL_SUNDAY                    // la case du dimanche (étape 5a-ii, livré)
  // (« rien : le jour férié n'est pas appris » — 5a-i — a perdu deux fois : retiré)
};
// Règle de prévision (docs/mesures-modele.md)
enum FcRule : uint8_t {
  FC_RULE_V1 = 1,              // ≤ 1.3.0 : actuel + c·ΔP + (1 − 0,6c)·tendance amortie — plus mauvaise que la
                               // persistance sur données réelles (+15 à +60 min)
  FC_RULE_V2 = 2               // modèle 2 : actuel + a(h)·ΔP + b(h)·(profil(t) − actuel), a et b appris (livré)
};
// Un interrupteur par levier, pour que sim/bench_prevision mesure chacun SÉPARÉMENT
// (docs/mesures-prevision.md). Les valeurs par défaut sont celles de la carte.
struct PredictOpts {
  uint8_t fcRule = FC_RULE_V2; // levier « modele » : règle des valeurs prévues (fc, courbe 24 h, besoin)
  // Trajectoire d'où se lisent les échéances « vide / pleine dans X min » (alertes, risque) : la règle 1.3.0,
  // dont la tendance porte les alertes (docs/mesures-modele.md) — FC_RULE_V2 = la prévision affichée (mesuré, non livré)
  uint8_t etaRule = FC_RULE_V1;
  bool  fcDayClass = true;     // modèle 2 : un jeu de coefficients par type de jour (ouvré | week-end et férié)
  bool  fcStation = true;      // modèle 2 : couple propre à chaque station à +60, +120 et +240 min (FcSt, livré) ;
                               // false = le couple de la classe pour toutes (commit 755d7c1), bancs seulement
  float fcHalfLifeDays = 7.f;  // modèle 2 : oubli de l'apprentissage (jours de quarts d'heure notés de la classe)
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
  // (Rajeunissement ciblé sur changement de capacité : mesuré, non livré, retiré — docs/mesures-prevision.md.)
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
// Par fenêtre de 30 min, cumulées sur les stations du parc : d = mouvement du profil, r = mouvement observé − d,
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
  int8_t   sens;             // motif appris de la journée : +1 = se vide le matin et se remplit
                             // le soir (pendulaire) ; ‑1 = l'inverse (arrivées au travail) ;
                             // 0 = pas de motif net, ou pas assez de relevés pour le dire
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
  int fleet;                   // parc estimé (max 24 h des vélos en station, à couverture égale)
  int inUse;                   // vélos ABSENTS des stations : en trajet, à l'atelier, perdus
  bool inUseValid;             // validé : 6 h de tranches OBSERVÉES + un creux + un maximum en PALIER
  bool inUseProvisional;       // provisoire : au moins 1 h observée, mais pas encore validable
  int histMinutes;             // profondeur d'historique observée, en minutes (trous et coupures exclus)
  int refCover;                // stations couvertes par la tranche de référence
  int refSpikes;               // relevés comparables plus hauts que le parc retenu, écartés (top-K)
  int64_t fleetAt;             // instant du maximum retenu (0 = le relevé courant)
  int dep1h, ret1h;            // activité sur 60 min glissantes
  bool actValid;               // 12 tranches réellement renseignées = 1 h pleine (sinon compteurs vides)
  int delta1h;                 // variation des vélos dispo sur 1 h (‑ = sorties nettes)
  bool deltaValid;             // au moins 1 h de recul pour la comparer
  float fillPct, balancePct;
  int need, surplus;           // somme des besoins / excédents (stations en tension)
  int nRenting, nReturning;    // stations ouvertes sommées dans bikes / docks
  int histSlots;               // lignes de l'anneau parcourues, TROUS COMPRIS (ne pas afficher tel quel)
};

struct Alert {
  uint8_t type, sev;           // sev : 3 critique, 2 haute, 1 moyenne, 0 info
  int16_t st;                  // index station
  int16_t minutes;             // depuis (vide/pleine) ou dans (prévision) ; ‑1
  int16_t value;               // n vélos / bornes HS…
};

// Épisode de rupture journalisé : « #12 vide, 8 h 03 → 8 h 41 (38 min) ».
struct AlertLog { int64_t t; int32_t dur; int16_t st; uint8_t type; };
// Le budget mémoire du journal (VN_ALERT_LOG × 16 o) et le commentaire de config.h reposent sur
// cette taille : un champ ajouté ici doit se voir tout de suite, pas à la première allocation.
static_assert(sizeof(AlertLog) == 16, "AlertLog doit rester à 16 octets (budget du journal)");

struct Transfer { int16_t from, to; uint8_t n; uint16_t distM; };

// Cellule de profil (jour × quart d'heure) : moyenne ×256 et n = relevés comptés, plafonné à
// min(g_pred.profDecayN, 31) (5 bits) — voir profWeight,
// dispersion d = écart absolu moyen au profil ×16 (plafond 15,9 vélos) et son compte dn (3 bits,
// plafonné à 7 : au-delà, d suit la même moyenne mobile que a). d n'est pas utilisé tant que dn = 0.
struct ProfCell { uint16_t a; uint8_t n : 5; uint8_t dn : 3; uint8_t d; } __attribute__((packed));

struct NetEvent { int16_t st; uint8_t type; };   // nouvelle rupture (toast UI)

// ── Modèle 2 : coefficients de prévision appris en ligne (docs/mesures-modele.md) ──
//   prévu(h) = actuel + a(h)·[profil(t+h) − profil(t)] + b(h)·[profil(t) − actuel], borné à [0, capacité utile]
// Un couple (a, b) par classe de jour (ouvré | week-end et férié, la classe de l'instant t) et par horizon
// d'ancrage (15, 30, 60, 120, 240 min), interpolé entre eux (0 à h = 0, celui de 240 min au-delà, non mesuré).
// Apprentissage : tous les quarts d'heure de 6 h à 22 h, chaque station ouverte dont le profil est connu à t et
// à t + h émet une prévision « d'apprentissage » par horizon (ses deux écarts au profil ×64, le relevé, la
// capacité) ; à l'échéance, le relevé de l'anneau la juge pour CHAQUE couple d'une grille (a = 0…1,5 au pas de
// 0,1, b = 0…1 au pas de 0,05) : erreur absolue de la prévision arrondie et bornée — la règle de la notation du
// serveur —, calculée en entiers ; son cumul, avec oubli, en flottants. Le couple appliqué minimise
// l'erreur moyenne + 0,005 × (a + b) (vers la persistance ; à égalité, le premier de la grille : a puis b
// croissants), avec oubli (demi-vie g_pred.fcHalfLifeDays) ; tant qu'une case (classe, horizon) a moins de
// FC_MIN_LOTS quarts d'heure jugés — une journée complète de 6 h à 22 h, ≈ 9 000 prévisions sur le parc réel —,
// a = b = 0 : la persistance (avec 8, la classe week-end quittait la persistance à +240 min sur 9 quarts d'heure
// jugés, tous du samedi, et perdait 38 % contre elle le dimanche 01/11 : docs/mesures-modele.md). Une station
// sans profil à t ou à t + h n'apprend rien et se prévoit par la persistance à cet horizon.
// Couples par station (g_pred.fcStation, livré) : aux horizons de 60, 120 et 240 min, chaque station choisit
// son couple parmi une sous-grille de 6 × 5 (FC_SA × FC_SB) sur SES erreurs (mêmes prévisions d'apprentissage,
// même règle d'erreur, même oubli) plus FC_KAPPA prévisions fictives de sa classe (l'erreur moyenne de la classe à
// ce couple) : une station peu observée garde le meilleur couple de la classe sur la sous-grille, une station
// régulière ou bruitée s'en écarte. Choix refait quand un lot de sa classe est jugé à cet horizon, gardé dans
// le fichier ; tant que la classe n'a pas FC_MIN_LOTS quarts d'heure jugés à cet horizon : la persistance.
// +15 et +30 min gardent le couple de la classe (par station, la perte y était mesurée : docs/mesures-modele.md).
// Seul le flux GBFS l'alimente : rien de ce que l'utilisateur règle ou saisit n'y entre.
static const int FC_NC = 2;                         // classes de jour : 0 = ouvré, 1 = week-end et férié
static const int FC_NH = 5;                         // horizons d'ancrage
static const int FC_NA = 16, FC_NB = 21;            // grille a × b
static const int FC_NG = FC_NA * FC_NB;
static const int FC_PEND = 17;                      // lots d'émission en attente : 240 / 15 + 1
extern const int16_t FC_HMIN[FC_NH];                // 15, 30, 60, 120, 240 (minutes)
static const float FC_MIN_LOTS = 64.f;              // quarts d'heure jugés (au moins une prévision) avant de quitter la persistance
static const float FC_PEN = 0.005f;                 // vélo par unité de a + b
static const float FC_FIX = 64.f;                   // écarts au profil, en 1/64 de vélo
// Statistiques de l'apprentissage, sauvegardées dans /vn_prof.bin (versions 5 et 6) : 13 520 octets
struct FcLearn {
  float err[FC_NC][FC_NH][FC_NG];   // Σ |prévision arrondie − relevé| par couple (a, b), avec oubli
  float w[FC_NC][FC_NH];            // prévisions jugées, avec le même oubli
  float lots[FC_NC][FC_NH];         // quarts d'heure jugés (au moins une prévision), avec le même oubli
};
static const int FC_SH0 = 2;                        // premier horizon à couples par station (60 min)
static const int FC_NSH = FC_NH - FC_SH0;           // 60, 120, 240 min
static const int FC_SNA = 6, FC_SNB = 5;            // sous-grille des stations
static const int FC_SNG = FC_SNA * FC_SNB;
extern const uint8_t FC_SA[FC_SNA];                 // indices de a dans la grille : 0 ; 0,2 ; 0,5 ; 0,7 ; 1 ; 1,2
extern const uint8_t FC_SB[FC_SNB];                 // indices de b : 0 ; 0,1 ; 0,2 ; 0,35 ; 0,5
static const float FC_KAPPA = 200.f;                // prévisions fictives de la classe (rétrécissement)
static const uint8_t FC_SEL_NONE = 255;             // pas de couple propre : celui de la classe
// Apprentissage d'une station, sauvegardé dans /vn_prof.bin (version 6, une par station) : 752 octets
struct FcSt {
  float err[FC_NC][FC_NSH][FC_SNG];  // Σ |erreur| de ses prévisions d'apprentissage par couple de la sous-grille
  float w[FC_NC][FC_NSH];            // ses prévisions jugées
  uint8_t sel[FC_NC][FC_NSH];        // couple choisi (index de la sous-grille), FC_SEL_NONE = celui de la classe
  uint8_t pad[2];
};
static const float FC_ERR_MIN = 64.f;               // prévisions jugées avant de publier l'erreur d'une station (un jour)
struct FcErr { float mae; float n; bool own; };     // erreur absolue moyenne (vélos), prévisions jugées (avec oubli)
// Prévision d'apprentissage en attente de son échéance (PSRAM, non sauvegardée : un redémarrage perd au plus
// les 4 dernières heures d'apprentissage)
struct FcIssue {
  int16_t x1[FC_NH];           // profil(t+h) − profil(t), ×64
  int16_t x2[FC_NH];           // profil(t) − actuel, ×64
  uint8_t cur;                 // relevé à l'émission, 255 = rien d'émis
  uint8_t cap;                 // capacité utile à l'émission
  uint8_t use;                 // bit h : profil connu à t et à t + h (la prévision compte pour l'apprentissage)
};

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
  WxStats   wxG{};                       // parc entier (le coefficient par station, étape 3b, n'a pas gagné : retiré)
  WxClimate wxClim{};                    // x̄ₕ
  float     wxAlpha = 0;                 // α global appliqué (0 si non significatif)
  float     wxAlphaRaw = 0, wxAlphaSE = 0; // estimation brute et son erreur-type
  uint8_t   wxSrc = WXC_DEFAULT;         // WxCoefSrc
  float     wxNow = 0;                   // pénalité météo actuelle x (0…1), 0 sans météo
  // Modèle 2 : coefficients appris (fcL, sauvegardé), prévisions en attente (fcPend), couples appliqués
  FcLearn * fcL = nullptr;               // PSRAM
  FcIssue * fcPend = nullptr;            // [FC_PEND][VN_MAX_ST], PSRAM
  uint32_t * fcLotErr = nullptr;         // [FC_NG], PSRAM : erreurs d'un lot par couple (calcul de fcLearnSlot)
  int64_t   fcPendT[FC_PEND] = {};       // tranche d'émission de chaque lot (0 = vide)
  uint8_t   fcPendC[FC_PEND] = {};       // classe de jour de chaque lot
  int       fcPendHead = -1;             // dernier lot émis
  float     fcA[FC_NC][FC_NH] = {}, fcB[FC_NC][FC_NH] = {};
  FcSt *    fcS = nullptr;               // [VN_MAX_ST], PSRAM : couples par station (sauvegardé, v6)
  // Interventions DÉCLARÉES (registre de régulation) : heure de la dernière par station, 0 = aucune. Une prévision
  // d'apprentissage dont la fenêtre [émission, échéance] contient une intervention déclarée n'est pas jugée : son
  // erreur est celle du camion, que rien ne permettait de prévoir, pas celle du réglage. Jamais déduit du flux
  // (feuille de route) : sans déclaration, rien n'est écarté. En mémoire seulement ; le serveur les garde dans
  // /vn_live.bin. Les profils (niveaux habituels) ne sont pas touchés : le vélo déposé est bien là.
  int64_t * ivAt = nullptr;              // [VN_MAX_ST], PSRAM
  uint32_t  fcIvSkipped = 0;             // prévisions d'apprentissage écartées pour cette raison (depuis le démarrage)
  void  noteIntervention(int i, int64_t t);
  Alert *   alerts = nullptr;            // [VN_MAX_ALERTS] — les plus graves, pas les premières
  int       nAlerts = 0;
  int       nAlertsTotal = 0;            // toutes, y compris celles écartées par le plafond
  int       alertCount[AL_TYPE_COUNT] = {};   // compteurs vrais (avant plafond)
  Transfer  tr[VN_MAX_TRANSFERS];
  int       nTr = 0;
  // Évènements pour l'UI (ruptures nouvelles)
  NetEvent  ev[8];
  int       nEv = 0;
  // Journal des ruptures (PSRAM, perdu au redémarrage) : la seule trace qu'une station est restée
  // vide de 8 h 03 à 8 h 41. Une entrée par épisode, `dur == 0` = encore en cours.
  AlertLog *log = nullptr;                // [VN_ALERT_LOG]
  int       nLog = 0;
  int       logHead = -1;                 // dernière entrée écrite
  // Météo + état des flux
  Weather   wx;
  ElecTypes elec;
  int64_t   lastUpdate = 0;              // dernier station_status réussi (horloge locale)
  int64_t   feedUpdated = 0;             // last_updated du flux
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
  // Motif de la journée, lu dans le PROFIL APPRIS (6 h / 9 h / 18 h) : dit si la station se vide
  // le matin pour se remplir le soir (+1), l'inverse (‑1), ou rien de net (0). Sert à ne pas
  // confondre « pleine ce soir » et « à vider » : mesuré sur 12 jours ouvrés réels, 69 stations
  // sur 139 s'inversent (Gare Centrale 2 : −94 vélos le matin, +53 le soir, 12 j sur 12).
  int8_t sensJour(int s, int64_t now) const;
  // Vrai si l'instant t est un jour férié prévu avec le profil du dimanche (étape 5a-ii) : provenance affichable
  bool  holidayProfile(int64_t t) const;
  // Dispersion habituelle (écart absolu moyen au profil, vélos) à l'instant t, lissée comme le profil ;
  // cnt = relevés qui la fondent. false si aucun relevé.
  bool  dispersionAt(int s, int64_t t, float & d, float & cnt) const;
  // Projection d'une station sur nSteps pas de stepMin minutes à partir de now (point 0 = actuel),
  // avec la MÊME règle que la prévision 2 h (g_pred.fcRule). conf[k] = 0…1 par pas
  // (confiance du profil sur ce créneau). Renvoie false si la station est inconnue.
  bool  project(int s, int64_t now, int stepMin, int nSteps, float * out, float * conf, float * wxPart = nullptr) const;
  // Pénalité météo attendue x(τ) pour un instant τ ≥ now ; false si inconnue (pas de correction)
  bool  wxPenaltyAt(int64_t now, int64_t tau, float & x) const;
  float wxClimAt(int64_t t) const;       // x̄ₕ de l'heure locale de t (0 si inconnue)
  void  learnWeather(int64_t slotT);     // une fenêtre de 30 min (appelée par applyStatus)
  void  learnDispersion(int s, int cell, int64_t t);   // une tranche de 5 min, avant la moyenne (applyStatus)
  void  updateWxCoef();                  // α global depuis les statistiques
  // Modèle 2 : une tranche réelle de 5 min (appelée par applyStatus, profils déjà nourris) — juge les
  // prévisions d'apprentissage arrivées à échéance, puis en émet si c'est un quart d'heure de 6 h à 22 h
  void  fcLearnSlot(int64_t slotT, int64_t now);
  void  fcSelect();                      // couples (a, b) appliqués, depuis fcL
  void  fcSelectStations(int cls, int h); // couple de chaque station (horizon d'ancrage h ≥ FC_SH0), depuis fcS et fcL
  // Couple interpolé à l'horizon hMin (minutes) : celui de la classe, ou de la station s (≥ 0) à partir de 60 min
  void  fcCoefAt(int cls, float hMin, float & a, float & b, int s = -1) const;
  int   fcClassAt(int64_t t) const;      // classe de jour des coefficients (0 ouvré, 1 week-end et férié)
  // Erreur MESURÉE du réglage appliqué à l'horizon d'ancrage h (index de FC_HMIN) pour la classe cls : erreur
  // absolue moyenne, sur les prévisions d'apprentissage jugées (avec le même oubli), du couple que la prévision
  // de la station i applique aujourd'hui. « own » : mesurée sur les prévisions de la station elle-même (≥ +60 min,
  // au moins FC_ERR_MIN jugées) ; sinon sur celles de toute sa classe — l'erreur moyenne du parc, dite comme telle.
  // false : rien de mesuré (classe sans prévision jugée, ou station sans profil — elle se prévoit alors par la
  // persistance, dont l'erreur n'est pas celle de la classe). C'est l'erreur qu'aurait faite le réglage actuel sur
  // ces prévisions passées ; l'erreur réellement constatée est notée par le serveur (lib/forecast.mjs).
  bool  fcErrAt(int i, int cls, int h, int64_t now, FcErr & e) const;
  float learnPct() const;                // 0…100
  int   ringIndex(int back) const;       // index du point « back » tranches en arrière (‑1)
  // Mouvements (départs + retours) de la tranche courante : la même grandeur que les barres
  // « mouvements / heure », versée dans le résumé du jour pour les échelles 1 semaine / 1 mois.
  int   slotMoves() const { return head >= 0 ? (int)net[head].dep + (int)net[head].ret : 0; }

  // Sauvegarde : serialize() sous verrou (rapide), puis writeSaved() hors verrou (flash lente)
  struct Blob { uint8_t * prof = nullptr; size_t profLen = 0; uint8_t * hist = nullptr; size_t histLen = 0;
                uint8_t * days = nullptr; size_t daysLen = 0; };
  bool serialize(Blob & out) const;
  static bool writeSaved(Blob & b);
  bool load(int64_t now);
  // Contrôles d'en-tête et de taille appliqués par load() à /vn_prof.bin et /vn_hist.bin (format connu,
  // taille exacte ; pour l'historique, sauvegardé depuis moins de 20 h à l'instant now). La synchro en
  // ligne (core/sync) les applique AVANT d'écrire un fichier reçu : un fichier que load() refuserait
  // n'efface jamais celui qui est en flash.
  static bool profFileOk(const uint8_t * b, size_t len);
  static bool histFileOk(const uint8_t * b, size_t len, int64_t now);
  void resetHistory();
};

extern Model g_model;

const char * statusShort(uint8_t status);        // « VIDE »… (badges)
const char * holidayNameAt(int64_t t);           // nom du jour férié légal de l'instant t (heure locale), nullptr sinon
float haversineM(float lat1, float lon1, float lat2, float lon2);

} // namespace vn
