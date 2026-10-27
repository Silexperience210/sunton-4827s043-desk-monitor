// ── Sunton du camion simulé : le rôle « régulateur » du firmware contre un vrai serveur ─
// Même code que la carte (src/core/regul.cpp : vue locale, contrôles, file hors connexion, échange) ;
// seul le transport change : curl au lieu de HTTPClient. La « flash » est le dossier donné : la file,
// l'état relu et le réglage y restent d'un appel à l'autre, comme après un redémarrage de la carte.
// Sert au test de bout en bout online/test/reg.e2e.mjs.
//
//   reg_sim <adresse du serveur> <dossier flash> <commande> [arguments]
//     poll                                   échange seul (file, puis relecture de la tournée)
//     start <camion> <régulateur> <ok> <hs>  début de tournée (index dans les listes du serveur)
//     next                                   prochain arrêt du plan, fait comme proposé
//     station <sid> <déposés> <pris> <HS pris>
//     atelier <HS déchargés> <déposés> <repris>
//     impossible <sid> <raison 0…5>
//     end <ok> <hs>                          fin de tournée, chargement restant déclaré
//     join <tournée>                         rejoindre une tournée commencée ailleurs
//     show                                   état seul, sans réseau
//   REG_OFFLINE=1 : aucun échange après la commande (partage de connexion coupé) ; VN_TOKEN : jeton ;
//   REG_DEV : identifiant de l'appareil (un second Sunton).
// Sortie : une ligne par clé (OK, WHY, POLL, TOUR, LOAD, QUEUE, REJ, STOP, EVENT, STATUS), lue par le test.
#include "../src/core/regul.h"
#include "../src/core/sync.h"
#include "../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

static std::string s_tmp, s_token;
static const char * DEV = "sunton-camion-sim";      // REG_DEV : un second Sunton

static std::string hdrs() {
  std::string h = std::string(" -H 'X-VN-Device: ") + DEV + "' -H 'X-VN-Fw: sim' -H 'Accept: application/json'";
  if (!s_token.empty()) h += " -H 'Authorization: Bearer " + s_token + "'";
  return h;
}
static int runCurl(const std::string & cmd) {
  FILE * p = popen(cmd.c_str(), "r");
  if (!p) return -1;
  char out[16] = {0};
  size_t n = fread(out, 1, sizeof(out) - 1, p);
  int st = pclose(p);
  if (st != 0 || n == 0) return -1;                    // connexion impossible
  int code = atoi(out);
  return code ? code : -1;
}
static size_t slurp(const std::string & f, char * buf, size_t cap) {
  FILE * fp = fopen(f.c_str(), "rb");
  if (!fp) return 0;
  size_t n = fread(buf, 1, cap, fp);
  fclose(fp);
  return n;
}
static int curlGet(void *, const char * url, char * buf, size_t cap, size_t & len) {
  len = 0;
  std::string f = s_tmp + "/get.json";
  unlink(f.c_str());
  int code = runCurl("curl -s --max-time 20 -o '" + f + "' -w '%{http_code}'" + hdrs() + " '" + url + "'");
  if (code != 200) return code;
  len = slurp(f, buf, cap);
  return 200;
}
static int curlPost(void *, const char * url, const char * body, size_t n, char * resp, size_t cap, size_t & rlen) {
  rlen = 0;
  std::string fb = s_tmp + "/post.json", fr = s_tmp + "/resp.json";
  FILE * fp = fopen(fb.c_str(), "wb");
  if (!fp) return -1;
  fwrite(body, 1, n, fp);
  fclose(fp);
  unlink(fr.c_str());
  int code = runCurl("curl -s --max-time 20 -o '" + fr + "' -w '%{http_code}' -H 'Content-Type: application/json'" + hdrs() +
                     " --data-binary @'" + fb + "' '" + url + "'");
  if (code > 0) rlen = slurp(fr, resp, cap);
  return code;
}

static void show(const RegState & st, bool hasUrl) {
  RegTourView v;
  regTourNow(st, v);
  if (v.has) printf("TOUR %s %d %d %s %s\n", v.id, v.local ? 1 : 0, v.cap, v.truckName, v.regName);
  else printf("TOUR -\n");
  RegCount l = regLoadNow(st);
  printf("LOAD %d %d\n", l.ok, l.hs);
  printf("QUEUE %d SENT %d PENDING %d\n", st.nq, st.nSent, regPendingEvents(st));
  printf("LISTS %d %d\n", st.snap.nTrucks, st.snap.nRegs);
  printf("SNAP %d %lld %d\n", st.snap.valid ? 1 : 0, (long long)st.snapAt, st.snapFromFlash ? 1 : 0);
  for (int i = 0; i < st.nRej; i++) printf("REJ %s \xE2\x80\x94 %s\n", st.rej[i].what, st.rej[i].why);
  int idx[REG_MAX_STOPS];
  int n = regStopsNow(st, idx, REG_MAX_STOPS);
  printf("STOPS %d\n", n);
  char a[96];
  for (int k = 0; k < n; k++) {
    const RegStop & s = st.snap.stops[idx[k]];
    regActText(s, a, sizeof(a));
    printf("STOP %d %s %s %s | %s | %s\n", s.seq, s.kind == RK_ATELIER ? "atelier" : "station", s.sid[0] ? s.sid : "-", s.name, a, s.why);
  }
  for (int i = 0; i < st.snap.nEvents; i++) {
    regVerdictText(st.snap.events[i], a, sizeof(a));
    printf("EVENT %s %s | %s\n", st.snap.events[i].id, st.snap.events[i].name, a);
  }
  for (int i = 0; i < st.snap.nOpen; i++) printf("OPEN %s %s %s %s\n", st.snap.open[i].id, st.snap.open[i].device, st.snap.open[i].truckName, st.snap.open[i].regName);
  char line[200];
  regStatusLine(st, hasUrl, plat::now(), line, sizeof(line));
  printf("STATUS %s\n", line);
  if (st.lastErr[0]) printf("LASTERR %s\n", st.lastErr);
}

