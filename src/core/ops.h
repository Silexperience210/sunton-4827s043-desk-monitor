// ── Exploitation : ingestion des flux, fraîcheur, provenance des chiffres ─
// Tout ce que l'écran affiche pour justifier un chiffre passe par ici :
//  - FeedStat : compteurs réels de la tâche réseau (un par flux) ;
//  - fraîcheur : âge du dernier relevé réussi et seuil du bandeau ;
//  - provenance : champ de flux ou formule derrière chaque tuile du tableau de bord.
// Portable (aucun Arduino) : testé tel quel par sim/test_core.cpp.
#pragma once
#include <stdint.h>
#include "model.h"
#include "settings.h"

namespace vn {

// ── Ingestion ────────────────────────────────────────────────────────
enum FeedId : uint8_t { FEED_STATUS, FEED_INFO, FEED_TYPES, FEED_WEATHER, FEED_COUNT };
const char * feedName(int feed);             // « station_status »… « météo »

// Codes d'échec : > 0 = statut HTTP ; ‑1…‑11 = HTTPClient (HTTPC_ERROR_*) ;
// ≤ ‑100 = codes propres à VelohNav Ops ci-dessous.
enum : int16_t {
  ING_E_PARSE = -100,          // réponse reçue mais JSON illisible / champ manquant
  ING_E_EMPTY = -101,          // JSON valide mais aucun enregistrement exploitable
  ING_E_TRUNC = -102,          // réponse plus courte que Content-Length
  ING_E_BEGIN = -103,          // HTTPClient::begin() a refusé l'URL
};
const char * ingestErrText(int code);        // libellé court en français

struct FeedStat {
  int64_t  okAt;               // dernier succès (epoch), 0 = jamais
  int64_t  errAt;              // dernier échec (epoch), 0 = jamais
  int16_t  errCode;            // code du dernier échec (conservé après un succès)
  uint16_t fails;              // échecs consécutifs, remis à 0 au premier succès
  uint32_t bytes;              // octets reçus au dernier relevé (succès ou échec)
  uint32_t ms;                 // durée du dernier relevé : HTTP (tentatives comprises) + analyse
  uint32_t okCount, errCount;  // totaux depuis le démarrage
};
void ingestOk(FeedStat & f, int64_t now, uint32_t bytes, uint32_t ms);
void ingestErr(FeedStat & f, int64_t now, int code, uint32_t bytes, uint32_t ms);

// ── Fraîcheur ────────────────────────────────────────────────────────
// Seuil du bandeau « données vieilles » (secondes) :
//  - station_status : g_set.staleMin minutes si réglé, sinon 3 × refreshSec, au moins 3 min ;
//  - météo : 3 × sa propre période (VN_WX_REFRESH_S), même règle « 3 périodes manquées ».
int  staleThresholdS(const Settings & s);
int  wxStaleThresholdS();
// Âge (s) d'un relevé réussi ; ‑1 si inconnu (jamais reçu, ou heure pas encore synchronisée)
int64_t dataAgeS(int64_t okAt, int64_t now);
// Vieux = âge STRICTEMENT supérieur au seuil ; un âge inconnu n'est pas « vieux »
// (c'est un autre état : rien reçu, l'écran d'accueil ou le bandeau hors ligne le disent).
bool isStale(int64_t okAt, int64_t now, int thresholdS);

// ── État des chiffres affichés ───────────────────────────────────────
// Ce que le tableau de bord a le droit d'afficher. Un modèle sans relevé vaut 0
// partout : sans cette règle, « 0 vélo » se lisait comme une mesure.
enum DataState : uint8_t {
  DS_LIVE,                     // relevé frais, Wi-Fi associé
  DS_STALE,                    // Wi-Fi associé, relevé plus vieux que le seuil : dernières valeurs, datées
  DS_OFFLINE,                  // Wi-Fi perdu : dernières valeurs, « hors ligne depuis HH:MM »
  DS_NONE                      // aucun relevé depuis le démarrage : « — » partout, jamais 0
};
DataState dataState(int64_t lastUpdate, int64_t now, int thresholdS, bool wifiUp);

// ── Association Wi-Fi ────────────────────────────────────────────────
// reason : code wifi_err_reason_t d'ESP-IDF rapporté par l'évènement « déconnecté »
// (0 = aucun évènement : la borne n'a jamais répondu).
enum AssocCause : uint8_t { AC_NOT_FOUND, AC_AUTH, AC_TIMEOUT, AC_LOST, AC_REFUSED };
AssocCause assocCause(int reason);
const char * assocCauseText(int cause);      // « réseau introuvable », « mot de passe refusé »…

// Pourquoi aucun relevé frais n'arrive : une phrase, la même au port série et à l'écran.
// wifi = net::WifiState (0 aucun réseau, 1 connexion, 2 associé, 3 échec) ; reason comme ci-dessus ;
// st = compteurs de station_status.
void offlineCause(char * out, size_t n, int wifi, const char * ssid, int reason, bool timeOk, const FeedStat & st);

// ── Provenance d'un chiffre ──────────────────────────────────────────
enum KpiId : uint8_t {
  KPI_BIKES, KPI_DELTA1H,      // tuile « Vélos dispo »
  KPI_INUSE, KPI_FLEET,        // tuile « En trajet »
  KPI_DOCKS, KPI_FILL,         // tuile « Bornes lib. »
  KPI_BIKES_DIS, KPI_DOCKS_DIS,// tuile « Vélos HS »
  KPI_TEMP, KPI_WXSCORE, KPI_FC60,   // bandeau météo + prévision +1 h
  KPI_COUNT
};
enum SrcKind : uint8_t { SRC_FEED, SRC_CALC };

struct Provenance {
  const char * name;           // libellé du chiffre
  bool    available;           // false → l'écran affiche « — » et `missing`
  bool    estimate;            // estimation (et non mesure ni somme de mesures)
  bool    isFloat;
  float   value;               // valeur affichée (entière si !isFloat)
  const char * unit;           // suffixe (« % », « °C »…), "" sinon
  uint8_t kind;                // SrcKind
  const char * source;         // « GBFS », « Open-Meteo », « calcul interne »
  const char * feed;           // flux d'origine (« station_status »…), nullptr si aucun
  const char * field;          // champ exact (« num_vehicles_available »…), nullptr si aucun
  const char * rule;           // règle appliquée, une phrase
  const char * validity;       // condition de validité (estimations), nullptr sinon
  bool    validityMet;
  const char * missing;        // raison de l'absence (si !available)
  int64_t feedAt;              // horodatage du flux (last_updated GBFS / fetchedAt météo)
  int64_t recvAt;              // réception par la carte (0 si inconnue)
  int64_t refAt;               // instant de référence d'une estimation (creux du parc…), 0 sinon
  int     count;               // stations sommées, ‑1 si sans objet
  char    detail[112];         // état chiffré de la condition / précision (peut être vide)
};
bool provenance(const Model & m, int kpi, Provenance & out);

// ── Part météo d'une prévision ───────────────────────────────────────
// Origine du coefficient (une phrase, pour l'écran) : météo indisponible / périmée, défaut (trop
// peu de pluie observée), sans effet mesurable, ou appris (α ± erreur-type).
void wxOriginText(const Model & m, char * out, size_t n);
// Phrase de la fiche : part météo `part` (vélos) de la prévision à l'instant `at` ; `forecast` :
// part issue de la météo ANNONCÉE (au-delà de 2 h) plutôt qu'actuelle. Sans part notable
// (< 0,5 vélo), dit pourquoi (temps normal, coefficient nul, météo indisponible…).
void wxPartText(const Model & m, float part, bool forecast, int64_t at, char * out, size_t n);

// ── Écart au profil (« inhabituel ») ─────────────────────────────────
// Ligne de la fiche station : état, attendu, tolérance (formule fixe), confiance du profil et, pour
// information, l'écart habituel mesuré pour ce créneau et cette station (« ± 2,1, mesuré sur 34 jours »).
void anomLineText(const Station & s, char * out, size_t n);
// Ligne courte de la liste d'alertes (AL_UNUSUAL)
void anomAlertText(const Station & s, char * out, size_t n);

} // namespace vn
