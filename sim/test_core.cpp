// ── Tests du cœur (sans UI) : parsing, noms, prédiction, persistance ──
// make test  →  ./test_core <dossier_json>
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/gbfs.h"
#include "../src/core/weather.h"
#include "../src/core/ops.h"
#include "../src/core/wifiscan.h"
#include "../src/core/calendar.h"
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

// /vn_prof.bin des versions 1 et 2 (cellule de 3 octets), reconstruit depuis un fichier v3 de nSt stations :
// mêmes moyennes et mêmes n, sans dispersion (v2 garde l'apprentissage météo, v1 non)
static std::vector<uint8_t> legacyProf(const std::vector<uint8_t> & v3, int nSt, int ver) {
  const size_t hdr = 40, cells = (size_t)nSt * 7 * 96;
  std::vector<uint8_t> out(v3.begin(), v3.begin() + hdr + (size_t)nSt * 8);
  uint16_t v = (uint16_t)ver; memcpy(out.data() + 4, &v, 2);
  const uint8_t * c4 = v3.data() + hdr + (size_t)nSt * 8;
  for (size_t q = 0; q < cells; q++) {
    ProfCell c; memcpy(&c, c4 + q * 4, 4);
    uint8_t b[3]; memcpy(b, &c.a, 2); b[2] = (uint8_t)c.n;
    out.insert(out.end(), b, b + 3);
  }
  const uint8_t * rest = c4 + cells * 4;
  out.insert(out.end(), rest, rest + 7 * 96);
  if (ver >= 2) out.insert(out.end(), rest + 7 * 96, rest + 7 * 96 + sizeof(WxClimate) + sizeof(WxStats) * (1 + nSt));
  return out;
}
// Moyennes et n identiques, dispersion inconnue (ce que doit donner une migration v1/v2 → v3)
static bool sameMeans(const Model & a, const Model & b, int nSt, bool dispZero = true) {
  for (size_t q = 0; q < (size_t)nSt * 7 * 96; q++) {
    if (a.prof[q].a != b.prof[q].a || a.prof[q].n != b.prof[q].n) return false;
    if (dispZero && (a.prof[q].d || a.prof[q].dn)) return false;
  }
  return true;
}

// ── Écart au profil (« inhabituel ») ─────────────────────────────────
static void testAnomaly() {
  settingsDefaults(g_set);
  g_alertOpt = AlertOpts();                        // la formule fixe, aussitôt (la règle de la v1.0.8)
  g_alertOpt.disp = false; g_alertOpt.persist = 1;
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
  g_alertOpt = AlertOpts();
}

// ── Alerte calme : dispersion apprise (1a), inertie (1b), migration de /vn_prof.bin v3 ──
// Trois stations de 20 places, toutes à 10 vélos en moyenne : « calme » (bruit −1…+1),
// « agitée » (bruit −4…+4, uniforme), « fixe » (aucun bruit).
static const int64_t AC_T0 = 1791842400 - 86400 * 7;          // mardi 06/10 00:00
static int acNoise(int st, int64_t t) {
  uint32_t h = (uint32_t)(t / 300) * 2654435761u ^ (uint32_t)(st + 1) * 40503u;
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  if (st == 0) { static const int N[4] = { -1, 0, 0, 1 }; return N[h % 4]; }
  if (st == 1) return (int)(h % 9) - 4;
  return 0;
}
static void acSet(Model & m, int64_t t, int v0 = -1, int v1 = -1, int v2 = -1) {
  StatusRec sr[3];
  int v[3] = { v0, v1, v2 };
  for (int s = 0; s < 3; s++) {
    char sid[4]; snprintf(sid, sizeof(sid), "%d", 21 + s);
    sr[s] = mkStat(sid, v[s] >= 0 ? v[s] : 10 + acNoise(s, t), 20);
  }
  m.applyStatus(sr, 3, t);
}
static void acLearn(Model & m, int days) {
  InfoRec ir[3] = { mkInfo("21", 49.60f, 6.13f, 20), mkInfo("22", 49.61f, 6.14f, 20), mkInfo("23", 49.62f, 6.15f, 20) };
  m.applyInfo(ir, 3, AC_T0);
  for (int64_t t = AC_T0; t < AC_T0 + (int64_t)days * 86400; t += 300) acSet(m, t);
}