int main(int argc, char ** argv) {
  if (argc < 4) { fprintf(stderr, "usage : reg_sim <adresse> <dossier flash> <commande> [arguments]\n"); return 2; }
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  sim::g_fsDir = argv[2];
  sim::g_epoch0 = getenv("REG_NOW") ? strtoll(getenv("REG_NOW"), nullptr, 10) : (int64_t)time(nullptr);
  if (getenv("VN_TOKEN")) s_token = getenv("VN_TOKEN");
  if (getenv("REG_DEV")) DEV = getenv("REG_DEV");
  s_tmp = std::string(argv[2]) + "/.curl";
  std::string mk = "mkdir -p '" + s_tmp + "'";
  if (system(mk.c_str()) != 0) return 2;
  syncCfgDefaults(g_sync);
  const bool hasUrl = syncNormalizeUrl(argv[1], g_sync.url, sizeof(g_sync.url));
  regCfgLoad();
  regInit(g_reg);
  regSnapLoad(g_reg);
  regQueueLoad(g_reg);
  const char * cmd = argv[3];
  const int64_t now = plat::now();
  char why[200] = "";
  bool ok = true;
  auto A = [&](int i) { return argc > i ? atoi(argv[i]) : 0; };
  if (!strcmp(cmd, "start")) {
    ok = regStart(g_reg, A(4), A(5), RegCount{ (int16_t)A(6), (int16_t)A(7) }, now, DEV, why, sizeof(why));
  } else if (!strcmp(cmd, "next")) {
    int idx[REG_MAX_STOPS];
    int n = regStopsNow(g_reg, idx, REG_MAX_STOPS);
    if (!n) { ok = false; snprintf(why, sizeof(why), "aucun arr\xC3\xAAt propos\xC3\xA9"); }
    else {
      const RegStop & s = g_reg.snap.stops[idx[0]];
      RegDecl d;
      memset(&d, 0, sizeof(d));
      d.kind = s.kind;
      snprintf(d.sid, sizeof(d.sid), "%s", s.sid);
      snprintf(d.name, sizeof(d.name), "%s", s.name);
      d.dropped = s.drop; d.took = s.take; d.plSeq = (int8_t)s.seq; d.plDrop = s.drop; d.plTake = s.take;
      ok = regDeclare(g_reg, d, now, DEV, why, sizeof(why));
      printf("NEXT %s %s\n", s.kind == RK_ATELIER ? "atelier" : s.sid, s.name);
    }
  } else if (!strcmp(cmd, "station") || !strcmp(cmd, "impossible")) {
    RegDecl d;
    memset(&d, 0, sizeof(d));
    d.kind = RK_STATION;
    d.plSeq = -1;
    snprintf(d.sid, sizeof(d.sid), "%s", argc > 4 ? argv[4] : "");
    if (!strcmp(cmd, "station")) { d.dropped = RegCount{ (int16_t)A(5), 0 }; d.took = RegCount{ (int16_t)A(6), (int16_t)A(7) }; }
    else d.impossible = regWhy(A(5));
    ok = regDeclare(g_reg, d, now, DEV, why, sizeof(why));
  } else if (!strcmp(cmd, "atelier")) {
    RegDecl d;
    memset(&d, 0, sizeof(d));
    d.kind = RK_ATELIER;
    d.plSeq = -1;
    d.dropped = RegCount{ (int16_t)A(5), (int16_t)A(4) };
    d.took = RegCount{ (int16_t)A(6), 0 };
    ok = regDeclare(g_reg, d, now, DEV, why, sizeof(why));
  } else if (!strcmp(cmd, "end")) {
    ok = regEnd(g_reg, RegCount{ (int16_t)A(4), (int16_t)A(5) }, now, why, sizeof(why));
  } else if (!strcmp(cmd, "join")) {
    regJoin(g_reg, argc > 4 ? argv[4] : "");
  } else if (strcmp(cmd, "poll") && strcmp(cmd, "show")) {
    fprintf(stderr, "commande inconnue : %s\n", cmd);
    return 2;
  }
  printf("OK %d\n", ok ? 1 : 0);
  if (!ok) printf("WHY %s\n", why);
  if (strcmp(cmd, "show") && !getenv("REG_OFFLINE")) {
    static char buf[64 * 1024];
    RegIo io = { nullptr, curlGet, curlPost };
    int r = regPoll(io, g_sync.url, g_reg, now, buf, sizeof(buf));
    printf("POLL %d\n", r);
  }
  show(g_reg, hasUrl);
  return 0;
}
