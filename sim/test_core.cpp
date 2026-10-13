// ── Tests du cœur (sans UI) : parsing, noms, prédiction, persistance ──
// make test  →  ./test_core <dossier_json>
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/gbfs.h"
#include "../src/core/weather.h"
#include "../src/platform/platform.h"
#include "../src/hal/touch_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

static int s_fail = 0, s_ok = 0;
#define CHECK(c, ...) do { if (c) s_ok++; else { s_fail++; printf("ÉCHEC %s:%d : ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<char> readFile(const std::string & p) {
  std::vector<char> v;
  FILE * f = fopen(p.c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  v.resize(n);
  if (fread(v.data(), 1, n, f) != (size_t)n) v.clear();
  fclose(f);
  return v;
}

static void testNames() {
  struct { const char * in, * out; } T[] = {
    { "#00001-LEON XIII", "Leon XIII" },
    { "#00053-CLOCHE D'OR", "Cloche d'Or" },
    { "#00075-JEANNE D'ARC", "Jeanne d'Arc" },
    { "#00027-CHARLY'S GARE", "Charly's Gare" },
    { "#00051-BEI DER AUER", "Bei der Auer" },
    { "#00047-BGL BNP PARIBAS", "BGL BNP Paribas" },
    { "#00086-PWC - BD KOCKELSCHEUER", "PWC - Bd Kockelscheuer" },
    { "#00033-K. ADENAUER", "K. Adenauer" },
    { "#00090-HUBERTY (MÜHLENBACH)", "Huberty (Mühlenbach)" },
    { "#00104-ITZIG VILLAGE/PLACE MANDERSCHEID", "Itzig Village/Place Manderscheid" },
    { "#00145-PÔLE D'ÉCHANGE HOWALD", "Pôle d'Échange Howald" },
    { "#00110-ROND-POINT GAASSCHTBIERG", "Rond-Point Gaasschtbierg" },
    { "00012 - HAMILIUS", "Hamilius" },
    { "#00042-LYCEE TECHNIQUE DE BONNEVOIE", "Lycee Technique de Bonnevoie" },
  };
  for (auto & t : T) {
    char o[64];
    cleanStationName(t.in, o, sizeof(o));
    CHECK(strcmp(o, t.out) == 0, "nom « %s » → « %s » (attendu « %s »)", t.in, o, t.out);
  }
  CHECK(isNonPublic("Borne Atelier Lux"), "atelier");
  CHECK(!isNonPublic("Atelierstrooss"), "faux positif atelier");
}

static void testIso() {
  CHECK(parseIso8601("2026-10-13T12:30:41.839Z") == 1791894641, "iso ms");
  CHECK(parseIso8601("2026-10-13T14:30:41+02:00") == 1791894641, "iso offset");
  CHECK(parseIso8601("0001-01-01T00:00:00Z") == 0, "iso an 1");
  CHECK(parseIso8601("n'importe quoi") == 0, "iso invalide");
}

static void testParse(const std::string & dir) {
  auto info = readFile(dir + "/station_information.json");
  auto st = readFile(dir + "/station_status.json");
  auto ty = readFile(dir + "/vehicle_types.json");
  auto wx = readFile(dir + "/weather.json");
  if (info.empty() || st.empty()) { printf("(jeu de données absent : tests GBFS ignorés)\n"); return; }
  static InfoRec ir[VN_MAX_ST];
  static StatusRec sr[VN_MAX_ST];
  ElecTypes et;
  CHECK(gbfsParseTypes(ty.data(), ty.size(), et) && et.n == 1 && strcmp(et.id[0], "electrical") == 0, "vehicle_types");
  int ni = gbfsParseInfo(info.data(), info.size(), ir, VN_MAX_ST);
  int64_t lu = 0;
  int ns = gbfsParseStatus(st.data(), st.size(), sr, VN_MAX_ST, et, &lu);
  CHECK(ni > 120 && ni < 200, "stations publiques : %d", ni);
  CHECK(ns >= ni, "statuts %d ≥ infos %d", ns, ni);
  CHECK(lu > 1700000000, "last_updated");
  int bikes = 0, elec = 0;
  for (int i = 0; i < ns; i++) { bikes += sr[i].bikes; elec += sr[i].elec; CHECK(sr[i].elec <= sr[i].bikes, "élec ≤ vélos"); }
  CHECK(bikes > 0 && elec == bikes, "flotte 100 %% électrique (%d/%d)", elec, bikes);
  for (int i = 0; i < ni; i++) CHECK(!isNonPublic(ir[i].name) && ir[i].name[0], "station publique nommée");
  Weather w;
  if (!wx.empty()) {
    CHECK(wxParse(wx.data(), wx.size(), w) && w.valid && w.nfc >= 3, "météo");
    CHECK(w.score >= 0 && w.score <= 10, "score météo %.1f", w.score);
  }
  Weather r; r.valid = true; r.temp = 12; r.rain = 1.2f; r.wind = 30; r.code = 63; r.nfc = 0;
  wxScore(r);
  CHECK(r.score < 5, "pluie + vent pénalisés (%.1f)", r.score);
}