static void testAlertCalm() {
  settingsDefaults(g_set);
  g_alertOpt = AlertOpts();
  static Model m;
  CHECK(m.init(), "init alerte calme");
  acLearn(m, 21);
  const int64_t tq = AC_T0 + 22 * 86400 + 10 * 3600;          // mercredi 28/10, en journée
  const Station & calm = m.st[0], & agit = m.st[1], & fixe = m.st[2];

  // 1. La dispersion apprise suit le bruit réel de chaque station (écart absolu moyen : 0,5 ; 2,2 ; 0)
  float d0, c0, d1, c1, d2, c2;
  CHECK(m.dispersionAt(0, tq, d0, c0) && m.dispersionAt(1, tq, d1, c1) && m.dispersionAt(2, tq, d2, c2), "dispersion connue (3 semaines)");
  CHECK(fabsf(d0 - 0.5f) < 0.25f && fabsf(d1 - 2.22f) < 0.6f && d2 < 0.15f && c0 >= 4 && c1 >= 4,
        "dispersion apprise : calme %.2f (0,5), agitée %.2f (2,2), fixe %.2f (0) ; relevés %.0f / %.0f", d0, d1, d2, c0, c1);

  // 2. Levier 1a : la même valeur jugée selon la variabilité de SA station
  g_alertOpt.disp = true; g_alertOpt.k = 3; g_alertOpt.persist = 1;
  acSet(m, tq, 10, 16, 10);
  CHECK(agit.anomSrc == ATS_DISP && agit.anom == AN_NORMAL && agit.anomTol == (int)lroundf(fmaxf(4, 3 * agit.anomDisp10 / 10.f)) && agit.anomTol >= 6,
        "agitée : 16 contre 10, variation habituelle ± %.1f → seuil ± %d, silence (état %d)", d1, agit.anomTol, agit.anom);
  CHECK(agit.anomDisp10 >= 15 && agit.anomDisp10 <= 30 && agit.anomDispDays >= 17 && agit.anomDispDays <= 22,
        "agitée : dispersion affichable %d dixièmes, mesurée sur %d jours", agit.anomDisp10, agit.anomDispDays);
  g_alertOpt.disp = false;
  m.recompute(tq);
  CHECK(agit.anom == AN_HIGH && agit.anomSrc == ATS_FORMULA, "même relevé, formule fixe (± 4) : l'agitée sonnait (état %d)", agit.anom);
  g_alertOpt.disp = true;
  acSet(m, tq + 60, 15, -1, -1);
  CHECK(calm.anomSrc == ATS_DISP && calm.anomTol == 4 && calm.anom == AN_HIGH && countAlerts(m, AL_UNUSUAL, 0) == 1,
        "vrai écart sur la station calme (15 contre 10, plancher ± 4) : alerte quand même (état %d, ± %d)", calm.anom, calm.anomTol);
  acSet(m, tq + 90, -1, -1, 15);
  CHECK(fixe.anomSrc == ATS_DISP && fixe.anomTol == 4 && fixe.anom == AN_HIGH, "dispersion nulle : le plancher de 4 vélos tient (± %d)", fixe.anomTol);
  acSet(m, tq + 120, 10, 2, 10);
  CHECK(agit.anom == AN_LOW && countAlerts(m, AL_UNUSUAL, 1) == 1, "agitée : 2 contre 10 (écart 8 > ± %d) : alerte « en dessous »", agit.anomTol);

  // 2b. Ce que l'écran en dit : chaque chiffre est celui du calcul, l'origine est nommée
  {
    char t1[256], t2[128], want[96];
    anomLineText(agit, t1, sizeof(t1));
    anomAlertText(agit, t2, sizeof(t2));
    snprintf(want, sizeof(want), "\xC2\xB1 %d,%d (mesur\xC3\xA9 sur %d jours) \xE2\x86\x92 seuil \xC2\xB1 %d (3 \xC3\x97 l'\xC3\xA9" "cart habituel)",
             agit.anomDisp10 / 10, agit.anomDisp10 % 10, agit.anomDispDays, agit.anomTol);
    CHECK(strstr(t1, "Inhabituel : 2 v\xC3\xA9los contre 10 attendus, \xC3\xA9" "cart de -8") && strstr(t1, want),
          "fiche (1a) : « %s »", t1);
    snprintf(want, sizeof(want), "Inhabituel : 2 contre 10 \xC2\xB7 \xC2\xB1 %d (habituel \xC2\xB1 %d,%d)", agit.anomTol, agit.anomDisp10 / 10, agit.anomDisp10 % 10);
    CHECK(!strcmp(t2, want), "liste (1a) : « %s »", t2);
    acSet(m, tq + 150, 10, 10, 10);
    anomLineText(calm, t1, sizeof(t1));
    CHECK(strstr(t1, "Habituel : 10 v\xC3\xA9los contre 10 attendus") && strstr(t1, "seuil \xC2\xB1 4 (plancher de 4 v\xC3\xA9los)"),
          "créneau très calme : le plancher est nommé (« %s »)", t1);
    g_alertOpt.disp = false;
    m.recompute(tq + 150);
    anomLineText(agit, t1, sizeof(t1));
    char want2[200];
    snprintf(want2, sizeof(want2), "Habituel : 10 v\xC3\xA9los contre 10 attendus (\xC2\xB1 4) \xC2\xB7 confiance du profil 100 %% \xC2\xB7 "
             "\xC3\xA9" "cart habituel \xC3\xA0 cette heure \xC2\xB1 %d,%d, mesur\xC3\xA9 sur %d jours (pour information)",
             agit.anomDisp10 / 10, agit.anomDisp10 % 10, agit.anomDispDays);
    CHECK(!strcmp(t1, want2), "formule fixe : phrase de la v1.0.8, l'écart habituel en information (« %s »)", t1);
    anomLineText(fixe, t1, sizeof(t1));
    CHECK(strstr(t1, "\xC2\xB1 0,0, mesur\xC3\xA9") != nullptr && fixe.anomDispOk, "dispersion nulle affichée 0,0 (pas 0,1) : « %s »", t1);
    Station u = agit; u.anomDispOk = false;
    anomLineText(u, t1, sizeof(t1));
    CHECK(!strcmp(t1, "Habituel : 10 v\xC3\xA9los contre 10 attendus (\xC2\xB1 4) \xC2\xB7 confiance du profil 100 %"),
          "formule fixe sans dispersion connue : phrase de la v1.0.8 à l'identique (« %s »)", t1);
    g_alertOpt.disp = true;
    acSet(m, tq + 120 + 60, 10, 2, 10);
  }

  // 3. L'alerte ne touche pas la prévision : fc identique quel que soit le réglage de l'alerte
  float fcA[VN_FC_STEPS], fcB[VN_FC_STEPS];
  g_alertOpt.disp = false; g_alertOpt.persist = 1; m.recompute(tq + 120); memcpy(fcA, agit.fc, sizeof(fcA));
  g_alertOpt.disp = true; g_alertOpt.persist = 3; m.recompute(tq + 120); memcpy(fcB, agit.fc, sizeof(fcB));
  CHECK(memcmp(fcA, fcB, sizeof(fcA)) == 0 && agit.conf > 0, "prévision 2 h identique, alerte calme ou non");

  // 4. Levier 1b : n'alerter que si l'écart tient 3 relevés de 5 min consécutifs
  g_alertOpt.persist = 3;
  const int64_t ti = tq + 3600 - (tq + 3600) % 300;           // début de tranche
  acSet(m, ti, 0);
  CHECK(calm.anom == AN_PENDING && calm.anomRun == 1 && countAlerts(m, AL_UNUSUAL, 0) == 0,
        "inertie : 1er relevé hors tolérance → en attente, pas d'alerte (état %d, %d/3)", calm.anom, calm.anomRun);
  acSet(m, ti + 300, 0);
  CHECK(calm.anom == AN_PENDING && calm.anomRun == 2 && countAlerts(m, AL_UNUSUAL, 0) == 0, "2e relevé : toujours en attente (%d/3)", calm.anomRun);
  {
    char t1[256];
    anomLineText(calm, t1, sizeof(t1));
    char want[160];
    snprintf(want, sizeof(want), "\xC3\x80 confirmer : 0 v\xC3\xA9lo contre %d attendus, \xC3\xA9" "cart de %+d (2 relev\xC3\xA9s sur 3 de suite avant l'alerte)",
             calm.anomExp, -calm.anomExp);
    CHECK(strstr(t1, want) != nullptr,
          "fiche (1b) : l'attente est dite (« %s »)", t1);
  }
  acSet(m, ti + 600, 1);
  CHECK(calm.anom == AN_LOW && calm.anomRun == 3 && countAlerts(m, AL_UNUSUAL, 0) == 1,
        "vrai incident (station vidée) : alerte au 3e relevé, 10 min après (état %d)", calm.anom);
  acSet(m, ti + 660, 0);
  CHECK(calm.anom == AN_LOW, "relevé de la même tranche : l'alerte tient");
  acSet(m, ti + 900);
  CHECK(calm.anom == AN_NORMAL && calm.anomRun == 0, "retour dans la tolérance : silence (état %d)", calm.anom);
  acSet(m, ti + 1200, 16); acSet(m, ti + 1500, 4); acSet(m, ti + 1800, 16);
  CHECK(calm.anom == AN_PENDING && calm.anomRun == 1 && countAlerts(m, AL_UNUSUAL, 0) == 0,
        "écarts alternés (+6, −6, +6) : jamais confirmés (%d/3)", calm.anomRun);
  // une tranche manquante interrompt la série
  acSet(m, ti + 2100, 0);
  acSet(m, ti + 2700, 0);                                      // pas de relevé à ti + 2400
  CHECK(calm.anom == AN_PENDING && calm.anomRun == 1, "trou dans l'historique : la série repart de 1 (%d/3)", calm.anomRun);
  acSet(m, ti + 3000, 0); acSet(m, ti + 3300, 0);
  CHECK(calm.anom == AN_LOW && countAlerts(m, AL_UNUSUAL, 0) == 1, "puis 3 relevés consécutifs : alerte");
  // inertie et dispersion ensemble : l'agitée, vidée, sonne quand même
  acSet(m, ti + 3600, 10, 0); acSet(m, ti + 3900, 10, 0); acSet(m, ti + 4200, 10, 1);
  CHECK(agit.anom == AN_LOW && agit.anomSrc == ATS_DISP && countAlerts(m, AL_UNUSUAL, 1) == 1,
        "agitée vidée 15 min (1a + 1b) : alerte (état %d, ± %d)", agit.anom, agit.anomTol);
  g_alertOpt.persist = 1;

  // 5. Persistance v3 : aller-retour exact (moyenne, n, dispersion, compte, date de début)
  sim::g_fsDir = "/tmp/vn_test_calm";
  if (system("rm -rf /tmp/vn_test_calm") != 0) {}
  Model::Blob b;
  CHECK(m.serialize(b), "sérialisation profils v3");
  std::vector<uint8_t> v3(b.prof, b.prof + b.profLen);
  Model::writeSaved(b);
  uint16_t ver; memcpy(&ver, v3.data() + 4, 2);
  CHECK(ver == 3 && v3.size() == 40 + 3 * 8 + 3 * 672 * 4 + 672 + sizeof(WxClimate) + 4 * sizeof(WxStats) + 3 * 8,
        "fichier v3 : version %d, %zu octets (cellule de 4 octets + date de dispersion)", ver, v3.size());
  const int64_t tl = ti + 4500;
  static Model r;
  r.init(); r.load(tl);
  CHECK(memcmp(r.prof, m.prof, sizeof(ProfCell) * 3 * 672) == 0 && memcmp(r.dispSince, m.dispSince, 3 * 8) == 0,
        "v3 relu : cellules et dates de dispersion identiques");
  // 6. Migration v2 (v1.0.9) et v1 (v1.0.8) : moyenne et n intacts, dispersion inconnue → formule fixe, dite
  for (int lv = 2; lv >= 1; lv--) {
    std::vector<uint8_t> old = legacyProf(v3, 3, lv);
    plat::fsWrite("/vn_prof.bin", old.data(), old.size());
    static Model o1, o2;
    Model & o = lv == 2 ? o2 : o1;
    o.init();
    CHECK(o.load(tl), "v%d relu", lv);
    CHECK(sameMeans(o, m, 3) && o.dispSince[1] == 0, "v%d → v3 : moyennes et n identiques, dispersion inconnue", lv);
    CHECK(lv == 1 ? o.wxG.sn == 0 : memcmp(&o.wxG, &m.wxG, sizeof(WxStats)) == 0, "v%d : apprentissage météo %s", lv, lv == 1 ? "à zéro" : "gardé");
    InfoRec io[3] = { mkInfo("21", 49.60f, 6.13f, 20), mkInfo("22", 49.61f, 6.14f, 20), mkInfo("23", 49.62f, 6.15f, 20) };
    o.applyInfo(io, 3, tl);
    g_alertOpt.disp = true;
    acSet(o, tl, -1, 16);
    CHECK(o.st[1].anomSrc == ATS_FORMULA_YOUNG && o.st[1].anomTol == 4 && o.st[1].anom == AN_HIGH && !o.st[1].anomDispOk,
          "v%d migré : dispersion pas encore mesurée → formule fixe (± %d), et l'écran le sait (origine %d)", lv, o.st[1].anomTol, o.st[1].anomSrc);
    char t1[256], t2[128];
    anomLineText(o.st[1], t1, sizeof(t1));
    anomAlertText(o.st[1], t2, sizeof(t2));
    CHECK(strstr(t1, "\xC2\xB1 4 par d\xC3\xA9" "faut : \xC3\xA9" "cart habituel de ce cr\xC3\xA9neau pas encore mesur\xC3\xA9") && strstr(t2, "(\xC2\xB1 4 par d\xC3\xA9" "faut)"),
          "v%d migré : « %s » / « %s »", lv, t1, t2);
    if (lv == 2) {                                          // la dispersion se réapprend : une semaine plus tard, elle sert
      for (int64_t t = tl + 300; t < tl + 8 * 86400; t += 300) acSet(o, t);
      acSet(o, tl + 8 * 86400 + 600, -1, 16);
      CHECK(o.st[1].anomSrc == ATS_DISP && o.st[1].anom == AN_NORMAL && o.st[1].anomDispDays >= 7,
            "v2 migré + 8 jours : dispersion réapprise (± %d, %d jours), l'agitée ne sonne plus pour 6 vélos", o.st[1].anomTol, o.st[1].anomDispDays);
    }
  }
  // 7. Fichiers corrompus : ignorés sans plantage, jamais lus de travers
  struct Bad { std::string what; std::vector<uint8_t> data; };
  std::vector<Bad> bads;
  { auto x = v3; uint16_t v = 4; memcpy(x.data() + 4, &v, 2); bads.push_back({ "version 4 (inconnue)", x }); }
  { auto x = v3; x.pop_back(); bads.push_back({ "v3 tronqué d'un octet", x }); }
  { auto x = legacyProf(v3, 3, 2); uint16_t v = 3; memcpy(x.data() + 4, &v, 2); bads.push_back({ "v2 annoncé v3 (taille fausse)", x }); }
  { auto x = v3; uint16_t v = 0; memcpy(x.data() + 4, &v, 2); bads.push_back({ "version 0", x }); }
  { auto x = v3; uint16_t ns = 9999; memcpy(x.data() + 6, &ns, 2); bads.push_back({ "nombre de stations aberrant", x }); }
  plat::fsRemove("/vn_hist.bin");                 // le profil seul (l'historique et les résumés par jour
  plat::fsRemove("/vn_days.bin");                 // recréeraient les stations)
  for (auto & bd : bads) {
    plat::fsWrite("/vn_prof.bin", bd.data.data(), bd.data.size());
    Model & q = *new Model();                      // un modèle neuf par fichier (le banc de la carte au démarrage)
    q.init();
    q.load(tl);
    bool empty = true;
    for (int k = 0; k < 3 * 672; k++) if (q.prof[k].n || q.prof[k].dn) empty = false;
    CHECK(empty && q.n == 0, "%s : fichier ignoré, profil vide", bd.what.c_str());
  }
  { // n hors bornes dans un v2 (corruption douce) : plafonné ; date de dispersion future : oubliée
    auto x = legacyProf(v3, 3, 2);
    x[40 + 3 * 8 + 2] = 200;
    plat::fsWrite("/vn_prof.bin", x.data(), x.size());
    static Model q2; q2.init(); q2.load(tl);
    CHECK(q2.prof[0].n == VN_PROFILE_DECAY_N, "v2, n = 200 dans une cellule : plafonné à %d (%d)", VN_PROFILE_DECAY_N, q2.prof[0].n);
    auto y = v3;
    int64_t fut = tl + 400 * 86400;
    memcpy(y.data() + y.size() - 3 * 8, &fut, 8);
    plat::fsWrite("/vn_prof.bin", y.data(), y.size());
    static Model q3; q3.init(); q3.load(tl);
    CHECK(q3.dispSince[0] == 0 && q3.dispSince[1] == m.dispSince[1], "date de dispersion dans le futur : oubliée (les autres gardées)");
  }
  g_alertOpt = AlertOpts();
}

