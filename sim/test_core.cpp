// ── Tests du cœur (sans UI) : parsing, noms, prédiction, persistance ──
// make test  →  ./test_core <dossier_json>
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/gbfs.h"
#include "../src/core/weather.h"
#include "../src/core/ops.h"
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

// Le parc (« en trajet ») ne se lit que sur un creux de NUIT observé : ce test
// fige les trois cas — carte neuve, historique de jour seulement, nuit complète.
static void testParc() {
  settingsDefaults(g_set);
  InfoRec ir[3] = { mkInfo("1", 49.60f, 6.13f, 20), mkInfo("2", 49.61f, 6.14f, 20), mkInfo("3", 49.62f, 6.15f, 20) };
  StatusRec sr[3];

  // (a) carte fraîchement flashée : un seul relevé, en pleine journée
  {
    static Model m;
    CHECK(m.init(), "init parc (neuf)");
    const int64_t t0 = 1791842400 + 14 * 3600;             // mardi 13/10 14:00
    m.applyInfo(ir, 3, t0);
    for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, 5, 20);
    m.applyStatus(sr, 3, t0);
    CHECK(!m.tot.inUseValid, "carte neuve : en-trajet NON valide");
    CHECK(m.tot.inUse == 0, "carte neuve : en-trajet vaut 0 (l'écran montre un tiret)");
    CHECK(!m.tot.deltaValid && !m.tot.actValid, "carte neuve : ni variation ni activité annoncées");
  }

  // (b) 8 h de relevés, mais tous de jour : la profondeur ne suffit pas
  {
    static Model m;
    CHECK(m.init(), "init parc (jour)");
    const int64_t t0 = 1791842400 + 10 * 3600;             // 10:00
    m.applyInfo(ir, 3, t0);
    for (int64_t t = t0; t <= t0 + 8 * 3600; t += 300) {
      for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, 6 + s, 20);
      m.applyStatus(sr, 3, t);
    }
    CHECK(m.count >= 72, "8 h de relevés (%d échantillons)", m.count);
    CHECK(!m.tot.inUseValid, "historique de jour seul : en-trajet NON valide");
    CHECK(m.tot.deltaValid, "8 h de recul : la variation sur 1 h est valide");
  }

  // (c) une nuit est traversée : tous les vélos sont en station de 01 h à 05 h
  {
    static Model m;
    CHECK(m.init(), "init parc (nuit)");
    const int64_t t0 = 1791842400 + 20 * 3600;             // mardi 20:00
    m.applyInfo(ir, 3, t0);
    for (int64_t t = t0; t <= t0 + 10 * 3600; t += 300) {
      struct tm lt; plat::localTime(t, &lt);
      int inStation = (lt.tm_hour >= 1 && lt.tm_hour < 5) ? 20 : 8;
      for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, inStation, 20);
      m.applyStatus(sr, 3, t);
    }
    CHECK(m.count >= 72, "10 h de relevés (%d échantillons)", m.count);
    CHECK(m.tot.inUseValid, "creux de nuit observé : en-trajet VALIDE");
    CHECK(m.tot.fleet == 60, "parc estimé 60 (3 × 20), obtenu %d", m.tot.fleet);
    CHECK(m.tot.inUse == 36, "en-trajet = parc − en station (36), obtenu %d", m.tot.inUse);
    CHECK(m.tot.actValid, "activité sur 1 h disponible");
  }

  // (d) en pleine nuit, le relevé courant EST le maximum (les vélos rentrent encore) :
  //     il doit compter comme creux de nuit, sinon la tuile afficherait « — » chaque nuit.
  {
    static Model m;
    CHECK(m.init(), "init parc (nuit en cours)");
    const int64_t t0 = 1791842400 + 20 * 3600;             // mardi 20:00
    const int64_t t1 = t0 + 7 * 3600 + 1800;               // mercredi 03:30
    m.applyInfo(ir, 3, t0);
    int last = 0;
    for (int64_t t = t0; t <= t1; t += 300) {
      last = 8 + (int)((t - t0) / 1800);                    // remplissage continu jusqu'à 03:30
      if (last > 20) last = 20;
      for (int s = 0; s < 3; s++) sr[s] = mkStat(ir[s].sid, last, 20);
      m.applyStatus(sr, 3, t);
    }
    CHECK(m.tot.inUseValid, "nuit en cours, relevé courant maximal : en-trajet VALIDE");
    CHECK(m.tot.fleet == 3 * last, "parc = relevé courant (%d), obtenu %d", 3 * last, m.tot.fleet);
  }
}


