// ── Émetteur simulé de la commande série « dump » ────────────────────
// Joue le rôle de la carte au bout de la liaison série : lit les commandes sur l'entrée standard
// (avec le MÊME lecteur de lignes que la carte, vn::CmdLine) et répond sur la sortie standard avec
// le MÊME instantané et le MÊME émetteur (core/dump.cpp). Branché sur un pseudo-terminal, il sert
// à vérifier l'outil PC (tools/carte_dump.py) sans la carte : tools/test_carte_dump.py.
//
//   dump_sim [--fs DOSSIER | --donnees DOSSIER] [--reference DOSSIER]
//            [--couper-apres N] [--silence-apres N] [--corrompre L] [--perdre L] [--bruit]
//            [--ancien] [--reset] [--rythme µs]
//
//   --fs DOSSIER        modèle relu par Model::load depuis DOSSIER/vn_*.bin (comme au démarrage)
//   --donnees DOSSIER   sinon : parc réel de docs/donnees, 1 h de relevés fabriqués
//   --reference DOSSIER écrit les octets de l'instantané (ce que l'outil doit reproduire)
//   (les défauts ci-dessous ne touchent que le PREMIER vidage : un second essai est propre)
//   --couper-apres N    ferme la liaison après N lignes du vidage (câble arraché)
//   --silence-apres N   n'envoie plus rien après N lignes, sans fermer (carte figée)
//   --corrompre L       altère un caractère de la ligne L du vidage (parasite)
//   --perdre L          n'envoie pas la ligne L (octets perdus)
//   --bruit             journaux intercalés, dont certains collés devant une ligne VNDUMP
//   --ancien            micrologiciel sans la commande (v1.0.8) : journaux seulement
//   --reset             la carte redémarre à l'ouverture du port (lignes [boot])
#include "../src/core/dump.h"
#include "../src/core/model.h"
#include "../src/core/gbfs.h"
#include "../src/core/settings.h"
#include "../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