// Station fictive au profil connu
static InfoRec mkInfo(const char * sid, float lat, float lon, int cap) {
  InfoRec r; memset(&r, 0, sizeof(r));
  strcpy(r.sid, sid); snprintf(r.name, sizeof(r.name), "Test %s", sid);
  r.lat = lat; r.lon = lon; r.cap = (uint16_t)cap;
  return r;
}
static StatusRec mkStat(const char * sid, int bikes, int cap) {
  StatusRec s; memset(&s, 0, sizeof(s));
  strcpy(s.sid, sid);
  s.bikes = (uint16_t)bikes; s.elec = (uint16_t)bikes; s.docks = (uint16_t)(cap - bikes);
  s.installed = s.renting = s.returning = 1;
  return s;
}

static void testModel() {
  settingsDefaults(g_set);
  static Model m;   // instance de test (même code que g_model)
  CHECK(m.init(), "init");
  // 1791842400 = mardi 13/10/2026 00:00 heure de Luxembourg
  const int64_t T0 = 1791842400 - 86400 * 7;   // apprentissage à partir du mardi 06/10
  InfoRec ir[3] = { mkInfo("1", 49.60f, 6.13f, 20), mkInfo("2", 49.61f, 6.14f, 20), mkInfo("3", 49.62f, 6.15f, 20) };
  m.applyInfo(ir, 3, T0);
  CHECK(m.n == 3, "3 stations");
  // 8 jours : station 1 se vide de 7 h à 9 h (20 → 0), remonte à 18 h ; station 2 inverse ; 3 constante
  auto pattern = [](int st, int64_t t) -> int {
    struct tm lt; plat::localTime(t, &lt);
    float h = lt.tm_hour + lt.tm_min / 60.f;
    float f1 = h < 7 ? 1.f : h < 9 ? 1.f - (h - 7) / 2.f : h < 18 ? 0.f : h < 20 ? (h - 18) / 2.f : 1.f;
    if (st == 0) return (int)lroundf(20 * f1);
    if (st == 1) return (int)lroundf(20 * (1.f - f1));
    return 10;
  };
  StatusRec sr[3];
  for (int64_t t = T0; t < T0 + 8 * 86400; t += 300) {
    for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, pattern(s, t), 20);
    m.applyStatus(sr, 3, t);
  }
  CHECK(m.learnPct() > 99, "profil appris %.0f %%", m.learnPct());
  // Mercredi 14/10 06:30 : la station 1 doit prévoir une baisse à +1 h, la 2 une hausse
  int64_t tq = 1791842400 + 86400 + 6 * 3600 + 30 * 60;
  for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, pattern(s, tq), 20);
  m.applyStatus(sr, 3, tq);
  const Station & a = m.st[0], & b = m.st[1], & c = m.st[2];
  printf("  06:30  st1 %d → +1 h %.1f (+2 h %.1f, ETA vide %d min, conf %d %%) | st2 %d → +1 h %.1f | st3 %.1f\n",
         a.bikes, a.fc[12], a.fc[24], a.etaEmpty, a.conf, b.bikes, b.fc[12], c.fc[12]);
  CHECK(a.fc[12] < a.bikes - 3, "st1 se vide à +1 h");
  CHECK(fabsf(a.fc[24] - pattern(0, tq + 7200)) <= 4, "st1 +2 h ≈ profil (%.1f vs %d)", a.fc[24], pattern(0, tq + 7200));
  CHECK(b.fc[12] > b.bikes + 3, "st2 se remplit à +1 h");
  CHECK(fabsf(c.fc[12] - 10) < 1.0f, "st3 stable");
  CHECK(a.conf >= 90, "confiance élevée après 8 jours (%d)", a.conf);
  // Station 1 à 08:40 avec 3 vélos → rupture imminente détectée
  int64_t tr = tq + 2 * 3600 + 10 * 60;
  for (int64_t t = tq + 300; t <= tr; t += 300) {
    for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, pattern(s, t), 20);
    m.applyStatus(sr, 3, t);
  }
  printf("  08:40  st1 %d vélos, ETA vide %d min, besoin %+d, risque %d\n", m.st[0].bikes, m.st[0].etaEmpty, m.st[0].need, m.st[0].risk);
  CHECK(m.st[0].etaEmpty >= 0 && m.st[0].etaEmpty <= 30, "ETA rupture ≤ 30 min");
  CHECK(m.st[0].need > 0, "besoin d'apport");
  bool alert = false;
  for (int k = 0; k < m.nAlerts; k++) if (m.alerts[k].st == 0 && m.alerts[k].type == AL_SOON_EMPTY) alert = true;
  CHECK(alert, "alerte « rupture prévue »");
  // Vide → évènement + alerte critique + transfert depuis la station 2 (pleine)
  sr[0] = mkStat("1", 0, 20); sr[1] = mkStat("2", 19, 20); sr[2] = mkStat("3", 10, 20);
  m.nEv = 0;
  m.applyStatus(sr, 3, tr + 300);
  CHECK(m.st[0].status == ST_EMPTY && m.nEv >= 1 && m.ev[0].type == AL_EMPTY, "évènement rupture");
  CHECK(m.nAlerts > 0 && m.alerts[0].sev == 3, "alerte critique en tête");
  CHECK(m.nTr >= 1 && m.tr[0].to == 0, "transfert suggéré vers la station vide");
  // Trou de 2 h dans les données → tranches vides, pas de plantage
  int before = m.count;
  m.applyStatus(sr, 3, tr + 300 + 7200);
  CHECK(m.count == VN_RING_SLOTS || m.count > before, "ring après trou");
  int idx = m.ringIndex(5);
  CHECK(idx >= 0 && m.net[idx].avail == 0xFFFF, "tranche manquante marquée");
  // Persistance
  sim::g_fsDir = "/tmp/vn_test_fs";
  if (system("rm -rf /tmp/vn_test_fs") != 0) {}
  Model::Blob blob;
  CHECK(m.serialize(blob) && Model::writeSaved(blob), "sauvegarde");
  static Model m2;
  m2.init();
  CHECK(m2.load(tr + 300 + 7200 + 60), "rechargement");
  CHECK(m2.n == 3 && m2.count == m.count && m2.head == m.head, "dimensions restaurées");
  bool same = memcmp(m2.prof, m.prof, sizeof(ProfCell) * 3 * 7 * 96) == 0;
  CHECK(same, "profils identiques");
  int i5 = m.ringIndex(20);
  CHECK(m2.ring[(size_t)i5 * VN_MAX_ST + 1] == m.ring[(size_t)i5 * VN_MAX_ST + 1], "historique identique");
  static Model m3;
  m3.init();
  m3.load(tr + 300 + 7200 + 3 * 86400);
  CHECK(m3.count == 0 && m3.head == -1, "historique 24 h périmé ignoré");
  CHECK(memcmp(m3.prof, m.prof, sizeof(ProfCell) * 3 * 7 * 96) == 0 && m3.learnPct() > 99, "profils conservés");
}

