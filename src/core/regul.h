// ── Rôle « régulateur » : le Sunton du camion (online/lib/regulation.mjs) ─
// Le serveur fait le lourd : registre des tournées, plan (ESTIMÉ, avec sa marge mesurée), recoupement de
// chaque arrêt déclaré avec le flux public. Le Sunton du camion, lui, montre le prochain arrêt et fait
// VALIDER au régulateur ce qui a vraiment été fait : déposés, pris, hors service ramassés, atelier.
// Rien n'est déduit du flux public : un groupe de neuf personnes qui prend neuf vélos n'est pas un
// régulateur, seul ce que le régulateur déclare compte comme intervention.
//
// Hors connexion (partage de connexion du téléphone coupé, tunnel), chaque geste est rangé dans une file
// gardée en flash (/vn_regq.bin) puis envoyé dans l'ordre ; chaque déclaration a un identifiant unique,
// le serveur reconnaît un renvoi et ne la compte qu'une fois. Un refus du serveur sort de la file avec sa
// raison, affichée telle quelle.
//
// Portable (aucun Arduino) : vue locale, contrôles, file, analyse de la réponse et déroulé d'un échange
// sont testés par sim/test_core.cpp, et contre le vrai serveur par sim/reg_sim.cpp ; le transport HTTP est
// fourni par la plateforme (net_esp32.cpp, ou curl au simulateur).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vn {

// ── Réglage (fichier texte /vn_reg.cfg) ──────────────────────────────
enum RegRole : uint8_t { ROLE_POSTE = 0, ROLE_REGUL = 1 };
struct RegCfg {
  uint8_t role;                // ROLE_POSTE : écran de supervision ; ROLE_REGUL : écran « Tournée » en premier onglet
  char    lastReg[24];         // dernier régulateur choisi (proposé au départ suivant)
  char    lastTruck[24];       // dernier camion choisi
};
extern RegCfg g_regCfg;
static const char * const REG_CFG_PATH = "/vn_reg.cfg";
static const char * const REG_QUEUE_PATH = "/vn_regq.bin";
static const char * const REG_SNAP_PATH = "/vn_regsnap.json";
void regCfgDefaults(RegCfg & c);
bool regCfgLoad();
bool regCfgSave();

enum : int {
  REG_MAX_TRUCKS = 6, REG_MAX_PEOPLE = 8, REG_MAX_OPEN = 4, REG_MAX_STOPS = 8, REG_MAX_EVENTS = 8,
  REG_MAX_QUEUE = 24, REG_MAX_SENT = 8, REG_MAX_REJ = 4, REG_MAX_N = 99,
};

struct RegCount { int16_t ok, hs; };   // vélos en service / hors service
enum RegKind : uint8_t { RK_STATION = 0, RK_ATELIER = 1 };
// Recoupement d'un arrêt déclaré avec le flux public (fait par le serveur, 10 min après l'arrêt)
enum RegVerdict : int8_t { RV_NONE = 0, RV_CONCORDANT, RV_ECART, RV_NONVISIBLE, RV_HORSFLUX };