// ── Météo dans l'historique 5 min + format versionné de /vn_hist.bin ──
static void testHistWeather() {
  settingsDefaults(g_set);
  static Model m;
  CHECK(m.init(), "init historique météo");
  const int64_t t0 = 1791842400 + 10 * 3600;                   // mardi 13/10 10:00
  InfoRec ir[3] = { mkInfo("1", 49.60f, 6.13f, 20), mkInfo("2", 49.61f, 6.14f, 20), mkInfo("3", 49.62f, 6.15f, 20) };
  m.applyInfo(ir, 3, t0);
  StatusRec sr[3] = { mkStat("1", 5, 20), mkStat("2", 10, 20), mkStat("3", 15, 20) };
  // 1. Météo reçue et fraîche : la tranche la porte (arrondis documentés)
  Weather w; w.valid = true; w.fetchedAt = t0 - 120; w.temp = 12.4f; w.rain = 1.26f; w.wind = 23.6f; w.code = 63;
  w.nfc = 1; w.fc[0].prob = 80;
  m.wx = w;
  m.applyStatus(sr, 3, t0);
  const NetSample & a = m.net[m.head];
  CHECK(netWxKnown(a) && a.wxTemp == 12 && a.wxRain == 13 && a.wxWind == 24 && a.wxCode == 63 && a.wxProb == 80,
        "tranche avec météo : 12 °C, 1,3 mm, 24 km/h, code 63, 80 %% (%d, %d, %d, %d, %d)", a.wxTemp, a.wxRain, a.wxWind, a.wxCode, a.wxProb);
  CHECK(m.wxUse == WXU_OK, "météo fraîche : utilisée (%d)", m.wxUse);
  // 2. Météo jamais reçue : inconnue, pas zéro
  m.wx = Weather();
  m.applyStatus(sr, 3, t0 + 300);
  CHECK(!netWxKnown(m.net[m.head]) && m.net[m.head].wxProb == WX_NA && m.wxUse == WXU_NONE, "météo absente : tranche « inconnue », pas 0 °C ni 0 mm");
  // 3. Météo périmée (plus de 3 périodes) : inconnue aussi
  w.fetchedAt = t0 + 600 - 3 * VN_WX_REFRESH_S - 1;
  m.wx = w;
  m.applyStatus(sr, 3, t0 + 600);
  CHECK(!netWxKnown(m.net[m.head]) && m.wxUse == WXU_STALE, "météo de plus de 45 min : ignorée (état %d)", m.wxUse);
  // 4. Trou de 30 min : les tranches comblées n'inventent pas de météo
  w.fetchedAt = t0 + 2700;
  m.wx = w;
  m.applyStatus(sr, 3, t0 + 2700);
  CHECK(netWxKnown(m.net[m.head]), "relevé après le trou : météo");
  int gapOk = 0;
  for (int b = 1; b <= 5; b++) { const NetSample & g = m.net[m.ringIndex(b)]; gapOk += g.avail == 0xFFFF && !netWxKnown(g); }
  CHECK(gapOk == 5, "5 tranches comblées : sans relevé et sans météo (%d)", gapOk);

  // 5. Aller-retour au format courant (v2)
  sim::g_fsDir = "/tmp/vn_test_hist";
  if (system("rm -rf /tmp/vn_test_hist") != 0) {}
  Model::Blob blob;
  CHECK(m.serialize(blob), "sérialisation v2");
  std::vector<uint8_t> hist(blob.hist, blob.hist + blob.histLen), prof(blob.prof, blob.prof + blob.profLen);
  CHECK(Model::writeSaved(blob), "écriture v2");
  const int64_t tl = t0 + 3000;
  static Model r2;
  r2.init();
  CHECK(r2.load(tl) && r2.count == m.count && r2.head == m.head, "relecture v2");
  CHECK(memcmp(r2.net, m.net, sizeof(NetSample) * VN_RING_SLOTS) == 0, "v2 : échantillons et météo identiques");
  uint16_t ver2; memcpy(&ver2, hist.data() + 4, 2);
  CHECK(ver2 == 2, "fichier écrit en version 2 (%d)", ver2);

  // 6. Migration : un fichier v1 (v1.0.8, échantillons de 24 octets) est relu, météo inconnue
  const size_t n = 3, hdr = hist.size() - n * 8 - (size_t)VN_RING_SLOTS * (8 + 32 + n);
  CHECK(hdr >= 32 && hdr <= 48, "en-tête de %zu octets", hdr);
  std::vector<uint8_t> v1(hist.begin(), hist.begin() + hdr + n * 8 + VN_RING_SLOTS * 8);
  uint16_t one = 1; memcpy(v1.data() + 4, &one, 2);
  const uint8_t * src = hist.data() + hdr + n * 8 + VN_RING_SLOTS * 8;
  for (int r = 0; r < VN_RING_SLOTS; r++) v1.insert(v1.end(), src + r * 32, src + r * 32 + 24);   // champs v1 = 24 premiers octets
  v1.insert(v1.end(), src + VN_RING_SLOTS * 32, (const uint8_t *)hist.data() + hist.size());                   // lignes de vélos
  CHECK(v1.size() == hist.size() - VN_RING_SLOTS * 8, "fichier v1 : 8 octets de moins par tranche");
  auto loadWith = [&](const std::vector<uint8_t> & bytes, Model & mm) {
    plat::fsWrite("/vn_hist.bin", bytes.data(), bytes.size());
    mm.init();
    return mm.load(tl);
  };
  static Model r1;
  CHECK(loadWith(v1, r1) && r1.count == m.count && r1.head == m.head, "v1 relu (%d tranches)", r1.count);
  bool sameCore = true, anyWx = false;
  for (int r = 0; r < VN_RING_SLOTS; r++) {
    const NetSample & x = r1.net[r], & y = m.net[r];
    if (x.t != y.t || x.avail != y.avail || x.docks != y.docks || x.dep != y.dep || x.empty != y.empty) sameCore = false;
    if (netWxKnown(x) || x.wxWind != WX_NA || x.wxCode != WX_NA || x.wxProb != WX_NA) anyWx = true;
  }
  CHECK(sameCore, "v1 → v2 : relevés, retraits, ruptures conservés");
  CHECK(!anyWx, "v1 → v2 : météo « inconnue » partout (rien d'inventé)");
  CHECK(memcmp(r1.ring, m.ring, (size_t)VN_RING_SLOTS * VN_MAX_ST) == 0, "v1 → v2 : vélos par station conservés");
  // la carte continue d'écrire l'historique migré sans erreur
  r1.wx = w; r1.wx.fetchedAt = tl;
  r1.applyStatus(sr, 3, tl);
  CHECK(netWxKnown(r1.net[r1.head]) && r1.count == m.count + 1, "après migration : nouvelle tranche avec météo");

  // 7. Fichiers refusés proprement : tronqué, version future, magic faux, en-tête incohérent
  auto rejected = [&](std::vector<uint8_t> bytes, const char * what) {
    static Model z;
    bool any = loadWith(bytes, z);
    // le profil v1 (inchangé) peut se charger : on vérifie que l'historique, lui, est ignoré
    CHECK(z.count == 0 && z.head == -1, "%s : historique ignoré (any=%d, %d tranches)", what, any, z.count);
  };
  std::vector<uint8_t> t1 = v1; t1.pop_back();                       rejected(t1, "v1 tronqué d'un octet");
  std::vector<uint8_t> t2 = hist; t2.resize(t2.size() / 2);         rejected(t2, "v2 coupé en deux");
  std::vector<uint8_t> t3 = hist; uint16_t three = 3; memcpy(t3.data() + 4, &three, 2); rejected(t3, "version 3 (future)");
  std::vector<uint8_t> t4 = hist; t4[0] ^= 0xFF;                     rejected(t4, "magic faux");
  std::vector<uint8_t> t5 = hist; int32_t bad = VN_RING_SLOTS + 7; memcpy(t5.data() + 8, &bad, 4); rejected(t5, "tête hors anneau");
  std::vector<uint8_t> t6 = v1; uint16_t two = 2; memcpy(t6.data() + 4, &two, 2); rejected(t6, "taille v1 annoncée v2");
  std::vector<uint8_t> t7(hist.size(), 0xA5);                        rejected(t7, "fichier rempli de bruit");
  std::vector<uint8_t> t8(10, 0);                                    rejected(t8, "fichier plus court qu'un en-tête");
}

