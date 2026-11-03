// ── Synchronisation avec le serveur en ligne (online/server.mjs) ─────
// Le serveur relève les mêmes flux, avec le même cœur, 24 h/24. Une carte éteinte (le week-end, une
// coupure) reprend au démarrage ce qu'il a de MEILLEUR que ce qu'elle a, fichier par fichier :
//   /vn_prof.bin  profils appris        → adopté s'il compte strictement plus de relevés (learnSamples) ;
//   /vn_hist.bin  historique 24 h       → adopté s'il est plus récent d'au moins une minute et encore
//                                         relisible (sauvegardé il y a moins de 20 h, règle de Model::load) ;
//   /vn_days.bin  résumés par jour      → adopté s'il compte plus de jours, ou autant mais plus récents.
// Et, si l'option est active, elle lui ENVOIE ce qu'elle a appris de plus (profils, jours) : la première
// carte peut ainsi amorcer un serveur neuf, sans câble ni vidage série.
//
// Jamais un fichier invalide n'écrase celui de la flash : taille et CRC32 annoncés par le manifeste, puis
// les contrôles d'en-tête de Model::load (profFileOk, histFileOk, DayBook::check), AVANT l'écriture —
// elle-même atomique (plat::fsWrite écrit un fichier temporaire puis le renomme).
// Portable (aucun Arduino) : la décision, les contrôles et le déroulé sont testés par sim/test_core.cpp ;
// le transport HTTP est fourni par la plateforme (net_esp32.cpp, ou curl dans sim/sync_sim.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vn {

// ── Réglage (fichier texte /vn_sync.cfg, séparé de /settings.bin dont la taille est figée) ─
struct SyncCfg {
  char url[128];               // adresse du serveur, « http://umbrel.local:8790 » ; vide = aucune synchro
  char token[48];              // jeton du serveur (VN_TOKEN), envoyé en « Authorization: Bearer » ; vide = aucun
  bool atBoot;                 // synchroniser au démarrage (avant de relire l'historique)
  bool push;                   // envoyer au serveur ce que la carte a appris de plus que lui
};
extern SyncCfg g_sync;
static const char * const SYNC_CFG_PATH = "/vn_sync.cfg";
void syncCfgDefaults(SyncCfg & c);      // VN_SYNC_URL / VN_SYNC_TOKEN de secrets.h si définis, démarrage et envoi actifs
bool syncCfgLoad();
bool syncCfgSave();
// Adresse saisie → base normalisée (espaces retirés, « http:// » ajouté s'il manque, « / » final et
// « /sync/manifest » retirés). false si l'adresse est vide ou n'est pas http(s).
bool syncNormalizeUrl(const char * in, char * out, size_t n);

// ── Fichiers ─────────────────────────────────────────────────────────
enum SyncKind : uint8_t { SK_PROF, SK_HIST, SK_DAYS, SK_COUNT };
const char * syncFileName(int kind);    // « vn_prof.bin »…
const char * syncFilePath(int kind);    // « /vn_prof.bin »…
const char * syncKindLabel(int kind);   // « profils », « historique 24 h », « résumés par jour »

// Ce que la carte a en flash (en-têtes seulement : 40 octets par fichier)
struct SyncLocal {
  bool     has[SK_COUNT];
  uint32_t profSamples;        // FileHdr.learnSamples de /vn_prof.bin
  int64_t  profSince;
  int64_t  histSaved;          // FileHdr.savedAt de /vn_hist.bin
  uint16_t daysN;              // DaysFileHdr.nDays de /vn_days.bin
  int64_t  daysSaved;
};
void syncScanLocal(SyncLocal & out);
// Même lecture, sur un fichier en mémoire (en-tête valide requis)
bool syncHeaderOf(int kind, const uint8_t * b, size_t n, SyncLocal & out);
// En-tête « X-VN-Local » envoyé au serveur : « prof=12345;hist=1791894641;days=12 »
void syncLocalHeader(const SyncLocal & l, char * out, size_t n);

// ── Manifeste du serveur (GET <url>/sync/manifest) ───────────────────
struct SyncRemote { bool present; uint32_t size, crc; uint16_t ver; };
struct SyncManifest {
  int      proto;
  char     id[24];             // instantané : les fichiers se lisent sous <base>
  char     base[176];          // « http://…/sync/<id>/ »
  char     name[48], fw[16];
  int64_t  t;
  SyncRemote f[SK_COUNT];
  uint32_t profSamples;
  int64_t  profSince, histSaved, daysSaved;
  uint16_t daysN;
};
bool syncParseManifest(const char * json, size_t len, SyncManifest & m);

// ── Décision ─────────────────────────────────────────────────────────
struct SyncPlan {
  bool take[SK_COUNT];         // à télécharger
  bool push[SK_COUNT];         // à envoyer
  char why[SK_COUNT][96];      // raison, une phrase par fichier
};
void syncDecide(const SyncLocal & l, const SyncManifest & m, int64_t now, bool allowPush, SyncPlan & p);
// Fichier reçu : taille et CRC32 du manifeste, puis l'en-tête que Model::load exigera.
bool syncBlobOk(int kind, const uint8_t * b, size_t n, const SyncRemote & f, int64_t now, const char ** why);

// ── Déroulé complet ──────────────────────────────────────────────────
// Transport fourni par la plateforme. Codes : 200 = succès ; sinon code HTTP ou code d'échec négatif.
struct SyncIo {
  void * ctx;                  // libre pour le transport (en général : l'adresse de ce SyncIo)
  // GET url → buf (au plus cap octets) ; len = octets reçus
  int (*get)(void * ctx, const char * url, uint8_t * buf, size_t cap, size_t & len);
  // POST url ← buf (application/octet-stream, en-tête X-VN-CRC32)
  int (*post)(void * ctx, const char * url, const uint8_t * buf, size_t len, uint32_t crc);
  char localHdr[96];           // « X-VN-Local », rempli par syncRun avant le premier appel : le transport
                               // l'envoie avec « X-VN-Device », « X-VN-Fw » et le jeton (g_sync.token)
};
enum SyncState : int8_t { SS_NEVER, SS_RUNNING, SS_TOOK, SS_NOTHING, SS_FAIL };
struct SyncStatus {
  int64_t at;                  // fin de la dernière synchro (epoch), 0 = jamais
  int8_t  state;               // SyncState
  uint8_t took, pushed;        // bits (1 << SyncKind) : fichiers adoptés / envoyés
  int16_t code;                // dernier code d'échec (HTTP ou transport), 0 = aucun
  char    text[120];           // une phrase, la même au port série et à l'écran
};
// Manifeste, décision, téléchargements vérifiés, écritures atomiques, envois, compte rendu au serveur.
// Renvoie le nombre de fichiers adoptés (écrits en flash) ; ‑1 si le serveur n'a pas pu être lu.
// La carte doit relire ses fichiers (Model::load) après une adoption : au démarrage, c'est le chargement
// qui suit ; en cours de route, un redémarrage.
int  syncRun(SyncIo & io, const char * baseUrl, int64_t now, bool allowPush, SyncStatus & st);
void syncStatusLine(const SyncStatus & st, int64_t now, char * out, size_t n);   // « 26/10 07:58 · … »

} // namespace vn