// ── Compteurs d'ingestion (alimentés par la tâche réseau) ────────────
static void testIngest() {
  FeedStat f;
  memset(&f, 0, sizeof(f));
  ingestErr(f, 1000, 503, 0, 1200);
  CHECK(f.fails == 1 && f.errCode == 503 && f.errAt == 1000 && f.okAt == 0 && f.errCount == 1,
        "1er échec compté (fails=%u code=%d)", f.fails, f.errCode);
  ingestErr(f, 1060, -11, 512, 15000);
  CHECK(f.fails == 2 && f.errCode == -11 && f.bytes == 512 && f.ms == 15000, "2e échec consécutif (fails=%u)", f.fails);
  ingestOk(f, 1120, 42890, 812);
  CHECK(f.fails == 0 && f.okAt == 1120 && f.bytes == 42890 && f.ms == 812 && f.okCount == 1,
        "succès : échecs consécutifs remis à 0, taille et durée relevées (fails=%u)", f.fails);
  CHECK(f.errCode == -11 && f.errAt == 1060 && f.errCount == 2, "le dernier échec reste consultable après un succès");
  ingestErr(f, 1180, ING_E_PARSE, 100, 40);
  CHECK(f.fails == 1 && f.okAt == 1120, "un échec après un succès repart de 1 (fails=%u)", f.fails);
  CHECK(strcmp(ingestErrText(ING_E_PARSE), "JSON illisible") == 0 && strcmp(ingestErrText(-11), "d\xC3\xA9lai de lecture d\xC3\xA9pass\xC3\xA9") == 0
        && strcmp(ingestErrText(503), "service indisponible") == 0, "libellés d'erreur");
  for (int a = 0; a < FEED_COUNT; a++)
    for (int b = a + 1; b < FEED_COUNT; b++) CHECK(strcmp(feedName(a), feedName(b)) != 0, "noms de flux distincts");
}

// ── Fraîcheur : seuil du bandeau, des DEUX côtés ─────────────────────
static void testFreshness() {
  Settings s;
  settingsDefaults(s);
  CHECK(s.staleMin == 0, "seuil automatique par défaut");
  CHECK(staleThresholdS(s) == 180, "auto, 60 s → 3 min (%d s)", staleThresholdS(s));
  s.refreshSec = 30;  CHECK(staleThresholdS(s) == 180, "auto, 30 s → plancher 3 min (%d s)", staleThresholdS(s));
  s.refreshSec = 120; CHECK(staleThresholdS(s) == 360, "auto, 2 min → 6 min (%d s)", staleThresholdS(s));
  s.refreshSec = 300; CHECK(staleThresholdS(s) == 900, "auto, 5 min → 15 min (%d s)", staleThresholdS(s));
  s.staleMin = 10;    CHECK(staleThresholdS(s) == 600, "réglage manuel 10 min (%d s)", staleThresholdS(s));
  const int64_t ok = 1791894641;
  CHECK(!isStale(ok, ok + 180, 180), "âge = seuil : pas de bandeau");
  CHECK(isStale(ok, ok + 181, 180), "âge = seuil + 1 s : bandeau");
  CHECK(!isStale(ok, ok + 30, 180), "relevé frais : pas de bandeau");
  CHECK(!isStale(0, ok, 180) && dataAgeS(0, ok) == -1, "jamais reçu : âge inconnu, pas « vieux »");
  CHECK(!isStale(ok, 0, 180), "heure inconnue : pas de bandeau");
  CHECK(dataAgeS(ok, ok + 754) == 754, "âge en secondes");
  CHECK(wxStaleThresholdS() == 3 * VN_WX_REFRESH_S, "météo : 3 × sa période (%d s)", wxStaleThresholdS());
  CHECK(!isStale(ok, ok + wxStaleThresholdS(), wxStaleThresholdS()) && isStale(ok, ok + wxStaleThresholdS() + 1, wxStaleThresholdS()),
        "météo : seuil des deux côtés");
}