// ── Coefficient météo appris (étape 3a) ──────────────────────────────
// Monde déterministe : 4 stations (motifs de testModel), 7 semaines ; pluie de 2 mm/h
// les jours pairs de 7 h à 11 h et de 16 h à 20 h. Avec `effect`, la pluie freine le
// mouvement de la station de 60 % ; sans, elle ne change rien.
static bool wxRainAt(int64_t t, int64_t T0) {
  struct tm lt; plat::localTime(t, &lt);
  int day = (int)((t - T0) / 86400);
  return day % 2 == 0 && ((lt.tm_hour >= 7 && lt.tm_hour < 11) || (lt.tm_hour >= 16 && lt.tm_hour < 20));
}
static Weather wxFor(bool rain, int64_t t) {
  Weather w; w.valid = true; w.fetchedAt = t; w.temp = 14; w.wind = 8; w.code = rain ? 63 : 1; w.rain = rain ? 2.f : 0.f;
  w.nfc = 6;
  for (int i = 0; i < 6; i++) { w.fc[i].h = (int8_t)i; w.fc[i].temp = 14; w.fc[i].wind = 8; w.fc[i].rain = w.rain; w.fc[i].prob = rain ? 90 : 5; w.fc[i].code = (int16_t)w.code; }
  wxScore(w);
  return w;
}
static void wxWorld(Model & m, InfoRec * ir, float effect, int days, int64_t T0) {
  ir[0] = mkInfo("1", 49.60f, 6.13f, 20); ir[1] = mkInfo("2", 49.61f, 6.14f, 20);
  ir[2] = mkInfo("3", 49.62f, 6.15f, 20); ir[3] = mkInfo("4", 49.63f, 6.16f, 40);
  m.applyInfo(ir, 4, T0);
  static const int CAP[4] = { 20, 20, 20, 40 };
  float lvl[4];
  for (int s = 0; s < 4; s++) lvl[s] = (float)patternAt(s, T0);
  StatusRec sr[4];
  for (int64_t t = T0; t < T0 + (int64_t)days * 86400; t += 300) {
    bool rain = wxRainAt(t, T0);
    struct tm lt; plat::localTime(t, &lt);
    for (int s = 0; s < 4; s++) {
      float d = (float)(patternAt(s, t) - patternAt(s, t - 300));
      lvl[s] += d * (rain ? 1.f - effect : 1.f);
      if (lt.tm_hour == 3 && lt.tm_min == 0) lvl[s] = (float)patternAt(s, t);    // régulation de nuit
      lvl[s] = fminf((float)CAP[s], fmaxf(0.f, lvl[s]));
      sr[s] = mkStat(ir[s].sid, (int)lroundf(lvl[s]), CAP[s]);
    }
    m.wx = wxFor(rain, t);
    m.applyStatus(sr, 4, t);
  }
}
static void testWxLearn() {
  settingsDefaults(g_set);
  g_pred = PredictOpts();
  const int64_t T0 = 1791842400 - 86400 * 49;                  // mardi 25/08/2026
  CHECK(g_pred.wxFlow, "levier 3a actif par défaut (seul levier météo livré)");
  // 1. La pluie freine vraiment : α appris, positif, borné
  static Model m;
  CHECK(m.init(), "init météo apprise");
  InfoRec ir[4];
  wxWorld(m, ir, 0.6f, 49, T0);
  printf("  α appris %.3f (brut %.3f ± %.3f, origine %d, %.0f fenêtres pluvieuses)\n", m.wxAlpha, m.wxAlphaRaw, m.wxAlphaSE, m.wxSrc, m.wxG.nRain);
  CHECK(m.wxSrc == WXC_LEARNED && m.wxAlpha > 0.5f && m.wxAlpha <= 1.f, "effet réel : α appris > 0,5 et ≤ 1 (%.3f, origine %d)", m.wxAlpha, m.wxSrc);
  // Mercredi 14/10 06:55, il pleut : la station 1 doit se vider MOINS que le profil ne le dit
  const int64_t tq = T0 + 50 * 86400 + 6 * 3600 + 55 * 60;
  StatusRec sr[4];
  for (int s = 0; s < 4; s++) sr[s] = mkStat(ir[s].sid, patternAt(s, tq), s == 3 ? 40 : 20);
  m.wx = wxFor(true, tq);
  m.applyStatus(sr, 4, tq);
  const Station & a = m.st[0];
  float profDrop = 0, pn;
  { float p0, p1, n0; m.profileAt(0, tq, p0, n0); m.profileAt(0, tq + 3600, p1, pn); profDrop = p1 - p0; }
  printf("  pluie 06:55 : st1 %d → +1 h %.1f, dont météo %+.1f (profil seul %+.1f)\n", a.bikes, a.fc[12], a.fcWx[12], profDrop);
  CHECK(profDrop < -3 && a.fcWx[12] > 1.f, "pluie : part météo positive (%.1f) contre une baisse du profil (%.1f)", a.fcWx[12], profDrop);
  CHECK(a.fc[12] > a.bikes + profDrop + 1.f, "pluie : la station se vide moins que le profil seul");
  CHECK(fabsf(a.fcWx[0]) < 1e-6f && a.fc[0] == a.bikes, "point 0 = relevé réel, sans part météo");
  bool bounded = true;
  for (int k = 0; k < VN_FC_STEPS; k++) bounded &= fabsf(a.fcWx[k]) <= 0.4f * a.capEff() + 1e-4f;
  CHECK(bounded, "part météo bornée à 40 %% de la capacité utile");
  // Beau temps : aucune correction (x = 0 n'est pas pire que la normale)
  m.wx = wxFor(false, tq + 60);
  m.applyStatus(sr, 4, tq + 60);
  float mx = 0;
  for (int k = 0; k < VN_FC_STEPS; k++) mx = fmaxf(mx, fabsf(m.st[0].fcWx[k]));
  CHECK(mx == 0, "beau temps : part météo nulle partout (max %.3f)", mx);
  // 2. Météo indisponible ou périmée : prévision STRICTEMENT identique au modèle sans levier
  for (int mode = 0; mode < 2; mode++) {
    if (mode == 0) m.wx = Weather(); else { m.wx = wxFor(true, tq); m.wx.fetchedAt = tq + 120 - 3 * VN_WX_REFRESH_S - 1; }
    m.applyStatus(sr, 4, tq + 120);
    float on[VN_FC_STEPS]; memcpy(on, m.st[0].fc, sizeof(on));
    g_pred.wxFlow = false;
    m.recompute(tq + 120);
    bool same = memcmp(on, m.st[0].fc, sizeof(on)) == 0;
    g_pred.wxFlow = true;
    m.recompute(tq + 120);
    CHECK(same && m.wxUse == (mode ? WXU_STALE : WXU_NONE), "météo %s : prévision identique octet pour octet au modèle sans météo",
          mode ? "périmée" : "absente");
  }
  // 3. Courbe 24 h : même règle, part météo seulement sur l'horizon connu (2 h avec la météo actuelle)
  m.wx = wxFor(true, tq);
  m.applyStatus(sr, 4, tq + 180);
  float o24[97], c24[97], w24[97];
  m.project(0, tq + 180, 15, 97, o24, c24, w24);
  CHECK(fabsf(w24[4] - m.st[0].fcWx[12]) < 1e-3f, "24 h à +1 h : même part météo que la fiche (%.2f / %.2f)", w24[4], m.st[0].fcWx[12]);
  bool flatAfter = true;
  for (int k = 9; k < 97; k++) flatAfter &= fabsf(w24[k] - w24[8]) < 1e-4f;
  CHECK(!flatAfter, "au-delà de 2 h : la pluie prévue (6 h annoncées) continue de corriger");
  bool flat6 = true;
  for (int k = 25; k < 97; k++) flat6 &= fabsf(w24[k] - w24[24]) < 1e-4f;
  CHECK(flat6, "au-delà des 6 h annoncées : plus de correction supplémentaire (météo inconnue)");
  // Sans le levier 4 : la météo actuelle ne vaut que pour 2 h
  g_pred.wxForecast = false;
  m.project(0, tq + 180, 15, 97, o24, c24, w24);
  bool flat2 = true;
  for (int k = 9; k < 97; k++) flat2 &= fabsf(w24[k] - w24[8]) < 1e-4f;
  g_pred.wxForecast = true;
  CHECK(flat2, "levier 4 coupé : aucune correction au-delà de 2 h");
  // Reprise après la pluie : jeudi 05:00, il pleut ; pluie annoncée jusqu'à 9 h, sec ensuite.
  // ≤ 2 h (5 h–7 h) : météo actuelle ; 7 h–9 h, vague de départs de la station 1 : la pluie
  // ANNONCÉE freine la vague ; à partir de 9 h : plus aucune correction (la pluie a cessé).
  {
    const int64_t t5 = T0 + 51 * 86400 + 5 * 3600;
    for (int s2 = 0; s2 < 4; s2++) sr[s2] = mkStat(ir[s2].sid, patternAt(s2, t5), s2 == 3 ? 40 : 20);
    Weather r = wxFor(true, t5);
    for (int i = 4; i < 6; i++) { r.fc[i].rain = 0; r.fc[i].prob = 5; r.fc[i].code = 1; }
    m.wx = r;
    m.applyStatus(sr, 4, t5);
    float rv[97], rc[97], rw[97];
    m.project(0, t5, 15, 97, rv, rc, rw);
    float p7, p9, nn;
    m.profileAt(0, t5 + 2 * 3600, p7, nn); m.profileAt(0, t5 + 4 * 3600, p9, nn);
    printf("  reprise : part météo à 7 h %+.2f, à 9 h %+.2f (profil 7 h→9 h %+.1f), à 11 h %+.2f\n", rw[8], rw[16], p9 - p7, rw[24]);
    CHECK(rw[16] - rw[8] > 1.f, "pluie annoncée 7 h–9 h : la vague de départs est freinée (%+.2f)", rw[16] - rw[8]);
    bool after = true;
    for (int k = 17; k < 97; k++) after &= fabsf(rw[k] - rw[16]) < 1e-4f;
    CHECK(after, "reprise après la pluie : aucune correction supplémentaire une fois la pluie annoncée terminée");
    r.fc[2].rain = r.fc[3].rain = 0; r.fc[2].prob = r.fc[3].prob = 5; r.fc[2].code = r.fc[3].code = 1;
    m.wx = r;
    m.recompute(t5);
    m.project(0, t5, 15, 97, rv, rc, rw);
    CHECK(fabsf(rw[16] - rw[8]) < 1e-4f, "même matin, pluie annoncée seulement jusqu'à 7 h : la vague n'est pas freinée");
  }
  // 4. La pluie ne change rien : le modèle ne doit RIEN inventer
  static Model z;
  CHECK(z.init(), "init pluie sans effet");
  InfoRec iz[4];
  wxWorld(z, iz, 0.f, 49, T0);
  printf("  pluie sans effet : α %.3f (brut %.3f ± %.3f, origine %d)\n", z.wxAlpha, z.wxAlphaRaw, z.wxAlphaSE, z.wxSrc);
  CHECK(z.wxAlpha == 0 && z.wxSrc == WXC_NULL, "pluie sans effet : α = 0, origine « sans effet mesurable » (%.3f, %d)", z.wxAlpha, z.wxSrc);
  for (int s = 0; s < 4; s++) sr[s] = mkStat(iz[s].sid, patternAt(s, tq), s == 3 ? 40 : 20);
  z.wx = wxFor(true, tq);
  z.applyStatus(sr, 4, tq);
  CHECK(z.st[0].fcWx[12] == 0, "pluie sans effet : aucune part météo dans la prévision");
  // 5. Trop peu de pluie : origine « défaut »
  static Model y;
  CHECK(y.init(), "init défaut");
  InfoRec iy[4];
  wxWorld(y, iy, 0.6f, 9, T0);
  CHECK(y.wxSrc == WXC_DEFAULT && y.wxAlpha == 0, "9 jours (profil pas mûr) : origine « défaut », α = 0 (%d)", y.wxSrc);

  // 6. Persistance : /vn_prof.bin v2 garde l'apprentissage ; v1 se relit sans lui ; NaN refusé
  sim::g_fsDir = "/tmp/vn_test_wx";
  if (system("rm -rf /tmp/vn_test_wx") != 0) {}
  Model::Blob b;
  CHECK(m.serialize(b), "sérialisation profils v2");
  std::vector<uint8_t> prof(b.prof, b.prof + b.profLen);
  Model::writeSaved(b);
  static Model r;
  r.init();
  r.load(tq + 300);
  r.recompute(tq + 300);
  CHECK(memcmp(&r.wxG, &m.wxG, sizeof(WxStats)) == 0 && r.wxAlpha == m.wxAlpha && r.wxSrc == WXC_LEARNED,
        "v2 relu : statistiques et α identiques (%.3f)", r.wxAlpha);
  CHECK(memcmp(&r.wxClim, &m.wxClim, sizeof(WxClimate)) == 0 && memcmp(r.wxS, m.wxS, sizeof(WxStats) * 4) == 0, "v2 relu : x̄ par heure et statistiques par station");
  const size_t wxLen = sizeof(WxClimate) + sizeof(WxStats) * 5, dsLen = sizeof(int64_t) * 4;
  std::vector<uint8_t> v1 = legacyProf(prof, 4, 1);
  plat::fsWrite("/vn_prof.bin", v1.data(), v1.size());
  static Model r1;
  r1.init();
  r1.load(tq + 300);
  r1.recompute(tq + 300);
  CHECK(sameMeans(r1, m, 4) && r1.wxSrc == WXC_DEFAULT && r1.wxG.sn == 0,
        "profils v1 (v1.0.8) relus, apprentissage météo reparti de zéro");
  std::vector<uint8_t> bad = prof;
  float nanv = NAN; memcpy(bad.data() + (prof.size() - dsLen - wxLen) + sizeof(WxClimate), &nanv, 4);
  plat::fsWrite("/vn_prof.bin", bad.data(), bad.size());
  static Model r2;
  r2.init();
  r2.load(tq + 300);
  r2.recompute(tq + 300);
  CHECK(memcmp(r2.prof, m.prof, sizeof(ProfCell) * 4 * 7 * 96) == 0 && r2.wxG.sn == 0 && r2.wxAlpha == 0,
        "statistiques météo non finies : ignorées, profils gardés");
  // Textes de l'écran : chaque origine est nommée, la part affichée est celle calculée
  {
    char b2[256];
    m.wx = wxFor(true, tq + 400);
    for (int s2 = 0; s2 < 4; s2++) sr[s2] = mkStat(ir[s2].sid, patternAt(s2, tq + 400), s2 == 3 ? 40 : 20);
    m.applyStatus(sr, 4, tq + 400);
    wxPartText(m, m.st[0].fcWx[12], false, tq + 400 + 3600, b2, sizeof(b2));
    char want[32]; snprintf(want, sizeof(want), "dont %+d v", (int)lroundf(m.st[0].fcWx[12]));
    CHECK(strstr(b2, want) && strstr(b2, "m\xC3\xA9t\xC3\xA9o actuelle") && strstr(b2, "coefficient appris 1,00"),
          "fiche : « %s »", b2);
    wxPartText(m, -4.2f, true, tq + 400 + 4 * 3600, b2, sizeof(b2));
    CHECK(strstr(b2, "dont -4 v\xC3\xA9los") && strstr(b2, "m\xC3\xA9t\xC3\xA9o annonc\xC3\xA9" "e"), "24 h : « %s »", b2);
    wxPartText(m, 0.2f, false, tq + 400, b2, sizeof(b2));
    CHECK(strstr(b2, "aucun effet notable") != nullptr, "part < 0,5 vélo : « %s »", b2);
    wxPartText(z, 0, false, tq, b2, sizeof(b2));
    CHECK(strstr(b2, "non mesurable") != nullptr, "pluie sans effet : « %s »", b2);
    wxPartText(y, 0, false, tq, b2, sizeof(b2));
    CHECK(strstr(b2, "pas encore appris") != nullptr, "défaut : « %s »", b2);
    m.wx = Weather(); m.recompute(tq + 400);
    wxPartText(m, 0, false, tq, b2, sizeof(b2));
    CHECK(strstr(b2, "indisponible") && strstr(b2, "sans m\xC3\xA9t\xC3\xA9o"), "météo absente : « %s »", b2);
    m.wx = wxFor(true, tq); m.recompute(tq + 400 + 3 * VN_WX_REFRESH_S);
    wxPartText(m, 0, false, tq, b2, sizeof(b2));
    CHECK(strstr(b2, "plus de 45 min") != nullptr, "météo périmée : « %s »", b2);
    Provenance pv;
    CHECK(provenance(m, KPI_FC60, pv) && strstr(pv.rule, "Part m\xC3\xA9t\xC3\xA9o") && strstr(pv.detail, "p\xC3\xA9rim\xC3\xA9" "e"),
          "provenance +1 h : règle et état de la météo (« %s »)", pv.detail);
  }
  // 7. Coefficient borné : statistiques extrêmes → α plafonné à 1 (et à −0,5)
  static Model q;
  q.init();
  q.wxG.sdd = 100; q.wxG.szz = 1; q.wxG.sdz = 0; q.wxG.sdr = 0; q.wxG.szr = -50; q.wxG.sgg = 0.01f; q.wxG.sn = 500; q.wxG.nRain = 100;
  q.updateWxCoef();
  CHECK(q.wxAlpha == 1.f && q.wxAlphaRaw > 40, "α brut 50 → α appliqué borné à 1 (%.2f)", q.wxAlpha);
  q.wxG.szr = 50;
  q.updateWxCoef();
  CHECK(q.wxAlpha == -0.5f, "α brut −50 → borné à −0,5 (%.2f)", q.wxAlpha);
  q.wxG.szr = -0.02f; q.wxG.sgg = 1;
  q.updateWxCoef();
  CHECK(q.wxAlpha == 0 && q.wxSrc == WXC_NULL, "α brut sous 3 erreurs-types → 0, « sans effet mesurable »");
}