static std::vector<char> readAll(const std::string & p) {
  std::vector<char> v;
  FILE * f = fopen(p.c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  v.resize(n);
  if (fread(v.data(), 1, n, f) != (size_t)n) v.clear();
  fclose(f);
  return v;
}
static bool out(const char * s, size_t n) {
  while (n) { ssize_t w = write(1, s, n); if (w <= 0) return false; s += w; n -= (size_t)w; }
  return true;
}
static bool outs(const char * s) { return out(s, strlen(s)); }

int main(int argc, char ** argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  const char * fsDir = nullptr, * dataDir = "../docs/donnees", * refDir = nullptr;
  long cutAfter = -1, muteAfter = -1, corrupt = -1, lose = -1, pace = 0;
  bool noise = false, old = false, reset = false;
  for (int a = 1; a < argc; a++) {
    auto num = [&]() { return a + 1 < argc ? atol(argv[++a]) : -1; };
    if (!strcmp(argv[a], "--fs") && a + 1 < argc) fsDir = argv[++a];
    else if (!strcmp(argv[a], "--donnees") && a + 1 < argc) dataDir = argv[++a];
    else if (!strcmp(argv[a], "--reference") && a + 1 < argc) refDir = argv[++a];
    else if (!strcmp(argv[a], "--couper-apres")) cutAfter = num();
    else if (!strcmp(argv[a], "--silence-apres")) muteAfter = num();
    else if (!strcmp(argv[a], "--corrompre")) corrupt = num();
    else if (!strcmp(argv[a], "--perdre")) lose = num();
    else if (!strcmp(argv[a], "--rythme")) pace = num();
    else if (!strcmp(argv[a], "--bruit")) noise = true;
    else if (!strcmp(argv[a], "--ancien")) old = true;
    else if (!strcmp(argv[a], "--reset")) reset = true;
    else { fprintf(stderr, "option inconnue : %s\n", argv[a]); return 2; }
  }
  settingsDefaults(g_set);
  static Model m;
  if (!m.init()) { fprintf(stderr, "init modèle\n"); return 1; }
  int64_t now = 1791894641;                          // 13/10/2026 14:30:41 (instantané de docs/donnees)
  if (fsDir) {
    sim::g_fsDir = fsDir;
    m.load(0);
    if (m.n <= 0) { fprintf(stderr, "aucune station relue dans %s\n", fsDir); return 1; }
    sim::g_fsDir = "/tmp/vn_dump_sim_fs";             // plus rien n'est écrit dans le dossier source
  } else {
    sim::g_fsDir = "/tmp/vn_dump_sim_fs";
    auto info = readAll(std::string(dataDir) + "/station_information.json");
    auto stj = readAll(std::string(dataDir) + "/station_status.json");
    static InfoRec ir[VN_MAX_ST];
    static StatusRec sr[VN_MAX_ST];
    int ni = info.empty() ? -1 : gbfsParseInfo(info.data(), info.size(), ir, VN_MAX_ST);
    ElecTypes et;
    int64_t lu = 0;
    int ns = stj.empty() ? -1 : gbfsParseStatus(stj.data(), stj.size(), sr, VN_MAX_ST, et, &lu);
    if (ni <= 0 || ns <= 0) { fprintf(stderr, "jeu de données illisible : %s\n", dataDir); return 1; }
    int64_t t0 = now - 3600;
    m.applyInfo(ir, ni, t0);
    for (int k = 0; k <= 12; k++) {                  // 1 h de relevés : de petits mouvements autour du réel
      for (int i = 0; i < ns; i++) {
        int d = ((i * 7 + k * 3) % 5) - 2;
        int b = (int)sr[i].bikes + (k % 4 == 0 ? 0 : d);
        int cap = b + sr[i].docks;
        if (b < 0) b = 0;
        if (b > cap) b = cap;
        StatusRec r = sr[i];
        r.bikes = r.elec = (uint16_t)b; r.docks = (uint16_t)(cap - b); r.lastReported = t0 + k * 300;
        sr[i] = r;
      }
      m.applyStatus(sr, ns, t0 + k * 300);
    }
  }

  if (reset) {
    usleep(150000);
    outs("ESP-ROM:esp32s3-20210327\r\n[boot] flash 16 Mo \xC2\xB7 PSRAM 8192 Ko \xC2\xB7 RAM interne libre 301 Ko\r\n");
    outs("[boot] pr\xC3\xAAt en 2412 ms\r\n");
  }
  if (old) outs("[net] cycle 812 ms\r\n");

  CmdLine cmd;
  long logN = 0;
  for (;;) {
    char c;
    ssize_t r = read(0, &c, 1);
    if (r <= 0) return 0;
    const char * k = cmd.feed(c);
    if (!k) continue;
    if (old) { outs("[net] cycle 790 ms\r\n"); continue; }          // v1.0.8 : la commande n'existe pas
    if (!strcmp(k, "ping")) { char b[96]; snprintf(b, sizeof(b), "VNDUMP PRET proto=%d version=%s\n", DUMP_PROTO, VN_VERSION); outs(b); continue; }
    if (!strcmp(k, "aide")) { outs("VNDUMP AIDE commandes : dump (donnees apprises, lecture seule) | stop | ping | aide\n"); continue; }
    if (!strcmp(k, "stop")) continue;
    if (strcmp(k, "dump") != 0) { outs("VNDUMP INCONNU commande inconnue (aide)\n"); continue; }

    static DumpSnap snap;
    static DumpWriter w;
    bool ok = dumpSnapshot(m, snap);
    dumpSetup(w, snap, now, VN_VERSION);
    if (!ok) w.abort("instantane impossible (aucune station ou memoire insuffisante)");
    if (refDir) {
      mkdir(refDir, 0755);
      for (int i = 0; i < w.nf; i++) {
        std::string p = std::string(refDir) + w.f[i].name;
        FILE * f = fopen(p.c_str(), "wb");
        if (f) { fwrite(w.f[i].data, 1, w.f[i].len, f); fclose(f); }
      }
    }
    char line[DUMP_LINE_MAX + 8];
    size_t n;
    long L = 0;
    static int dumpNo = 0;
    const bool faults = ++dumpNo == 1;               // les défauts ne touchent que le premier vidage (reprise)
    bool stopped = false;
    while (w.next(line, sizeof(line), n)) {
      // « stop » reçu pendant l'émission (lu sans attendre entre deux lignes)
      struct pollfd pf = { 0, POLLIN, 0 };
      while (!stopped && poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
        if (read(0, &c, 1) <= 0) return 0;
        const char * k2 = cmd.feed(c);
        if (k2 && !strcmp(k2, "stop")) { stopped = true; w.abort("stop demande"); }
      }
      if (stopped && w.phase == DumpWriter::PH_ABORT) continue;        // la ligne prête est abandonnée
      L++;
      if (faults && cutAfter >= 0 && L > cutAfter) return 0;
      if (faults && muteAfter >= 0 && L > muteAfter) continue;
      if (faults && lose == L) continue;
      if (faults && corrupt == L && n > 30) line[n / 2] = line[n / 2] == 'A' ? 'B' : 'A';
      if (noise && L % 37 == 0) { char b[64]; snprintf(b, sizeof(b), "[net] cycle %ld ms\r\n", 700 + logN++ % 300); outs(b); }
      bool glued = noise && L % 53 == 0;
      if (glued) outs("[fs] historique sauvegard\xC3\xA9 (ok, 41 ms)");       // println en deux écritures :
      if (!out(line, n)) return 0;                                           //  le texte, une ligne VNDUMP,
      if (glued) outs("\r\n");                                               //  puis la fin de ligne du journal
      if (pace > 0) usleep((useconds_t)pace);
    }
    dumpFree(snap);
  }
}
