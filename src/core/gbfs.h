// ── Vel'OH! via GBFS v3 (cyclocity, public, sans clé) ────────────────
// Portage C++ de src/utils/gbfs.js (VelohNav) : mêmes flux, même
// sémantique « utilisable » (is_renting / is_returning), mêmes exclusions
// (stations fantômes, « BORNE ATELIER »). L'outil ops conserve en plus
// les vélos et bornes hors service (num_*_disabled) que l'app ignorait.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vn {

struct InfoRec {
  char     sid[8];
  char     name[48];      // nettoyé (« #00001-LEON XIII » → « Leon XIII »)
  char     addr[64];
  float    lat, lon;
  uint16_t cap;
};

struct StatusRec {
  char     sid[8];
  uint16_t bikes, elec, docks, bikesDis, docksDis;
  uint8_t  installed, renting, returning;
  int64_t  lastReported;
};

struct ElecTypes {           // ids des types de véhicule électriques
  int  n = 0;
  char id[6][24];
  bool known = false;        // flux vehicle_types lu
  bool isElec(const char * typeId) const;
};

// Parseurs purs : renvoient le nombre d'enregistrements (‑1 si JSON invalide)
int  gbfsParseInfo(const char * buf, size_t len, InfoRec * out, int max);
int  gbfsParseStatus(const char * buf, size_t len, StatusRec * out, int max,
                     const ElecTypes & et, int64_t * lastUpdated);
bool gbfsParseTypes(const char * buf, size_t len, ElecTypes & et);

// Utilitaires
int64_t parseIso8601(const char * s);                 // → epoch UTC, 0 si invalide / < 2000
void    cleanStationName(const char * in, char * out, size_t n);
bool    isNonPublic(const char * cleanedName);         // dépôt / atelier opérateur

} // namespace vn
