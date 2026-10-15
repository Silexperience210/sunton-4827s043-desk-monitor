// ── Vidage série des données apprises (commande « dump ») ────────────
// Recopie, en LECTURE SEULE, ce que la carte a appris — l'historique 5 min (/vn_hist.bin),
// les profils (/vn_prof.bin), les résumés par jour (/vn_days.bin) et la liste des stations
// (capacités) — sur la liaison série, en base64, par blocs, pour que le banc de prévision
// (sim/bench_prevision --reel) les rejoue sur le PC. Outil PC : tools/carte_dump.py.
//
// Ce qui est envoyé est un INSTANTANÉ du modèle en mémoire, pris sous le verrou par
// Model::serialize — exactement les octets que la prochaine sauvegarde écrirait. La flash
// n'est ni lue (un fichier en cours de réécriture par la tâche réseau serait déchiré) ni écrite.
//
// Protocole (lignes ASCII terminées par '\n', toutes préfixées « VNDUMP ») :
//   VNDUMP PRET proto=1 version=<VN_VERSION>                        (réponse à « ping »)
//   VNDUMP DEBUT proto=1 version=<VN_VERSION> t=<epoch> fichiers=<n>
//   VNDUMP FICHIER <i> <nom> taille=<octets> blocs=<k> crc=<crc32 hex du fichier>
//   VNDUMP B <i> <j> <base64 de ≤ 96 octets> <crc32 hex du bloc>    (j = 0 … k − 1)
//   VNDUMP FIN <i> <nom> blocs=<k> crc=<crc32 hex du fichier>
//   VNDUMP TERMINE fichiers=<n>
//   VNDUMP ABANDON <raison>      (commande « stop », ou instantané impossible)
// Un journal (plat::logf) peut s'intercaler entre deux lignes, ou précéder une ligne VNDUMP sur
// la même ligne (un println écrit en deux fois) : l'outil PC cherche « VNDUMP » dans la ligne.
// Portable (aucun Arduino) : testé tel quel par sim/test_core.cpp et sim/dump_sim.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "model.h"

namespace vn {

uint32_t crc32(const void * data, size_t n, uint32_t crc = 0);     // IEEE 802.3 (= zlib.crc32)
size_t   base64Encode(const uint8_t * in, size_t n, char * out);   // écrit 4·⌈n/3⌉ caractères + NUL

static const int    DUMP_PROTO = 1;
static const size_t DUMP_BLOCK = 96;          // octets par ligne (128 caractères base64)
static const size_t DUMP_LINE_MAX = 200;      // longueur maximale d'une ligne, '\n' compris
static const int    DUMP_MAX_FILES = 4;

struct DumpFile { const char * name; const uint8_t * data; size_t len; uint32_t crc; };

// Émetteur « à la demande » : next() rend la ligne suivante. La tâche série en écrit autant
// que la liaison en accepte ; rien n'est préparé d'avance (aucune mémoire hors l'instantané).
struct DumpWriter {
  DumpFile f[DUMP_MAX_FILES];
  int      nf = 0;
  int64_t  at = 0;                             // instant de l'instantané (epoch)
  const char * fw = "";                        // version du micrologiciel
  bool add(const char * name, const uint8_t * data, size_t len);   // false si plein
  // Ligne suivante dans buf (NUL-terminée, '\n' compris) ; false quand tout a été émis.
  bool next(char * buf, size_t cap, size_t & len);
  void abort(const char * why);                // la ligne suivante sera « ABANDON », puis plus rien
  bool done() const { return phase == PH_DONE; }
  size_t blocks(int i) const { return (f[i].len + DUMP_BLOCK - 1) / DUMP_BLOCK; }

  enum Phase : uint8_t { PH_BEGIN, PH_FILE, PH_BLOCK, PH_END, PH_TERM, PH_ABORT, PH_DONE };
  Phase  phase = PH_BEGIN;
  int    cur = 0;
  size_t blk = 0;
  char   why[48] = {};
};

// Instantané pour le vidage : les trois fichiers (serialize) + la liste des stations.
// À appeler SOUS le verrou du modèle ; l'émission, longue (~1 min à 115 200 bauds), se fait hors verrou.
struct DumpSnap {
  Model::Blob blob;
  uint8_t * csv = nullptr; size_t csvLen = 0;
};
bool dumpSnapshot(const Model & m, DumpSnap & out);
void dumpFree(DumpSnap & s);
void dumpSetup(DumpWriter & w, const DumpSnap & s, int64_t now, const char * fw);

// Liste des stations (« /vn_stations.csv », produite pour le vidage, jamais écrite en flash) :
//   # vn_stations v1 t=<epoch>
//   sid;capacite;publiee;lat;lon;nom
// Le banc en tire les capacités (l'historique 5 min ne garde que les vélos).
size_t stationsCsv(const Model & m, int64_t now, char * out, size_t cap);

// Commandes série, une par ligne (« dump », « stop », « ping », « aide ») : feed() rend la
// commande complète à la fin de ligne, nullptr sinon. Une ligne trop longue est ignorée en entier.
struct CmdLine {
  char buf[24];
  int  n = 0;
  bool over = false;
  const char * feed(char c);
};

} // namespace vn