// Atténuation nocturne : 23 h – 4 h seulement, bornes exactes (heure de Luxembourg)
static int64_t lux(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  tzset();
  return (int64_t)mktime(&t);
}
// ── Jours fériés (étape 5a-ii) : calendrier, profil du dimanche ──────
static int holTruth(int64_t t) {
  struct tm lt; plat::localTime(t, &lt);
  bool hol = isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  return (hol || lt.tm_wday == 0) ? 3 : lt.tm_wday == 6 ? 8 : 15;
}
static void testHolidays() {
  int em, ed;
  easterDate(2026, em, ed); CHECK(em == 4 && ed == 5, "Pâques 2026 : 5 avril (%d/%d)", ed, em);
  easterDate(2027, em, ed); CHECK(em == 3 && ed == 28, "Pâques 2027 : 28 mars (%d/%d)", ed, em);
  easterDate(2025, em, ed); CHECK(em == 4 && ed == 20, "Pâques 2025 : 20 avril (%d/%d)", ed, em);
  easterDate(2038, em, ed); CHECK(em == 4 && ed == 25, "Pâques 2038 : 25 avril, la plus tardive (%d/%d)", ed, em);
  static const int YES[][3] = { { 2026, 1, 1 }, { 2026, 4, 6 }, { 2026, 5, 1 }, { 2026, 5, 9 }, { 2026, 5, 14 }, { 2026, 5, 25 },
                                { 2026, 6, 23 }, { 2026, 8, 15 }, { 2026, 11, 1 }, { 2026, 12, 25 }, { 2026, 12, 26 },
                                { 2027, 3, 29 }, { 2027, 5, 6 }, { 2027, 5, 17 }, { 2024, 5, 9 }, { 2024, 5, 20 } };
  static const int NO[][3] = { { 2026, 4, 3 }, { 2026, 4, 5 }, { 2026, 12, 24 }, { 2026, 12, 31 }, { 2026, 5, 15 },
                               { 2026, 6, 22 }, { 2026, 7, 14 }, { 2026, 11, 11 }, { 2027, 1, 2 }, { 2026, 5, 24 } };
  int ok = 0;
  for (auto & d : YES) ok += isPublicHoliday(d[0], d[1], d[2]);
  CHECK(ok == 16, "16 jours fériés reconnus (Pâques, Ascension, Pentecôte, 9 mai, 23 juin… 2024–2027) : %d", ok);
  int ko = 0;
  for (auto & d : NO) ko += isPublicHoliday(d[0], d[1], d[2]);
  CHECK(ko == 0, "Vendredi saint, dimanche de Pâques, 24 et 31 décembre, 14 juillet… ne sont pas fériés : %d faux positif(s)", ko);

  // Mini-monde : semaine 15 vélos, samedi 8, dimanche et fériés 3 ; appris du 07/12/2026 au 31/12/2026
  // (Noël et Saint-Étienne compris, appris comme des dimanches)
  settingsDefaults(g_set);
  g_pred = PredictOpts();
  CHECK(g_pred.holidaySunday, "levier 5a-ii actif par défaut");
  static Model m;
  CHECK(m.init(), "init fériés");
  const int64_t T0 = lux(2026, 12, 7, 0, 0);
  InfoRec ir[1] = { mkInfo("1", 49.6f, 6.13f, 20) };
  m.applyInfo(ir, 1, T0);
  StatusRec sr[1];
  for (int64_t t = T0; t < lux(2027, 1, 1, 0, 0); t += 300) { sr[0] = mkStat("1", holTruth(t), 20); m.applyStatus(sr, 1, t); }
  float p, n;
  CHECK(m.profileAt(0, lux(2026, 12, 30, 10, 0), p, n) && fabsf(p - 15) < 0.6f, "mercredi ordinaire : profil de semaine (%.1f)", p);
  CHECK(m.profileAt(0, lux(2027, 1, 1, 10, 0), p, n) && fabsf(p - 3) < 0.6f, "vendredi 1er janvier : profil du dimanche (%.1f)", p);
  // Projection depuis le 1er janvier 20:00 : le lendemain est un SAMEDI ordinaire (pas « lundi » ni dimanche)
  const int64_t t1 = lux(2027, 1, 1, 20, 0);
  sr[0] = mkStat("1", 3, 20);
  m.applyStatus(sr, 1, t1);
  float o[97], c[97];
  m.project(0, t1, 15, 97, o, c);
  // samedi = 8 vélos, confiance c (2 samedis appris : le 26/12 férié compte pour un dimanche) ;
  // l'erreur à éviter : « dimanche + 1 = lundi » donnerait 3 + c·(15 − 3)
  printf("  1er janvier 20:00 : 3 vélos → samedi 10:00 %.1f (attendu 3 + c·5 = %.1f ; lundi par erreur : %.1f)\n",
         o[56], 3 + c[56] * 5, 3 + c[56] * 12);
  CHECK(fabsf(o[56] - (3 + c[56] * 5)) < 0.3f && c[56] > 0.5f, "lendemain d'un férié : profil de SON jour (samedi), pas « dimanche + 1 » (%.1f)", o[56]);
  CHECK(m.st[0].anom == AN_OFF_HOURS || m.st[0].anom == AN_NORMAL, "férié calme hors heures : pas d'alerte");
  // Férié en journée : 3 vélos sont l'habitude d'un dimanche → silence ; sans le levier → « en dessous »
  static Model q;
  q.init();
  q.applyInfo(ir, 1, T0);
  for (int64_t t = T0; t < lux(2027, 1, 1, 10, 0); t += 300) { sr[0] = mkStat("1", holTruth(t), 20); q.applyStatus(sr, 1, t); }
  sr[0] = mkStat("1", 3, 20);
  q.applyStatus(sr, 1, lux(2027, 1, 1, 10, 0) + 60);
  CHECK(q.st[0].anom == AN_NORMAL && countAlerts(q, AL_UNUSUAL, -1) == 0, "1er janvier 10:00, 3 vélos : habituel pour un férié (état %d)", q.st[0].anom);
  g_pred.holidaySunday = false;
  q.recompute(lux(2027, 1, 1, 10, 0) + 60);
  CHECK(q.st[0].anom == AN_LOW, "sans le levier : la même valeur sonnerait « en dessous » (état %d)", q.st[0].anom);
  CHECK(!q.holidayProfile(lux(2027, 1, 1, 10, 0)), "sans le levier : l'écran ne dit pas « profil du dimanche »");
  g_pred.holidaySunday = true;
  q.recompute(lux(2027, 1, 1, 10, 0) + 60);
  CHECK(q.holidayProfile(lux(2027, 1, 1, 0, 0)) && q.holidayProfile(lux(2027, 1, 1, 23, 59)) && !q.holidayProfile(lux(2026, 12, 31, 23, 59))
        && !q.holidayProfile(lux(2027, 1, 2, 0, 0)), "provenance affichable : « profil du dimanche » du 1er janvier 00:00 à 23:59, pas la veille ni le lendemain");
}