// ── Ce que le serveur renvoie (GET /reg/tour) ────────────────────────
struct RegTruck  { char id[24]; char name[40]; uint8_t cap; };
struct RegPerson { char id[24]; char name[40]; };
struct RegOpen   { char id[48]; char truckName[40]; char regName[40]; char device[32]; int64_t startedAt; };
struct RegStop {
  uint8_t  seq, kind;          // RegKind
  char     sid[8];
  char     name[48];
  RegCount drop, take;         // PROPOSÉ par le plan (estimé)
  int16_t  etaMin;             // arrivée estimée, minutes (‑1 inconnue)
  int16_t  benefitMin;         // minutes de rupture évitées (ESTIMÉES)
  int64_t  arriveAt;
  char     why[200];           // pourquoi cet arrêt, avec la classe de chaque nombre (mesuré / estimé)
  bool     hasNow;             // état de la station au calcul du plan (mesuré)
  int16_t  nowBikes, nowDocks, nowDis, nowCap;
  RegCount loadAfter;
};
struct RegEvent {
  char     id[64];
  uint8_t  kind;
  char     sid[8];
  char     name[48];
  int64_t  at;
  RegCount took, dropped;      // DÉCLARÉ par le régulateur
  char     impossible[48];     // raison, vide si l'arrêt a été fait
  int8_t   verdict;            // RegVerdict
  int16_t  declOk, obsOk;      // concordant / écart : variation déclarée et observée de la station (vélos en service)
};
struct RegSnap {
  bool      valid;
  int64_t   serverNow;
  char      device[32];        // l'identifiant sous lequel le serveur voit ce Sunton
  RegTruck  trucks[REG_MAX_TRUCKS];
  uint8_t   nTrucks;
  RegPerson regs[REG_MAX_PEOPLE];
  uint8_t   nRegs;
  char      atelierName[40];
  bool      atelierSet;        // point de l'atelier déclaré sur le serveur
  int16_t   atelierStock;      // vélos neufs/réparés déclarés à l'atelier (‑1 : non déclaré)
  RegOpen   open[REG_MAX_OPEN];
  uint8_t   nOpen;
  // tournée de cet appareil (ou rejointe)
  bool      hasTour;
  char      tourId[48];
  char      tourDevice[32];
  char      truckName[40], regName[40];
  uint8_t   cap;
  int64_t   startedAt;
  RegCount  load;              // départ déclaré + somme des arrêts déclarés (calculé par le serveur)
  bool      hasPlan;
  RegStop   stops[REG_MAX_STOPS];
  uint8_t   nStops;
  int16_t   planBenefit, planDuration;
  char      method[480];       // comment le plan est calculé et d'où vient sa marge (texte du serveur)
  RegEvent  events[REG_MAX_EVENTS];   // dernières déclarations, la plus récente d'abord
  uint8_t   nEvents;
};
bool regParseTour(const char * json, size_t len, RegSnap & out);

// ── File des gestes en attente d'envoi ───────────────────────────────
enum RegOp : uint8_t { RO_START = 1, RO_EVENT = 2, RO_END = 3 };
struct RegItem {
  uint8_t  op, kind;
  int16_t  seq;
  char     id[48];             // START : identifiant de la tournée ; EVENT : identifiant de la déclaration
  char     tour[48];           // EVENT, END
  char     truck[24], reg[24]; // START (identifiants du serveur)
  char     sid[8];
  char     name[48];
  int64_t  at;
  RegCount took, dropped;      // EVENT ; START et END : chargement déclaré dans « took »
  char     impossible[48];
  int8_t   plSeq;              // ‑1 : arrêt hors plan
  RegCount plDrop, plTake;     // ce que le plan proposait (gardé à côté du déclaré)
  uint8_t  cap;                // START : vue locale tant que le serveur n'a pas répondu
  char     truckName[40], regName[40];
};
struct RegReject { int64_t at; char what[48]; char why[120]; };

struct RegState {
  RegSnap   snap;              // dernier état reçu du serveur
  int64_t   snapAt;            // reçu à (heure de la carte), 0 = jamais
  bool      snapFromFlash;     // relu de la flash au démarrage, pas encore rafraîchi
  uint32_t  snapHash;
  RegItem   q[REG_MAX_QUEUE];  // gestes pas encore envoyés, dans l'ordre
  uint8_t   nq;
  RegItem   sent[REG_MAX_SENT];// gestes acceptés par le serveur mais pas encore dans l'état relu (snap) :
  uint8_t   nSent;             //   la vue locale les compte jusqu'à la relecture suivante
  RegReject rej[REG_MAX_REJ];  // refus du serveur, le plus récent d'abord
  uint8_t   nRej;
  uint32_t  counter;           // pour les identifiants
  char      joinTour[48];      // tournée rejointe (commencée sur un autre appareil), vide sinon
  int64_t   lastTry, lastOk;   // dernier échange tenté / réussi (epoch)
  int16_t   lastCode;          // 200, code HTTP ou échec du transport (< 0), 0 = jamais
  char      lastErr[120];
  uint32_t  version;           // change à chaque modification (l'écran redessine)
};
extern RegState g_reg;
void regInit(RegState & st);
bool regQueueSave(const RegState & st);   // file, gestes acceptés, refus, compteur (/vn_regq.bin)
bool regQueueLoad(RegState & st);
bool regSnapLoad(RegState & st);         // dernier état reçu, gardé en flash (redémarrage hors connexion)

