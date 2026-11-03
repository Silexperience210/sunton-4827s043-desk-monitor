// ── Carte simulée : la synchro du firmware contre un vrai serveur en ligne ─
// Même code que la carte (src/core/sync.cpp : manifeste, décision, contrôles, écritures, envois) ;
// seul le transport change : curl au lieu de HTTPClient. Les « fichiers en flash » sont ceux du
// dossier donné. Sert au test de bout en bout online/test/sync.e2e.mjs.
//
//   sync_sim <adresse du serveur> <dossier flash> [jeton]
// Sortie (une ligne par clé, lue par le test) : RESULT, TOOK, PUSHED, TEXT, puis ce que Model::load relit.
#include "../src/core/sync.h"
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

static std::string s_tmp;
static const char * s_token = "";

static std::string hdrs(const SyncIo * io) {
  std::string h = " -H 'X-VN-Device: sunton-sim' -H 'X-VN-Fw: sim'";
  if (io && io->localHdr[0]) h += std::string(" -H 'X-VN-Local: ") + io->localHdr + "'";
  if (*s_token) h += std::string(" -H 'Authorization: Bearer ") + s_token + "'";
  return h;
}
static int runCurl(const std::string & cmd) {
  FILE * p = popen(cmd.c_str(), "r");
  if (!p) return -1;
  char out[16] = {0};
  size_t n = fread(out, 1, sizeof(out) - 1, p);
  int st = pclose(p);
  if (st != 0 || n == 0) return -1;                    // connexion impossible (HTTPC_ERROR_CONNECTION_REFUSED)
  return atoi(out);
}
static int curlGet(void * ctx, const char * url, uint8_t * buf, size_t cap, size_t & len) {
  len = 0;
  std::string f = s_tmp + "/get.bin";
  unlink(f.c_str());
  int code = runCurl("curl -s --max-time 30 -o '" + f + "' -w '%{http_code}'" + hdrs((const SyncIo *)ctx) + " '" + url + "'");
  if (code != 200) return code;
  FILE * fp = fopen(f.c_str(), "rb");
  if (!fp) return -1;
  len = fread(buf, 1, cap, fp);
  fclose(fp);
  return 200;
}
static int curlPost(void * ctx, const char * url, const uint8_t * buf, size_t len, uint32_t crc) {
  std::string f = s_tmp + "/post.bin";
  FILE * fp = fopen(f.c_str(), "wb");
  if (!fp) return -1;
  fwrite(buf, 1, len, fp);
  fclose(fp);
  char c[16];
  snprintf(c, sizeof(c), "%08X", crc);
  return runCurl("curl -s --max-time 30 -o /dev/null -w '%{http_code}' -H 'Content-Type: application/octet-stream' -H 'X-VN-CRC32: " +
                 std::string(c) + "'" + hdrs((const SyncIo *)ctx) + " --data-binary @'" + f + "' '" + url + "'");
}

int main(int argc, char ** argv) {
  if (argc < 3) { fprintf(stderr, "usage : sync_sim <adresse> <dossier flash> [jeton]\n"); return 2; }
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  sim::g_fsDir = argv[2];
  sim::g_epoch0 = (int64_t)time(nullptr);
  if (argc > 3) s_token = argv[3];
  s_tmp = std::string(argv[2]) + "/.curl";
  std::string mk = "mkdir -p '" + s_tmp + "'";
  if (system(mk.c_str()) != 0) return 2;
  settingsDefaults(g_set);
  syncCfgDefaults(g_sync);
  if (!syncNormalizeUrl(argv[1], g_sync.url, sizeof(g_sync.url))) { fprintf(stderr, "adresse refusée\n"); return 2; }
  SyncIo io = {};
  io.ctx = &io;
  io.get = curlGet;
  io.post = curlPost;
  SyncStatus st = {};
  int r = syncRun(io, g_sync.url, plat::now(), true, st);
  printf("RESULT %d\nSTATE %d\nTOOK %u\nPUSHED %u\nTEXT %s\n", r, st.state, st.took, st.pushed, st.text);
  // Ce que la carte relirait au démarrage
  static Model m;
  if (!m.init()) return 1;
  bool any = m.load(plat::now());
  printf("LOADED %d\nSAMPLES %u\nRING %d\nDAYS %d\nSTATIONS %d\n", any ? 1 : 0, m.learnSamples, m.count, m.days.count, m.n);
  return 0;
}