// ── Provenance : la règle écrite est celle appliquée ─────────────────
static void testProvenance(const std::string & dir) {
  settingsDefaults(g_set);
  // Modèle vide : rien n'est inventé
  {
    static Model m;
    CHECK(m.init(), "init provenance (vide)");
    m.recompute(1791894641);
    for (int k = 0; k < KPI_COUNT; k++) {
      Provenance p;
      CHECK(provenance(m, k, p), "provenance %d", k);
      CHECK(!p.available && p.missing && p.missing[0], "vide : « %s » sans donnée → raison affichée", p.name);
      CHECK(p.name && p.rule && p.source, "vide : libellé, règle et source");
    }
  }
  auto info = readFile(dir + "/station_information.json");
  auto st = readFile(dir + "/station_status.json");
  auto ty = readFile(dir + "/vehicle_types.json");
  auto wx = readFile(dir + "/weather.json");
  if (info.empty() || st.empty()) return;
  static InfoRec ir[VN_MAX_ST];
  static StatusRec sr[VN_MAX_ST];
  ElecTypes et;
  gbfsParseTypes(ty.data(), ty.size(), et);
  int ni = gbfsParseInfo(info.data(), info.size(), ir, VN_MAX_ST);
  int64_t lu = 0;
  int ns = gbfsParseStatus(st.data(), st.size(), sr, VN_MAX_ST, et, &lu);
  static Model m;
  CHECK(m.init(), "init provenance (flux réel)");
  const int64_t now = lu + 40;
  m.applyInfo(ir, ni, now);
  m.applyStatus(sr, ns, now);
  m.feedUpdated = lu;
  Weather w;
  if (!wx.empty() && wxParse(wx.data(), wx.size(), w)) { w.fetchedAt = now; m.wx = w; }
  // Sommes recalculées indépendamment, enregistrement par enregistrement du flux
  int bikes = 0, docks = 0, dis = 0, ddis = 0, nRent = 0, nRet = 0;
  for (int k = 0; k < ns; k++) {
    bool pub = false;
    for (int j = 0; j < ni; j++) if (strcmp(ir[j].sid, sr[k].sid) == 0) { pub = true; break; }
    if (!pub) continue;
    if (sr[k].renting) { bikes += sr[k].bikes; nRent++; }
    if (sr[k].returning) { docks += sr[k].docks; nRet++; }
    dis += sr[k].bikesDis; ddis += sr[k].docksDis;
  }
  Provenance p;
  provenance(m, KPI_BIKES, p);
  CHECK(p.available && p.kind == SRC_FEED && strcmp(p.feed, "station_status") == 0 && strcmp(p.field, "num_vehicles_available") == 0,
        "vélos dispo : champ num_vehicles_available de station_status");
  CHECK((int)p.value == bikes && p.count == nRent, "vélos dispo = Σ is_renting (%d, attendu %d sur %d stations)", (int)p.value, bikes, nRent);
  CHECK(p.feedAt == lu && p.recvAt == now, "vélos dispo : horodatage last_updated et réception");
  CHECK(!p.estimate && !p.validity, "vélos dispo : mesure, pas une estimation");
  provenance(m, KPI_DOCKS, p);
  CHECK(strcmp(p.field, "num_docks_available") == 0 && (int)p.value == docks && p.count == nRet,
        "bornes libres = Σ is_returning (%d, attendu %d)", (int)p.value, docks);
  provenance(m, KPI_BIKES_DIS, p);
  CHECK(strcmp(p.field, "num_vehicles_disabled") == 0 && (int)p.value == dis, "vélos HS = Σ toutes stations (%d, attendu %d)", (int)p.value, dis);
  provenance(m, KPI_DOCKS_DIS, p);
  CHECK(strcmp(p.field, "num_docks_disabled") == 0 && (int)p.value == ddis, "bornes HS (%d, attendu %d)", (int)p.value, ddis);
  provenance(m, KPI_FILL, p);
  CHECK(p.kind == SRC_CALC && p.available && fabsf(p.value - 100.f * bikes / (bikes + docks)) < 0.01f,
        "remplissage = vélos ÷ (vélos + bornes) (%.2f)", p.value);
  provenance(m, KPI_INUSE, p);
  CHECK(p.estimate && p.validity && !p.validityMet && !p.available && p.missing,
        "en trajet, carte neuve : estimation NON valable, « — » et raison");
  provenance(m, KPI_DELTA1H, p);
  CHECK(!p.available && p.missing, "variation 1 h sans recul : « — » et raison");
  provenance(m, KPI_TEMP, p);
  CHECK(p.available && strcmp(p.source, "Open-Meteo") == 0 && strcmp(p.field, "current.temperature_2m") == 0
        && p.value == w.temp && p.feedAt == now, "température : Open-Meteo current.temperature_2m (%.1f)", p.value);
  provenance(m, KPI_FC60, p);
  CHECK(p.estimate && p.validity && (int)p.value == (int)lroundf(m.netFc[12]), "prévision +1 h : estimation");
  for (int k = 0; k < KPI_COUNT; k++) {
    provenance(m, k, p);
    CHECK(!p.estimate || p.validity, "%s : une estimation dit sa condition de validité", p.name);
    CHECK(p.available || (p.missing && p.missing[0]), "%s : absence motivée", p.name);
    CHECK(p.kind != SRC_FEED || (p.field && p.feed), "%s : champ de flux cité", p.name);
  }
  // Cas discriminant (absent de l'instantané réel, où aucune station fermée ne garde
  // de vélos) : location suspendue avec 6 vélos à quai, retours suspendus avec 9 bornes.
  {
    static Model mc;
    CHECK(mc.init(), "init provenance (stations suspendues)");
    InfoRec r3[3] = { mkInfo("1", 49.60f, 6.13f, 20), mkInfo("2", 49.61f, 6.14f, 20), mkInfo("3", 49.62f, 6.15f, 20) };
    StatusRec s3[3] = { mkStat("1", 5, 20), mkStat("2", 6, 20), mkStat("3", 11, 20) };
    s3[1].renting = 0;                                   // 6 vélos non louables, 14 bornes
    s3[2].returning = 0;                                 // 11 vélos louables, 9 bornes refusées
    mc.applyInfo(r3, 3, now);
    mc.applyStatus(s3, 3, now);
    provenance(mc, KPI_BIKES, p);
    CHECK((int)p.value == 16 && p.count == 2, "vélos dispo hors location suspendue : 5 + 11 = 16 (obtenu %d sur %d)", (int)p.value, p.count);
    provenance(mc, KPI_DOCKS, p);
    CHECK((int)p.value == 29 && p.count == 2, "bornes libres hors retours suspendus : 15 + 14 = 29 (obtenu %d sur %d)", (int)p.value, p.count);
    provenance(mc, KPI_FILL, p);
    CHECK(fabsf(p.value - 100.f * 16 / 45) < 0.01f, "remplissage 16 ÷ 45 (%.2f)", p.value);
  }
  // Avec un creux de nuit : l'estimation devient valable et dit d'où vient le parc
  {
    static Model mn;
    CHECK(mn.init(), "init provenance (nuit)");
    InfoRec r3[3] = { mkInfo("1", 49.60f, 6.13f, 20), mkInfo("2", 49.61f, 6.14f, 20), mkInfo("3", 49.62f, 6.15f, 20) };
    StatusRec s3[3];
    const int64_t t0 = 1791842400 + 20 * 3600;
    mn.applyInfo(r3, 3, t0);
    for (int64_t t = t0; t <= t0 + 10 * 3600; t += 300) {
      struct tm lt; plat::localTime(t, &lt);
      int inSt = (lt.tm_hour >= 1 && lt.tm_hour < 5) ? 20 : 8;
      for (int k = 0; k < 3; k++) s3[k] = mkStat(r3[k].sid, inSt, 20);
      mn.applyStatus(s3, 3, t);
    }
    provenance(mn, KPI_INUSE, p);
    struct tm lt; plat::localTime(p.refAt, &lt);
    CHECK(p.available && p.validityMet && (int)p.value == mn.tot.inUse && lt.tm_hour >= 1 && lt.tm_hour < 5,
          "en trajet après une nuit : valable, maximum daté de la nuit (%02d h)", lt.tm_hour);
  }
}