// ── Étalonnage tactile ───────────────────────────────────────────────
namespace hal {
void touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap) {
  x0 = 0; x1 = 479; y0 = 0; y1 = 271; swap = false;
}
}
struct Phys { const char * name; int mode; };
static void phys(int mode, int x, int y, int & rx, int & ry) {
  switch (mode) {
    case 0: rx = x; ry = y; break;                                         // GT911 de la carte
    case 1: ry = 3860 - x * 3600 / 479; rx = 230 + y * 3570 / 271; break;  // résistif, axes échangés + miroir
    case 2: rx = 800 - x * 800 / 480; ry = 480 - y * 480 / 272; break;     // GT911 mal configuré 800×480 inversé
    default: rx = 150 + x * 8; ry = 3900 - y * 13; break;                  // résistif, Y inversé
  }
}
static void testTouchCal() {
  static const int16_t TX[5] = { 40, 440, 440, 40, 240 }, TY[5] = { 40, 40, 232, 232, 136 };
  const char * names[4] = { "GT911 direct", "résistif échangé+miroir", "GT911 800x480 inversé", "résistif Y inversé" };
  for (int m = 0; m < 4; m++) {
    int16_t rx[5], ry[5];
    for (int i = 0; i < 5; i++) {
      int a, b;
      phys(m, TX[i] + ((i * 7) % 5) - 2, TY[i] + ((i * 3) % 5) - 2, a, b);   // doigt imprécis de ±2 px
      rx[i] = (int16_t)a; ry[i] = (int16_t)b;
    }
    vn::TouchCal c;
    bool ok = hal::touchSolveCal(rx, ry, TX, TY, c);
    CHECK(ok, "étalonnage résolu (%s)", names[m]);
    if (!ok) continue;
    vn::g_set.tcal = c;
    int worst = 0;
    for (int y = 0; y < 272; y += 8) for (int x = 0; x < 480; x += 8) {
      int a, b; int16_t sx, sy;
      phys(m, x, y, a, b);
      hal::touchToScreen(a, b, sx, sy);
      int e = abs(sx - x) + abs(sy - y);
      if (e > worst) worst = e;
    }
    printf("  tactile %-28s swap=%d x[%d…%d] y[%d…%d] → erreur max %d px\n", names[m], c.swap, c.x0, c.x1, c.y0, c.y1, worst);
    CHECK(worst <= 6, "précision après étalonnage (%s) : %d px", names[m], worst);
    CHECK(hal::touchCalError(c, rx[4], ry[4], TX[4], TY[4]) <= 5, "contrôle au centre");
  }
  // GT911 de la carte sans étalonnage : correspondance directe
  vn::g_set.tcal.valid = 0;
  int16_t sx, sy;
  hal::touchToScreen(123, 45, sx, sy);
  CHECK(sx == 123 && sy == 45, "GT911 d'usine : brut = pixels (%d,%d)", sx, sy);
  hal::touchToScreen(600, -20, sx, sy);
  CHECK(sx == 479 && sy == 0, "bornage aux bords de l'écran");
  // Mesure dégénérée (toutes les cibles au même endroit) → refusée
  int16_t same[4] = { 500, 500, 500, 500 };
  vn::TouchCal bad;
  CHECK(!hal::touchSolveCal(same, same, TX, TY, bad), "mesure incohérente refusée");
}