// ── Vue locale : l'état relu du serveur + les gestes acceptés depuis + ceux qui attendent dans la file ─
struct RegTourView {
  bool    has, local;          // local : commencée ici, pas encore confirmée par le serveur
  char    id[48];
  char    truckName[40], regName[40];
  uint8_t cap;
  int64_t startedAt;
};
void     regTourNow(const RegState & st, RegTourView & v);
RegCount regLoadNow(const RegState & st);
int      regPendingEvents(const RegState & st);
void     regStopKey(uint8_t kind, const char * sid, char * out, size_t n);   // « station:47 », « atelier: »
bool     regStopDone(const RegState & st, const RegStop & s);
int      regStopsNow(const RegState & st, int * idx, int max);               // arrêts du plan pas encore faits

// ── Gestes : contrôlés comme le serveur les contrôlera ; why = la raison du refus, une phrase ─
struct RegDecl {
  uint8_t  kind;
  char     sid[8];
  char     name[48];
  RegCount took, dropped;
  const char * impossible;     // nullptr : arrêt fait
  int8_t   plSeq;              // ‑1 hors plan
  RegCount plDrop, plTake;
};
bool regDeclare(RegState & st, const RegDecl & d, int64_t now, const char * dev, char * why, size_t wn);
bool regStart(RegState & st, int truckIdx, int regIdx, RegCount load, int64_t now, const char * dev, char * why, size_t wn);
bool regEnd(RegState & st, RegCount load, int64_t now, char * why, size_t wn);
void regJoin(RegState & st, const char * tourId);
// Raisons proposées pour un arrêt impossible (les mêmes que sur le téléphone)
static const int REG_N_WHY = 6;
const char * regWhy(int i);

// ── Textes ───────────────────────────────────────────────────────────
void regActText(const RegStop & s, char * out, size_t n);       // « déposer 4 · ramasser 2 HS »
void regDeclText(uint8_t kind, RegCount took, RegCount dropped, char * out, size_t n);   // « déposé 4, pris 2 HS »
void regVerdictText(const RegEvent & e, char * out, size_t n);  // « concordant », « écart : observé +3 »…
void regStatusLine(const RegState & st, bool hasUrl, int64_t now, char * out, size_t n);

// ── Échange avec le serveur ──────────────────────────────────────────
// Transport fourni par la plateforme. Codes : 200 = succès ; sinon code HTTP ou code d'échec négatif.
// L'appareil s'annonce par « X-VN-Device » et envoie le jeton (g_sync.token) comme pour la synchro.
struct RegIo {
  void * ctx;
  int (*get)(void * ctx, const char * url, char * buf, size_t cap, size_t & len);
  int (*post)(void * ctx, const char * url, const char * body, size_t n, char * resp, size_t cap, size_t & rlen);
};
const char * regItemPath(const RegItem & it);                  // « /reg/event »…
size_t       regItemJson(const RegItem & it, char * out, size_t n);
// Envoie la file dans l'ordre (s'arrête au premier échec du réseau ou du serveur, la file est gardée),
// puis, si elle est vide, relit la tournée. buf : tampon de travail (≥ 8 Ko). Le verrou du modèle
// (plat::modelLock) protège l'état ; il n'est jamais tenu pendant une requête.
// Renvoie le nombre de gestes envoyés (acceptés ou refusés), ‑1 si le serveur n'a pas pu être joint.
int regPoll(RegIo & io, const char * base, RegState & st, int64_t now, char * buf, size_t cap);

} // namespace vn