// Apprend le motif de testModel sur `days` jours à partir du mardi 06/10 00:00
static int patternAt(int st, int64_t t) {
  struct tm lt; plat::localTime(t, &lt);
  float h = lt.tm_hour + lt.tm_min / 60.f;
  float f1 = h < 7 ? 1.f : h < 9 ? 1.f - (h - 7) / 2.f : h < 18 ? 0.f : h < 20 ? (h - 18) / 2.f : 1.f;
  if (st == 0) return (int)lroundf(20 * f1);
  if (st == 1) return (int)lroundf(20 * (1.f - f1));
  if (st == 3) return 20;                              // station de 40 places, stable à 20
  if (st == 4) return 5;                               // station de 10 places, stable à 5
  return 10;
}
static const int NS = 5;
static const int CAPS[NS] = { 20, 20, 20, 40, 10 };
static void learn(Model & m, int days, InfoRec * ir) {
  const int64_t T0 = 1791842400 - 86400 * 7;
  ir[0] = mkInfo("1", 49.60f, 6.13f, 20); ir[1] = mkInfo("2", 49.61f, 6.14f, 20);
  ir[2] = mkInfo("3", 49.62f, 6.15f, 20); ir[3] = mkInfo("4", 49.63f, 6.16f, 40);
  ir[4] = mkInfo("5", 49.64f, 6.17f, 10);
  m.applyInfo(ir, NS, T0);
  StatusRec sr[NS];
  for (int64_t t = T0; t < T0 + (int64_t)days * 86400; t += 300) {
    for (int s = 0; s < NS; s++) sr[s] = mkStat(ir[s].sid, patternAt(s, t), CAPS[s]);
    m.applyStatus(sr, NS, t);
  }
}
static void setAll(Model & m, InfoRec * ir, int64_t t, int st3 = -1, int st4 = -1, int st5 = -1) {
  StatusRec sr[NS];
  for (int s = 0; s < NS; s++) sr[s] = mkStat(ir[s].sid, patternAt(s, t), CAPS[s]);
  if (st3 >= 0) sr[2] = mkStat("3", st3, 20);
  if (st4 >= 0) sr[3] = mkStat("4", st4, 40);
  if (st5 >= 0) sr[4] = mkStat("5", st5, 10);
  m.applyStatus(sr, NS, t);
}
static int countAlerts(const Model & m, int type, int st) {
  int c = 0;
  for (int k = 0; k < m.nAlerts; k++) if (m.alerts[k].type == type && (st < 0 || m.alerts[k].st == st)) c++;
  return c;
}