// Rupture déjà en cours au démarrage : durée reconstituée depuis l'historique, inconnue sans historique
static void testRuptureStart() {
  settingsDefaults(g_set);
  static Model m;
  CHECK(m.init(), "init (rupture)");
  const int64_t T = 1791842400 + 8 * 3600;                  // 13/10/2026 08:00
  InfoRec ir[2] = { mkInfo("A", 49.60f, 6.13f, 20), mkInfo("B", 49.61f, 6.14f, 20) };
  m.applyInfo(ir, 2, T - 3600);
  StatusRec sr[2];
  // A : 6 vélos jusqu'à 07:35, puis vide ; B : pleine depuis 07:20
  for (int64_t t = T - 3600; t <= T - 300; t += 300) {
    sr[0] = mkStat("A", t < T - 1500 ? 6 : 0, 20);
    sr[1] = mkStat("B", t < T - 2400 ? 12 : 20, 20);
    m.applyStatus(sr, 2, t);
  }
  // Redémarrage : l'historique est restauré mais le premier relevé repart de zéro
  m.lastUpdate = 0;
  for (int i = 0; i < m.n; i++) { m.st[i].status = ST_CLOSED; m.st[i].emptySince = m.st[i].fullSince = 0; }
  sr[0] = mkStat("A", 0, 20); sr[1] = mkStat("B", 20, 20);
  m.applyStatus(sr, 2, T);
  CHECK(m.st[0].status == ST_EMPTY && m.st[0].emptySince == T - 1500,
        "vide depuis 07:35 reconstitué (obtenu %lld min avant)", (long long)((T - m.st[0].emptySince) / 60));
  CHECK(m.st[1].status == ST_FULL && m.st[1].fullSince == T - 2400,
        "pleine depuis 07:20 reconstitué (obtenu %lld min avant)", (long long)((T - m.st[1].fullSince) / 60));
  // Sans historique récent : durée inconnue (0), pas « depuis 0 min »
  static Model m2;
  CHECK(m2.init(), "init (sans historique)");
  m2.applyInfo(ir, 2, T);
  m2.applyStatus(sr, 2, T);
  CHECK(m2.st[0].status == ST_EMPTY && m2.st[0].emptySince == 0, "durée inconnue sans historique");
  // Nouvelle rupture après le démarrage : horodatée au relevé
  sr[1] = mkStat("B", 0, 20);
  m2.applyStatus(sr, 2, T + 60);
  CHECK(m2.st[1].status == ST_EMPTY && m2.st[1].emptySince == T + 60, "nouvelle rupture horodatée");
  CHECK(m2.nEv == 1, "évènement « vient de se vider » (%d)", m2.nEv);
}

int main(int argc, char ** argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  testNames();
  testIso();
  if (argc > 1) testParse(argv[1]);
  testModel();
  testRuptureStart();
  testTouchCal();
  printf("\n%d vérifications réussies, %d échec(s)\n", s_ok, s_fail);
  return s_fail ? 1 : 0;
}