// ── Fériés et récence (branche feat/feries-recense) ─────────────────
// Calendrier : Pâques et les trois fêtes mobiles sur plusieurs années (dates de contrôle calculées par un
// second algorithme indépendant — Knuth, TAOCP 1.3.2 — et, pour 2026 et 2027, la liste officielle de
// luxembourg.public.lu) ; noms ; collisions (Ascension le 1er mai en 2008, le 9 mai en 2024).
static void testCalendarYears() {
  static const int E[][7] = {   // année, Pâques (m, j), Ascension (m, j), lundi de Pentecôte (m, j)
    { 2000, 4, 23, 6, 1, 6, 12 }, { 2008, 3, 23, 5, 1, 5, 12 }, { 2011, 4, 24, 6, 2, 6, 13 },
    { 2019, 4, 21, 5, 30, 6, 10 }, { 2024, 3, 31, 5, 9, 5, 20 }, { 2025, 4, 20, 5, 29, 6, 9 },
    { 2026, 4, 5, 5, 14, 5, 25 }, { 2027, 3, 28, 5, 6, 5, 17 }, { 2028, 4, 16, 5, 25, 6, 5 },
    { 2030, 4, 21, 5, 30, 6, 10 }, { 2038, 4, 25, 6, 3, 6, 14 }, { 2285, 3, 22, 4, 30, 5, 11 } };
  int okE = 0, okM = 0;
  for (auto & e : E) {
    int m, d;
    easterDate(e[0], m, d);
    okE += m == e[1] && d == e[2];
    // lundi de Pâques = lendemain (passage de mois compris : 31/3 → 1/4 en 2024)
    int lm = e[1], ld = e[2] + 1;
    if (lm == 3 && ld == 32) { lm = 4; ld = 1; }
    const char * a = publicHolidayName(e[0], lm, ld), * b = publicHolidayName(e[0], e[3], e[4]), * c = publicHolidayName(e[0], e[5], e[6]);
    bool collide = (e[3] == 5 && (e[4] == 1 || e[4] == 9));   // l'Ascension tombe sur un férié fixe : le nom fixe l'emporte
    okM += a && !strcmp(a, "lundi de P\xC3\xA2ques") && b && (collide || !strcmp(b, "Ascension")) && c && !strcmp(c, "lundi de Pentec\xC3\xB4te");
    okM -= isPublicHoliday(e[0], e[1], e[2]);                  // le dimanche de Pâques n'est pas férié
  }
  CHECK(okE == 12, "Pâques juste sur 12 années de 2000 à 2285, dont la plus précoce (22 mars) et des plus tardives (%d/12)", okE);
  CHECK(okM == 12, "lundi de Pâques, Ascension, lundi de Pentecôte nommés sur les 12 années, dimanche de Pâques non férié (%d/12)", okM);
  // Les 11 fériés de 2026 et 2027, tels que luxembourg.public.lu les publie
  static const int OFF[][3] = { { 2026, 1, 1 }, { 2026, 4, 6 }, { 2026, 5, 1 }, { 2026, 5, 9 }, { 2026, 5, 14 }, { 2026, 5, 25 },
    { 2026, 6, 23 }, { 2026, 8, 15 }, { 2026, 11, 1 }, { 2026, 12, 25 }, { 2026, 12, 26 },
    { 2027, 1, 1 }, { 2027, 3, 29 }, { 2027, 5, 1 }, { 2027, 5, 9 }, { 2027, 5, 6 }, { 2027, 5, 17 },
    { 2027, 6, 23 }, { 2027, 8, 15 }, { 2027, 11, 1 }, { 2027, 12, 25 }, { 2027, 12, 26 } };
  int okO = 0;
  for (auto & d : OFF) okO += isPublicHoliday(d[0], d[1], d[2]);
  CHECK(okO == 22, "liste officielle 2026 et 2027 : 22/22 (%d)", okO);
  // Nombre de jours fériés par année : 11, sauf collision (10 jours distincts en 2008 et en 2024)
  auto count = [](int y) {
    static const int ML[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int c = 0;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    for (int m = 1; m <= 12; m++) for (int d = 1; d <= ML[m - 1] + (m == 2 && leap); d++) c += isPublicHoliday(y, m, d);
    return c;
  };
  int c11 = 0;
  for (int y = 2025; y <= 2040; y++) c11 += count(y) == 11;
  CHECK(c11 == 16 && count(2024) == 10 && count(2008) == 10, "11 fériés par an de 2025 à 2040 (%d/16), 10 jours distincts en 2024 et 2008 (%d, %d)",
        c11, count(2024), count(2008));
  CHECK(!strcmp(publicHolidayName(2026, 12, 26), "Saint-\xC3\x89tienne") && !strcmp(publicHolidayName(2026, 6, 23), "F\xC3\xAAte nationale")
        && !publicHolidayName(2026, 2, 30) && !publicHolidayName(2026, 13, 1), "noms des fêtes fixes ; dates impossibles refusées");
}

// Levier « fériés » (a) : ce qu'un jour férié verse au profil, selon g_pred.holidayLearn
static void testHolidayLearn() {
  settingsDefaults(g_set);
  InfoRec ir[1] = { mkInfo("1", 49.6f, 6.13f, 20) };
  StatusRec sr[1];
  const int64_t T0 = lux(2026, 4, 27, 0, 0), H = lux(2026, 5, 14, 0, 0), H1 = lux(2026, 5, 15, 0, 0);   // 14/05/2026 : Ascension (jeudi)
  const int THU = 4 * 96 + 40, SUN = 0 * 96 + 40;                  // cellules jeudi et dimanche 10:00
  static const char * NAME[3] = { "son jour", "dimanche", "exclu" };
  for (int v = HL_DAY; v <= HL_SKIP; v++) {
    g_pred = PredictOpts();
    g_pred.holidayLearn = (uint8_t)v;
    static Model m;
    m = Model(); m.init();
    m.applyInfo(ir, 1, T0);
    for (int64_t t = T0; t < H; t += 300) { sr[0] = mkStat("1", 10, 20); m.applyStatus(sr, 1, t); }
    ProfCell thu = m.prof[THU], sun = m.prof[SUN];
    uint8_t cn = m.cellN[THU], cs = m.cellN[SUN];
    for (int64_t t = H; t < H1; t += 300) { sr[0] = mkStat("1", 2, 20); m.applyStatus(sr, 1, t); }
    bool thuMoved = m.prof[THU].a != thu.a || m.prof[THU].n != thu.n, sunMoved = m.prof[SUN].a != sun.a || m.prof[SUN].n != sun.n;
    bool cnMoved = m.cellN[THU] != cn || m.cellN[SUN] != cs;
    if (v == HL_DAY) CHECK(thuMoved && !sunMoved, "férié appris à « %s » : la case du jeudi bouge, pas celle du dimanche", NAME[v]);
    if (v == HL_SUNDAY) CHECK(!thuMoved && sunMoved && m.prof[SUN].n == sun.n + 3, "férié appris à « %s » : 3 relevés de plus au dimanche 10:00, jeudi intact", NAME[v]);
    if (v == HL_SKIP) CHECK(!thuMoved && !sunMoved && !cnMoved, "férié « %s » : ni le jeudi ni le dimanche ne bougent, progression non comptée", NAME[v]);
    // le lendemain (vendredi) s'apprend normalement quelle que soit la variante
    ProfCell fri = m.prof[5 * 96 + 40];
    for (int64_t t = H1; t < H1 + 86400; t += 300) { sr[0] = mkStat("1", 10, 20); m.applyStatus(sr, 1, t); }
    CHECK(m.prof[5 * 96 + 40].n == (fri.n + 3 > VN_PROFILE_DECAY_N ? VN_PROFILE_DECAY_N : fri.n + 3), "« %s » : le lendemain d'un férié s'apprend (vendredi 10:00)", NAME[v]);
  }
  g_pred = PredictOpts();
  CHECK(g_pred.holidayLearn == HL_SUNDAY && g_pred.holidaySunday, "réglage livré : férié appris et prévu comme un dimanche");
}

// Levier « fériés » (c) : tolérance élargie un jour férié, et seulement ce jour-là
static void testHolidayAlert() {
  settingsDefaults(g_set);
  g_pred = PredictOpts();
  g_alertOpt = AlertOpts();
  CHECK(g_alertOpt.holidayScale == 1.f, "réglage livré : alerte inchangée un jour férié");
  static Model q;
  q = Model(); q.init();
  InfoRec ir[1] = { mkInfo("1", 49.6f, 6.13f, 20) };
  StatusRec sr[1];
  const int64_t T0 = lux(2026, 11, 9, 0, 0);
  q.applyInfo(ir, 1, T0);
  const int64_t sun = lux(2026, 12, 20, 10, 0), hol = lux(2026, 12, 25, 10, 0);   // dimanche ordinaire ; Noël (vendredi)
  for (int64_t t = T0; t < sun; t += 300) { sr[0] = mkStat("1", holTruth(t), 20); q.applyStatus(sr, 1, t); }
  // 3 vélos attendus (profil du dimanche), 8 relevés : écart 5 > ± 4
  sr[0] = mkStat("1", 8, 20);
  g_alertOpt.holidayScale = 1.5f;
  q.applyStatus(sr, 1, sun + 60);
  CHECK(q.st[0].anom == AN_HIGH && q.st[0].anomTol == 4, "dimanche ordinaire, tolérance des fériés × 1,5 : ± %d inchangé, alerte (état %d)", q.st[0].anomTol, q.st[0].anom);
  for (int64_t t = sun + 300; t < hol; t += 300) { sr[0] = mkStat("1", holTruth(t), 20); q.applyStatus(sr, 1, t); }
  sr[0] = mkStat("1", 8, 20);
  g_alertOpt.holidayScale = 1.f;
  q.applyStatus(sr, 1, hol + 60);
  CHECK(q.st[0].anom == AN_HIGH && countAlerts(q, AL_UNUSUAL, -1) == 1, "Noël 10:00, 8 vélos contre 3 : alerte sans détente (état %d)", q.st[0].anom);
  g_alertOpt.holidayScale = 1.5f;
  q.recompute(hol + 60);
  CHECK(q.st[0].anom == AN_NORMAL && q.st[0].anomTol == 6 && countAlerts(q, AL_UNUSUAL, -1) == 0, "Noël, tolérance × 1,5 : ± %d, pas d'alerte (état %d)", q.st[0].anomTol, q.st[0].anom);
  g_alertOpt = AlertOpts();
}

// Levier « récence » : poids exacts, valeur exacte après k relevés, cas limites, fichiers d'une autre mémoire
static void feedCell(Model & m, int64_t day0, int weeks, int bikes) {
  // 3 relevés par semaine dans la cellule du jour à 10:00 (10:00, 10:05, 10:10)
  StatusRec sr[1];
  for (int w = 0; w < weeks; w++)
    for (int k = 0; k < 3; k++) { sr[0] = mkStat("1", bikes, 30); m.applyStatus(sr, 1, day0 + (int64_t)w * 7 * 86400 + 10 * 3600 + k * 300); }
}
static void testDecay() {
  CHECK(profWeight(0, 24) == 1.f && profWeight(1, 24) == 0.5f && profWeight(22, 24) == 1.f / 23 && profWeight(23, 24) == 1.f / 24
        && profWeight(24, 24) == 1.f / 24, "poids : 1, 1/2 … 1/23, puis 1/24 (montée puis décroissance)");
  CHECK(profWeight(30, 48) == 1.f / 31 && profWeight(31, 48) == 1.f / 48 && profCountMax(48) == 31, "N = 48 > 5 bits : n saturé à 31, puis 1/48");
  CHECK(profWeight(5, 1) == 0.5f && profWeight(5, 0) == 0.5f && profCountMax(1) == 2 && profCountMax(-3) == 2 && profWeight(-1, 24) == 1.f,
        "cas limites : N < 2 traité comme 2, n négatif comme 0");
  settingsDefaults(g_set);
  InfoRec ir[1] = { mkInfo("1", 49.6f, 6.13f, 30) };
  // mardi 6 avril 2027 : 20 semaines sans changement d'heure ni mardi férié
  const int64_t M0 = lux(2027, 4, 6, 0, 0);
  const int C = 2 * 96 + 40;
  for (int N : { 2, 6, 12, 24, 48 }) {
    g_pred = PredictOpts();
    g_pred.profDecayN = N;
    static Model m;
    m = Model(); m.init();
    m.applyInfo(ir, 1, M0);
    feedCell(m, M0, 12, 10);                               // 36 relevés de 10 : moyenne exacte, n saturé
    bool flat = m.prof[C].a == 10 * 256 && m.prof[C].n == profCountMax(N);
    // puis 16 vélos : a_k = 16 − 6·λ^k, λ = 1 − 1/N ; erreur d'arrondi bornée par N/512 vélo
    float worst = 0;
    for (int w = 1; w <= 6; w++) {
      feedCell(m, M0 + (int64_t)(12 + w - 1) * 7 * 86400, 1, 16);
      double expect = 16 - 6 * pow(1 - 1.0 / (N < 2 ? 2 : N), 3 * w);
      worst = fmaxf(worst, fabsf((float)(m.prof[C].a / 256.0 - expect)));
    }
    CHECK(flat && worst <= N / 512.f + 1 / 256.f, "N = %d : 10 constant → a = 10 exactement ; puis 16 : a = 16 − 6·λ^k à %.4f vélo près (borne %.4f)",
          N, worst, N / 512.f + 1 / 256.f);
  }
  // N = 24 : exactement la règle de la v1.0.9 (moyenne plafonnée à 24), relevé par relevé
  {
    g_pred = PredictOpts();
    static Model m;
    m = Model(); m.init();
    m.applyInfo(ir, 1, M0);
    uint16_t a = 0; int n = 0; bool same = true;
    StatusRec sr[1];
    uint32_t h = 12345;
    for (int w = 0; w < 30; w++) for (int k = 0; k < 3; k++) {
      h = h * 1103515245u + 12345u;
      int x = (int)((h >> 16) % 31);
      sr[0] = mkStat("1", x, 30);
      m.applyStatus(sr, 1, M0 + (int64_t)w * 7 * 86400 + 10 * 3600 + k * 300);
      if (n == 0) { a = (uint16_t)(x * 256); n = 1; }
      else { int nn = n < 24 ? n + 1 : 24; float v = a + (x * 256.f - a) / nn; a = (uint16_t)fminf(65535.f, fmaxf(0.f, v + 0.5f)); n = nn; }
      same &= m.prof[C].a == a && m.prof[C].n == n;
    }
    CHECK(same, "N = 24 (livré) : identique au bit près à la moyenne plafonnée de la v1.0.9 sur 90 relevés aléatoires");
  }
  // Un fichier écrit avec une mémoire de 24 relu avec une autre : format inchangé (v3), n relu tel quel,
  // puis ramené au plafond de la nouvelle mémoire au premier relevé
  {
    g_pred = PredictOpts();
    sim::g_fsDir = "/tmp/vn_test_decay";
    if (system("rm -rf /tmp/vn_test_decay") != 0) {}
    static Model m;
    m = Model(); m.init();
    m.applyInfo(ir, 1, M0);
    feedCell(m, M0, 10, 10);
    Model::Blob b;
    CHECK(m.serialize(b), "sérialisation (mémoire 24)");
    uint16_t ver; memcpy(&ver, b.prof + 4, 2);
    Model::writeSaved(b);
    const int64_t tl = M0 + 10 * 7 * 86400;
    for (int N : { 12, 48 }) {
      g_pred.profDecayN = N;
      static Model r;
      r = Model(); r.init(); r.load(tl); r.applyInfo(ir, 1, tl);
      bool same = r.prof[C].a == 10 * 256 && r.prof[C].n == 24;
      feedCell(r, tl, 1, 16);                                   // mardi suivant (tl est un mardi 00:00)
      double expect = N == 12 ? 16 - 6 * pow(11.0 / 12, 3) : 10 + (16 - 10) * (1 - (24.0 / 25) * (25.0 / 26) * (26.0 / 27));
      CHECK(ver == 3 && same && fabsf(r.prof[C].a / 256.f - (float)expect) < 0.02f && r.prof[C].n == (N == 12 ? 12 : 27),
            "fichier v3 écrit avec N = 24, relu avec N = %d : moyenne et n = 24 relus, puis %s (a = %.3f, attendu %.3f, n = %d)", N,
            N == 12 ? "poids 1/12 et n ramené à 12" : "la montée continue (1/25, 1/26, 1/27)", r.prof[C].a / 256.f, expect, r.prof[C].n);
    }
    // une v2 (cellules de 3 octets) relue avec N = 12 : n plafonné à 12 dès la lecture
    g_pred.profDecayN = 24;
    Model::Blob b2; m.serialize(b2);
    std::vector<uint8_t> v3(b2.prof, b2.prof + b2.profLen);
    plat::psFree(b2.prof); plat::psFree(b2.hist);
    std::vector<uint8_t> v2 = legacyProf(v3, 1, 2);
    plat::fsWrite("/vn_prof.bin", v2.data(), v2.size());
    g_pred.profDecayN = 12;
    static Model r2;
    r2 = Model(); r2.init();
    CHECK(r2.load(tl) && r2.prof[C].a == 10 * 256 && r2.prof[C].n == 12, "v2 relue avec N = 12 : moyenne intacte, n plafonné à 12 (%d)", r2.prof[C].n);
  }
  // Rajeunissement ciblé (mesuré, non livré) : seule la station dont la capacité annoncée change
  {
    g_pred = PredictOpts();
    CHECK(g_pred.rejuvN == 0, "réglage livré : pas de rajeunissement ciblé");
    static Model m;
    m = Model(); m.init();
    InfoRec two[2] = { mkInfo("1", 49.6f, 6.13f, 30), mkInfo("2", 49.61f, 6.13f, 30) };
    m.applyInfo(two, 2, M0);
    StatusRec sr[2];
    for (int w = 0; w < 10; w++) for (int k = 0; k < 3; k++) {
      sr[0] = mkStat("1", 10, 30); sr[1] = mkStat("2", 10, 30);
      m.applyStatus(sr, 2, M0 + (int64_t)w * 7 * 86400 + 10 * 3600 + k * 300);
    }
    const int64_t t1 = M0 + 10 * 7 * 86400;
    two[0].cap = 31; m.applyInfo(two, 2, t1);
    CHECK(m.prof[C].n == 24, "rajeunissement désactivé : capacité 30 → 31, n intact (%d)", m.prof[C].n);
    g_pred.rejuvN = 6;
    two[0].cap = 32; m.applyInfo(two, 2, t1);
    CHECK(m.prof[C].n == 24, "31 → 32 : une borne de plus, pas de rajeunissement (%d)", m.prof[C].n);
    two[0].cap = 45; m.applyInfo(two, 2, t1);
    CHECK(m.prof[C].n == 6 && m.prof[C].a == 10 * 256 && m.prof[672 + C].n == 24,
          "32 → 45 bornes : n ramené à 6 pour cette station seulement, moyenne gardée (%d, voisine %d)", m.prof[C].n, m.prof[672 + C].n);
    g_pred = PredictOpts();
  }
  g_pred = PredictOpts();
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

// ── Recherche Wi-Fi : liste construite à partir de bornes brutes synthétiques ──
static WifiRaw mkRaw(const char * ssid, int rssi, int ch = 6, bool secure = true) {
  WifiRaw r{};
  snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
  r.rssi = (int16_t)rssi; r.channel = (uint8_t)ch; r.secure = secure;
  return r;
}

static void testWifiScan() {
  static WifiList L;
  char nm[33];
  // 30 réseaux distincts (> les 16 de l'ancien tableau), dans l'ordre de balayage par
  // canal ; le plus fort arrive en 26ᵉ position. Aucun ne doit être perdu.
  {
    std::vector<WifiRaw> raw;
    for (int i = 0; i < 30; i++) {
      snprintf(nm, sizeof(nm), "Voisin-%02d", i);
      raw.push_back(mkRaw(nm, i == 25 ? -38 : -60 - (i % 30), 1 + i % 13));
    }
    buildWifiList(raw.data(), (int)raw.size(), L);
    CHECK(L.n == 30 && L.dropped == 0, "30 réseaux → 30 lignes, aucun écarté (%d lignes, %d écartés)", L.n, L.dropped);
    CHECK(L.n > 0 && strcmp(L.net[0].ssid, "Voisin-25") == 0 && L.net[0].rssi == -38,
          "le plus fort, vu en 26ᵉ, est en tête (« %s »)", L.n ? L.net[0].ssid : "");
    bool sorted = true;
    for (int i = 1; i < L.n; i++) if (L.net[i].rssi > L.net[i - 1].rssi) sorted = false;
    CHECK(sorted, "liste triée par RSSI décroissant");
  }
  // Plus de réseaux que VN_SCAN_MAX : seuls les plus faibles tombent, et on les compte.
  {
    std::vector<WifiRaw> raw;
    const int N = VN_SCAN_MAX + 20;
    for (int i = 0; i < N; i++) {              // RSSI croissant dans l'ordre de balayage : le pire cas
      snprintf(nm, sizeof(nm), "Dense-%03d", i);
      raw.push_back(mkRaw(nm, -95 + i));
    }
    buildWifiList(raw.data(), N, L);
    CHECK(L.n == VN_SCAN_MAX && L.dropped == 20, "%d réseaux → %d lignes + 20 écartés (%d, %d)", N, VN_SCAN_MAX, L.n, L.dropped);
    snprintf(nm, sizeof(nm), "Dense-%03d", N - 1);
    CHECK(strcmp(L.net[0].ssid, nm) == 0, "le plus fort (dernier balayé) reste en tête");
    CHECK(L.net[L.n - 1].rssi == -95 + 20, "la dernière ligne gardée est la plus forte des faibles (%d dBm)", L.net[L.n - 1].rssi);
  }
  // Maillage : un SSID, trois bornes, la plus faible vue en premier → une ligne, la plus forte.
  {
    WifiRaw raw[] = { mkRaw("Maison", -82, 1), mkRaw("Autre", -70, 6), mkRaw("Maison", -47, 11), mkRaw("Maison", -66, 6) };
    buildWifiList(raw, 4, L);
    int rows = 0, at = -1;
    for (int i = 0; i < L.n; i++) if (strcmp(L.net[i].ssid, "Maison") == 0) { rows++; at = i; }
    CHECK(rows == 1, "SSID vu par 3 bornes → 1 ligne (%d)", rows);
    CHECK(at >= 0 && L.net[at].rssi == -47 && L.net[at].channel == 11 && L.net[at].aps == 3,
          "RSSI le plus fort (-47, canal 11, 3 bornes) : %d, canal %d, %d bornes",
          at >= 0 ? L.net[at].rssi : 0, at >= 0 ? L.net[at].channel : 0, at >= 0 ? L.net[at].aps : 0);
    CHECK(L.n == 2 && at == 0, "« Maison » (-47) passe devant « Autre » (-70)");
  }
  // Réseaux masqués : comptés, jamais une ligne vide.
  {
    WifiRaw raw[] = { mkRaw("", -40, 3), mkRaw("Visible", -60), mkRaw("", -75, 9) };
    buildWifiList(raw, 3, L);
    bool blank = false;
    for (int i = 0; i < L.n; i++) if (!L.net[i].ssid[0]) blank = true;
    CHECK(!blank && L.n == 1 && L.hidden == 2, "2 masqués comptés, aucune ligne vide (%d lignes, %d masqués)", L.n, L.hidden);
    buildWifiList(raw, 1, L);
    CHECK(L.n == 0 && L.hidden == 1, "uniquement masqué → liste vide, 1 masqué");
    buildWifiList(raw, 0, L);
    CHECK(L.n == 0 && L.hidden == 0 && L.dropped == 0, "aucune borne → liste vide");
  }
  // SSID de 32 octets (maximum 802.11) : conservé entier et terminé.
  {
    WifiRaw r = mkRaw("0123456789abcdef0123456789ABCDEF", -50);
    buildWifiList(&r, 1, L);
    CHECK(L.n == 1 && strlen(L.net[0].ssid) == 32, "SSID de 32 octets intact");
  }
  // Codes du framework : ‑1 (WIFI_SCAN_RUNNING) se réessaie, ‑2 (WIFI_SCAN_FAILED) échoue.
  CHECK(scanVerdict(0, 0) == SCAN_OK && scanVerdict(12, 0) == SCAN_OK, "≥ 0 → terminé");
  CHECK(scanVerdict(-1, 0) == SCAN_RETRY, "‑1 à la 1ʳᵉ tentative → réessai");
  CHECK(scanVerdict(-2, 0) == SCAN_FAIL, "‑2 → échec immédiat, sans réessai");
  CHECK(scanVerdict(-1, 0) != scanVerdict(-2, 0), "‑1 et ‑2 traités différemment");
  int tries = 1;                                   // radio toujours « en cours » : boucle de doScan()
  for (int a = 0; scanVerdict(-1, a) == SCAN_RETRY; a++) tries++;
  CHECK(tries == VN_SCAN_TRIES && scanVerdict(-1, VN_SCAN_TRIES - 1) == SCAN_FAIL,
        "‑1 persistant → %d tentatives puis échec (%d)", VN_SCAN_TRIES, tries);
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
  testAlertCalm();
  testHistWeather();
  testWxLearn();
  testHolidays();
  testCalendarYears();
  testHolidayLearn();
  testHolidayAlert();
  testDecay();
  testTouchCal();
  testNightDim();
  testWifiScan();
  printf("\n%d vérifications réussies, %d échec(s)\n", s_ok, s_fail);
  return s_fail ? 1 : 0;
}