// ── Prévision 24 h : même règle que la prévision 2 h ─────────────────
static void testProjection24() {
  settingsDefaults(g_set);
  static Model m;
  CHECK(m.init(), "init 24 h");
  InfoRec ir[NS];
  learn(m, 8, ir);
  const int64_t tq = 1791842400 + 86400 + 6 * 3600 + 30 * 60;   // mercredi 14/10 06:30
  setAll(m, ir, tq);
  float o5[VN_FC_STEPS], c5[VN_FC_STEPS];
  CHECK(m.project(0, tq, 5, VN_FC_STEPS, o5, c5), "projection 2 h");
  float worst = 0;
  for (int k = 0; k < VN_FC_STEPS; k++) worst = fmaxf(worst, fabsf(o5[k] - m.st[0].fc[k]));
  CHECK(worst < 1e-4f, "project() au pas de 5 min = prévision 2 h de la fiche (écart max %.5f)", worst);
  CHECK(lroundf(c5[12] * 100) == m.st[0].conf, "confiance à +1 h identique (%ld vs %d)", lroundf(c5[12] * 100), m.st[0].conf);
  static const int N24 = 24 * 4 + 1;
  float o24[N24], c24[N24];
  CHECK(m.project(0, tq, 15, N24, o24, c24), "projection 24 h");
  CHECK(fabsf(o24[8] - m.st[0].fc[24]) < 1e-4f, "24 h à +2 h = prévision 2 h à +2 h (%.2f vs %.2f)", o24[8], m.st[0].fc[24]);
  printf("  24 h st1 depuis mer. 06:30 : +2 h %.1f · +12 h %.1f (profil %d) · +24 h %.1f\n",
         o24[8], o24[48], patternAt(0, tq + 12 * 3600), o24[96]);
  CHECK(fabsf(o24[48] - patternAt(0, tq + 12 * 3600)) <= 2, "+12 h (18:30) suit le profil appris (%.1f vs %d)", o24[48], patternAt(0, tq + 12 * 3600));
  CHECK(fabsf(o24[96] - patternAt(0, tq + 86400)) <= 2, "+24 h revient au niveau du matin (%.1f)", o24[96]);
  float cmin = 1;
  for (int k = 0; k < N24; k++) cmin = fminf(cmin, c24[k]);
  CHECK(cmin >= 0.9f, "profil de 8 jours : confiance ≥ 90 %% sur 24 h (min %.2f)", cmin);
  CHECK(!m.project(99, tq, 15, N24, o24, c24), "station inconnue refusée");

  // Profil d'un seul jour : la courbe existe mais chaque créneau est signalé peu fiable
  static Model y;
  CHECK(y.init(), "init 24 h (jeune)");
  InfoRec iy[NS];
  learn(y, 1, iy);                                               // mardi seulement
  const int64_t tw = 1791842400 - 86400 * 6 + 10 * 3600;        // mercredi 07/10 10:00
  setAll(y, iy, tw);
  y.project(0, tw, 15, N24, o24, c24);
  float cmax = 0;
  for (int k = 0; k < N24; k++) cmax = fmaxf(cmax, c24[k]);
  CHECK(cmax < 0.5f, "profil d'1 jour : confiance < 50 %% partout (max %.2f) → tracé en pointillés", cmax);
  // Aucun profil : courbe plate à la valeur courante, confiance nulle (rien d'inventé)
  static Model z;
  CHECK(z.init(), "init 24 h (sans profil)");
  InfoRec iz[1] = { mkInfo("9", 49.6f, 6.1f, 20) };
  z.applyInfo(iz, 1, tw);
  StatusRec sz[1] = { mkStat("9", 7, 20) };
  z.applyStatus(sz, 1, tw);
  z.project(0, tw, 15, N24, o24, c24);
  // (le premier relevé vient d'alimenter sa propre cellule : n = 1, confiance 1/8 au plus)
  bool flat = true;
  float cz = 0;
  for (int k = 0; k < N24; k++) { if (o24[k] != 7.f) flat = false; cz = fmaxf(cz, c24[k]); }
  CHECK(flat && cz <= 0.125f, "premier relevé : 7 vélos constants, confiance ≤ 1/8 (max %.3f)", cz);
}

// ── Écart au profil (« inhabituel ») ─────────────────────────────────
static void testAnomaly() {
  settingsDefaults(g_set);
  static Model m;
  CHECK(m.init(), "init écart");
  InfoRec ir[NS];
  learn(m, 8, ir);
  // Mercredi 14/10 10:00 : relevé « normal » qui ouvre la tranche (le profil est alimenté
  // avec la valeur habituelle), puis relevé de test dans la même tranche (profil intact).
  const int64_t t10 = 1791842400 + 86400 + 10 * 3600;
  setAll(m, ir, t10);
  const Station & s3 = m.st[2];
  const Station & s4 = m.st[3];
  CHECK(s3.anom == AN_NORMAL && s3.anomExp == 10 && s3.anomTol == 4 && s3.anomConf >= 90,
        "st3 10 vélos, attendu 10 ± 4 (état %d, attendu %d ± %d, conf %d)", s3.anom, s3.anomExp, s3.anomTol, s3.anomConf);
  CHECK(s4.anomTol == 8, "tolérance = 20 %% de la capacité si > 4 vélos (40 places → ± %d)", s4.anomTol);
  CHECK(countAlerts(m, AL_UNUSUAL, -1) == 0, "aucune alerte quand tout est habituel");
  setAll(m, ir, t10 + 60, 14);
  CHECK(s3.anom == AN_NORMAL && countAlerts(m, AL_UNUSUAL, 2) == 0, "écart = tolérance (14 vs 10 ± 4) : silence");
  setAll(m, ir, t10 + 120, 15);
  CHECK(s3.anom == AN_HIGH && countAlerts(m, AL_UNUSUAL, 2) == 1, "écart > tolérance (15 vs 10 ± 4) : alerte « au-dessus »");
  int v = -999, sev = -1;
  for (int k = 0; k < m.nAlerts; k++) if (m.alerts[k].type == AL_UNUSUAL && m.alerts[k].st == 2) { v = m.alerts[k].value; sev = m.alerts[k].sev; }
  CHECK(v == 5 && m.alertCount[AL_UNUSUAL] == 1, "alerte : écart signé +5 (%d)", v);
  CHECK(sev == 0, "alerte « inhabituel » de sévérité info (hors priorités d'intervention) : %d", sev);
  setAll(m, ir, t10 + 180, 6);
  CHECK(s3.anom == AN_NORMAL, "6 vs 10 ± 4 : silence");
  setAll(m, ir, t10 + 240, 5);
  CHECK(s3.anom == AN_LOW && countAlerts(m, AL_UNUSUAL, 2) == 1, "5 vs 10 ± 4 : alerte « en dessous »");
  setAll(m, ir, t10 + 270, -1, 28);
  CHECK(s4.anom == AN_NORMAL, "40 places : 28 vs 20 ± 8 → silence (pas d'alerte pour 8 vélos)");
  setAll(m, ir, t10 + 290, -1, 29);
  CHECK(s4.anom == AN_HIGH, "40 places : 29 vs 20 ± 8 → alerte");
  // Petite station : 20 % de 10 places = 2 vélos, c'est le plancher de 4 vélos qui s'applique
  const Station & s5 = m.st[4];
  setAll(m, ir, t10 + 295, -1, -1, 8);
  CHECK(s5.anomTol == 4 && s5.anom == AN_NORMAL, "10 places : 8 vs 5 → silence, plancher ± 4 (± %d)", s5.anomTol);
  setAll(m, ir, t10 + 298, -1, -1, 10);
  CHECK(s5.anom == AN_HIGH, "10 places : 10 vs 5 ± 4 → alerte");
  // Hors heures d'activité : silence, même pour un écart énorme
  const int64_t t23 = 1791842400 + 86400 + 23 * 3600;
  setAll(m, ir, t23);
  setAll(m, ir, t23 + 60, 0);
  CHECK(s3.anom == AN_OFF_HOURS && countAlerts(m, AL_UNUSUAL, -1) == 0, "23 h : pas d'alerte (état %d)", s3.anom);
  // Profil trop jeune : silence, même pour un écart énorme
  static Model y;
  CHECK(y.init(), "init écart (jeune)");
  InfoRec iy[NS];
  learn(y, 1, iy);
  const int64_t tw = 1791842400 - 86400 * 6 + 10 * 3600;        // mercredi 07/10 10:00, 1 jour appris
  setAll(y, iy, tw);
  setAll(y, iy, tw + 60, 0);
  CHECK(y.st[2].anom == AN_YOUNG && y.st[2].anomConf < 50 && countAlerts(y, AL_UNUSUAL, -1) == 0,
        "profil d'1 jour : pas d'alerte (état %d, conf %d %%)", y.st[2].anom, y.st[2].anomConf);
  // Aucun profil
  static Model z;
  CHECK(z.init(), "init écart (sans profil)");
  InfoRec iz[1] = { mkInfo("9", 49.6f, 6.1f, 20) };
  z.applyInfo(iz, 1, tw);
  StatusRec sz[1] = { mkStat("9", 0, 20) };
  // profil vide : on lit avant que le relevé ne l'alimente (l'état reflète « aucun profil »)
  z.applyStatus(sz, 1, tw);
  CHECK(countAlerts(z, AL_UNUSUAL, -1) == 0 && (z.st[0].anom == AN_NO_PROFILE || z.st[0].anom == AN_YOUNG),
        "premier relevé : pas d'alerte (état %d)", z.st[0].anom);
}

// Atténuation nocturne : 23 h – 4 h seulement, bornes exactes (heure de Luxembourg)
static int64_t lux(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  tzset();
  return (int64_t)mktime(&t);
}
static void testNightDim() {
  settingsDefaults(g_set);
  struct { int h, mi; bool dim; } T[] = {
    { 12, 0, false }, { 22, 0, false }, { 22, 59, false }, { 23, 0, true }, { 23, 30, true },
    { 0, 0, true }, { 3, 59, true }, { 4, 0, false }, { 5, 30, false }, { 6, 0, false },
  };
  for (int day = 0; day < 2; day++) {          // hiver (CET) et été (CEST)
    int mo = day ? 7 : 1;
    for (auto & c : T) {
      int64_t t = lux(2026, mo, 15, c.h, c.mi);
      struct tm lt; plat::localTime(t, &lt);
      CHECK(lt.tm_hour == c.h && lt.tm_min == c.mi, "heure locale %02d:%02d", c.h, c.mi);
      g_set.brightness = 100; g_set.nightDim = 1;
      uint8_t b = effectiveBrightness(t);
      CHECK(b == (c.dim ? 20 : 100), "%02d/%02d %02d:%02d → %u %% (attendu %d)", 15, mo, c.h, c.mi, b, c.dim ? 20 : 100);
      g_set.brightness = 60;
      b = effectiveBrightness(t);
      CHECK(b == (c.dim ? 20 : 60), "réglage 60 %% à %02d:%02d → %u %%", c.h, c.mi, b);
      g_set.brightness = 100; g_set.nightDim = 0;
      b = effectiveBrightness(t);
      CHECK(b == 100, "interrupteur éteint à %02d:%02d → %u %%", c.h, c.mi, b);
    }
  }
  g_set.brightness = 15; g_set.nightDim = 1;
  CHECK(effectiveBrightness(lux(2026, 1, 15, 23, 30)) == 15, "réglage sous le plancher : jamais relevé à 20 %%");
  g_set.brightness = 100;
  CHECK(effectiveBrightness(0) == 100, "heure inconnue : pas d'atténuation");
  settingsDefaults(g_set);
}

int main(int argc, char ** argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  testNames();
  testIso();
  if (argc > 1) testParse(argv[1]);
  testModel();
  testParc();
  testRuptureStart();
  testIngest();
  testFreshness();
  testProvenance(argc > 1 ? argv[1] : "");
  testProjection24();
  testAnomaly();
  testTouchCal();
  testNightDim();
  printf("\n%d vérifications réussies, %d échec(s)\n", s_ok, s_fail);
  return s_fail ? 1 : 0;
}
