// ── Banc de prévision sur DONNÉES RÉELLES (bench_prevision --reel) ────
// Rejoue en marche avant l'historique 5 min vidé de la carte (tools/carte_dump.py) à travers le
// VRAI modèle : à chaque tranche, le modèle ne voit que le passé (applyStatus), et ses prévisions
// sont comparées à ce qui est RÉELLEMENT arrivé ensuite — le futur est dans le fichier.
//
//   bench_prevision --reel <vn_hist.bin | dossier> <vn_prof.bin | -> [--stations F] [--depart profil|zero]
//                   [--avec-fuite] [--levier nom=v]… [--verite DOSSIER]
//
//   <dossier>      tous les vn_hist.bin qu'il contient (archive de vidages successifs), fusionnés par
//                  tranche : la carte ne garde que 24 h, des semaines se constituent en vidant chaque jour
//   <vn_prof.bin>  profils appris par la carte : point de départ du modèle (--depart profil, défaut)
//                  et objet de la vérification de forme (« les créneaux ont-ils la forme attendue ? »)
//   --stations F   capacités : vn_stations.csv du vidage ou station_information.json (défaut : les
//                  vn_stations.csv trouvés à côté des historiques, sinon docs/donnees)
//   --depart zero  le modèle part de rien et apprend pendant le rejeu (maturation mesurable)
//   --avec-fuite   rejoue aussi les tranches que le profil a déjà apprises (voir « fuite » ci-dessous)
//   --levier       variante comparée, appariée, à la référence (mêmes noms que le banc simulé)
//   --verite D     (chaîne simulée seulement) incidents et types vrais, pour juger la vérité construite
//
// Fuite : un profil vidé à l'instant T a déjà appris toutes les tranches ≤ T. Les rejouer mesure une
// erreur trop optimiste (le modèle a vu la réponse). Par défaut, seules les tranches postérieures au
// profil sont rejouées ; si aucune ne l'est (un seul vidage : historique et profil du même instant),
// toutes le sont, et le rapport le dit en tête.
//
// Vérité des incidents (le modèle évalué n'y entre jamais) : un SAUT franc et durable par rapport à une
// référence a posteriori — moyenne des AUTRES jours rejoués de même type (ouvré / samedi / dimanche),
// même station, même quart d'heure (≥ 3 jours), sinon le profil de départ : |Δ15 min observé − Δ15 min
// de référence| ≥ max(5 vélos, 4 × le saut médian de la station), encore décalé d'au moins la moitié
// 30 min plus tard, hors sortie de rupture (station vide ou pleine juste avant : un rééquilibrage).
// Fenêtre de vérité 3 h. Jours fériés exclus (leur référence est justement en question). Calibré sur
// la chaîne simulée : 94 % des incidents injectés retrouvés (docs/mesures-prevision.md).
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/weather.h"
#include "../src/core/gbfs.h"
#include "../src/core/calendar.h"
#include "../src/core/days.h"
#include "../src/core/dump.h"
#include "../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

namespace reel {

static const int NH = 5;
static const int HK[NH] = { 3, 6, 12, 24, 48 };
static const char * HL[NH] = { "+15 min", "+30 min", "+60 min", "+120 min", "+240 min" };
static const int NP = 49;
static const char * TMP_FS = "/tmp/vn_reel_fs";

// ── Petits outils ────────────────────────────────────────────────────
static std::vector<uint8_t> readFile(const std::string & p) {
  std::vector<uint8_t> v;
  FILE * f = fopen(p.c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n > 0) { v.resize(n); if (fread(v.data(), 1, n, f) != (size_t)n) v.clear(); }
  fclose(f);
  return v;
}
static bool writeFile(const std::string & p, const std::vector<uint8_t> & v) {
  FILE * f = fopen(p.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(v.data(), 1, v.size(), f) == v.size();
  fclose(f);
  return ok;
}
static bool isDir(const std::string & p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static void findNamed(const std::string & dir, const char * name, std::vector<std::string> & out) {
  DIR * d = opendir(dir.c_str());
  if (!d) return;
  while (struct dirent * e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string p = dir + "/" + e->d_name;
    if (isDir(p)) findNamed(p, name, out);
    else if (!strcmp(e->d_name, name)) out.push_back(p);
  }
  closedir(d);
}
static std::string dirOf(const std::string & p) { size_t k = p.rfind('/'); return k == std::string::npos ? "." : p.substr(0, k); }
static std::string fmtT(int64_t t) {
  if (t <= 0) return "—";
  struct tm lt; plat::localTime(t, &lt);
  char b[32]; strftime(b, sizeof(b), "%d/%m/%Y %H:%M", &lt);
  return b;
}
static std::string fmtDay(int64_t t) {
  struct tm lt; plat::localTime(t, &lt);
  static const char * J[7] = { "dim", "lun", "mar", "mer", "jeu", "ven", "sam" };
  char b[32]; snprintf(b, sizeof(b), "%s %02d/%02d", J[lt.tm_wday], lt.tm_mday, lt.tm_mon + 1);
  return b;
}
static bool holidayT(int64_t t) { struct tm lt; plat::localTime(t, &lt); return isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday); }
static int dayClass(int64_t t) {                    // 0 ouvré, 1 samedi, 2 dimanche (et férié)
  struct tm lt; plat::localTime(t, &lt);
  if (lt.tm_wday == 0 || isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)) return 2;
  return lt.tm_wday == 6 ? 1 : 0;
}
static int64_t luxT(int y, int mo, int d, int h, int mi) {
  struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  return (int64_t)mktime(&t);
}
struct Hdr { uint32_t magic; uint16_t ver, nSt; int32_t head, count; int64_t learnSince; uint32_t learnSamples; int64_t savedAt; };
static bool readHdr(const std::vector<uint8_t> & b, Hdr & h) {
  if (b.size() < 40) return false;
  memcpy(&h.magic, &b[0], 4); memcpy(&h.ver, &b[4], 2); memcpy(&h.nSt, &b[6], 2);
  memcpy(&h.head, &b[8], 4); memcpy(&h.count, &b[12], 4); memcpy(&h.learnSince, &b[16], 8);
  memcpy(&h.learnSamples, &b[24], 4); memcpy(&h.savedAt, &b[32], 8);
  return true;
}
static void clearTmpFs() {
  mkdir(TMP_FS, 0755);
  for (const char * f : { "/vn_hist.bin", "/vn_prof.bin", "/vn_days.bin" }) unlink((std::string(TMP_FS) + f).c_str());
}
struct Acc {
  double s = 0, q = 0; long n = 0;
  void add(double e) { s += fabs(e); q += e * e; n++; }
  double mean() const { return n ? s / n : NAN; }
};
static double median(std::vector<double> v) {
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// ── Données ──────────────────────────────────────────────────────────
struct RSt {
  std::string sid, name;
  int cap = 0; bool capKnown = false;
  float lat = NAN, lon = NAN;
  int maxSeen = 0;
  std::vector<std::pair<int64_t, int>> caps;      // (instant de la liste, capacité) — agrandissements
  int capAt(int64_t t) const {                  // capacité de la première liste vidée à t ou après (sinon la dernière)
    if (caps.empty()) return cap;
    for (auto & k : caps) if (k.first >= t) return k.second;
    return caps.back().second;
  }
};
struct Data {
  std::vector<RSt> st;
  std::map<std::string, int> idx;
  std::vector<int64_t> t;                         // tranches, croissantes
  std::vector<std::vector<uint8_t>> v;            // [tranche][station] vélos, 255 = absent
  std::vector<NetSample> ns;                      // météo de la tranche
  std::unordered_map<int64_t, int> at;            // instant → tranche
  int files = 0, rejected = 0, conflicts = 0;
  std::vector<std::string> notes;
  int stOf(const std::string & sid) {
    auto it = idx.find(sid);
    if (it != idx.end()) return it->second;
    int k = (int)st.size(); st.push_back(RSt()); st.back().sid = sid; idx[sid] = k;
    return k;
  }
};

static bool loadHist(const std::string & path, Data & D, Model & scratch, std::map<int64_t, std::pair<std::vector<uint8_t>, NetSample>> & acc) {
  auto b = readFile(path);
  Hdr h{};
  if (!readHdr(b, h) || h.magic != 0x31484E56) {
    D.notes.push_back(path + " : ignoré (ce n'est pas un /vn_hist.bin : en-tête absent ou faux)");
    D.rejected++; return false;
  }
  clearTmpFs();
  writeFile(std::string(TMP_FS) + "/vn_hist.bin", b);
  scratch.n = 0;
  scratch.resetHistory();                          // efface aussi le dossier temporaire…
  writeFile(std::string(TMP_FS) + "/vn_hist.bin", b);   // … d'où la seconde copie
  scratch.load(0);
  if (scratch.count <= 0) {
    char m[160]; snprintf(m, sizeof(m), " : ignoré (version %u, %zu octets : taille ou version refusées par Model::load — fichier tronqué ?)", h.ver, b.size());
    D.notes.push_back(path + m);
    D.rejected++; return false;
  }
  std::vector<int> map(scratch.n);
  for (int i = 0; i < scratch.n; i++) map[i] = D.stOf(scratch.st[i].sid);
  for (int b2 = 0; b2 < scratch.count; b2++) {
    int r = scratch.ringIndex(b2);
    if (r < 0) continue;
    int64_t t = scratch.ringT[r];
    if (t <= 0) continue;
    const uint8_t * row = scratch.ring + (size_t)r * VN_MAX_ST;
    bool any = false;
    for (int i = 0; i < scratch.n; i++) any |= row[i] != 255;
    if (!any) continue;                            // tranche comblée après un trou : rien de relevé
    auto & e = acc[t];
    if (e.first.size() < D.st.size()) e.first.resize(D.st.size(), 255);
    bool fresh = true;
    for (int i = 0; i < scratch.n; i++) {
      uint8_t x = row[i];
      if (x == 255) continue;
      uint8_t & y = e.first[map[i]];
      if (y != 255) { fresh = false; if (y != x) D.conflicts++; }
      y = x;
    }
    if (fresh) e.second = scratch.net[r];
  }
  D.files++;
  return true;
}

static void loadStationsCsv(const std::string & path, Data & D) {
  auto b = readFile(path);
  if (b.empty()) return;
  std::string s((const char *)b.data(), b.size());
  int64_t t = 0;
  size_t k = s.find("t=");
  if (k != std::string::npos) t = atoll(s.c_str() + k + 2);
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    std::string ln = s.substr(p, e - p);
    p = e + 1;
    if (ln.empty() || ln[0] == '#' || ln.rfind("sid;", 0) == 0) continue;
    std::vector<std::string> f;
    size_t q = 0;
    for (int c = 0; c < 5; c++) { size_t z = ln.find(';', q); if (z == std::string::npos) break; f.push_back(ln.substr(q, z - q)); q = z + 1; }
    if (f.size() < 5) continue;
    f.push_back(ln.substr(q));
    int i = D.stOf(f[0]);
    RSt & r = D.st[i];
    int cap = atoi(f[1].c_str());
    r.name = f[5]; r.lat = (float)atof(f[3].c_str()); r.lon = (float)atof(f[4].c_str());
    if (cap > 0) {
      r.caps.push_back({ t, cap });
      r.cap = cap; r.capKnown = true;
    }
  }
}
static bool loadStationsJson(const std::string & path, Data & D) {
  auto b = readFile(path);
  if (b.empty()) return false;
  static InfoRec ir[VN_MAX_ST];
  int n = gbfsParseInfo((const char *)b.data(), b.size(), ir, VN_MAX_ST);
  if (n <= 0) return false;
  for (int k = 0; k < n; k++) {
    auto it = D.idx.find(ir[k].sid);
    if (it == D.idx.end()) continue;            // une station absente de l'historique ne sert à rien
    RSt & r = D.st[it->second];
    if (r.capKnown) continue;
    r.cap = ir[k].cap; r.capKnown = ir[k].cap > 0; r.name = ir[k].name; r.lat = ir[k].lat; r.lon = ir[k].lon;
  }
  return true;
}

// ── Vérité construite (indépendante du modèle évalué) ────────────────
struct Inc { int st; int64_t t0, t1; int sign; bool seen = false; };
struct Truth {
  std::vector<Inc> inc;
  std::vector<float> disp;                         // écart médian par station
  long refLoo = 0, refProf = 0, refNone = 0;       // origine de la référence (tranches 7–22 h)
  long rebal = 0;                                  // sauts écartés : sorties de rupture (rééquilibrages probables)
};
static float g_thAbs = 5.f, g_thK = 4.f; static int g_thRun = 6;    // seuils de la vérité (--verite-seuils A,K,N)
static void buildTruth(const Data & D, const Model * mRef, Truth & T) {
  const int S = (int)D.st.size(), NT = (int)D.t.size();
  // sommes par (station, type de jour, quart d'heure) et par jour
  std::vector<double> sum((size_t)S * 3 * 96, 0);
  std::vector<int> cnt((size_t)S * 3 * 96, 0), nd((size_t)S * 3 * 96, 0);
  std::vector<int32_t> lastDay((size_t)S * 3 * 96, INT32_MIN);
  std::vector<int> dc(NT), qq(NT); std::vector<int32_t> day(NT); std::vector<char> ok(NT);
  for (int k = 0; k < NT; k++) {
    struct tm lt; plat::localTime(D.t[k], &lt);
    dc[k] = dayClass(D.t[k]); qq[k] = (lt.tm_hour * 60 + lt.tm_min) / 15; day[k] = localDay(D.t[k]);
    ok[k] = lt.tm_hour >= 7 && lt.tm_hour < 22 && !holidayT(D.t[k]);
    if (!ok[k]) continue;
    for (int s = 0; s < S; s++) {
      uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
      if (x == 255) continue;
      size_t c = ((size_t)s * 3 + dc[k]) * 96 + qq[k];
      sum[c] += x; cnt[c]++;
      if (lastDay[c] != day[k]) { nd[c]++; lastDay[c] = day[k]; }
    }
  }
  // référence par tranche (laisser-un-jour-dehors), puis écarts
  std::vector<float> dev((size_t)NT * S, NAN);
  std::vector<std::vector<double>> absd(S);
  int k0 = 0;
  while (k0 < NT) {
    int k1 = k0;
    while (k1 < NT && day[k1] == day[k0]) k1++;
    std::map<size_t, std::pair<double, int>> own;
    for (int k = k0; k < k1; k++) if (ok[k])
      for (int s = 0; s < S; s++) {
        uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
        if (x == 255) continue;
        auto & o = own[((size_t)s * 3 + dc[k]) * 96 + qq[k]]; o.first += x; o.second++;
      }
    for (int k = k0; k < k1; k++) if (ok[k])
      for (int s = 0; s < S; s++) {
        uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
        if (x == 255) continue;
        size_t c = ((size_t)s * 3 + dc[k]) * 96 + qq[k];
        auto & o = own[c];
        float ref = NAN;
        if (nd[c] - 1 >= 3 && cnt[c] - o.second > 0) { ref = (float)((sum[c] - o.first) / (cnt[c] - o.second)); T.refLoo++; }
        else if (mRef) {
          float a, ne;
          if (mRef->profileAt(s, D.t[k], a, ne) && ne >= 4) { ref = a; T.refProf++; }
        }
        if (isnan(ref)) { T.refNone++; continue; }
        float d = x - ref;
        dev[(size_t)k * S + s] = d;
        absd[s].push_back(fabsf(d));
      }
    k0 = k1;
  }
  // saut sur 15 min, corrigé du mouvement de référence : r = (x(t) − x(t − 15)) − (R(t) − R(t − 15))
  T.disp.assign(S, NAN);
  std::vector<float> jr((size_t)NT * S, NAN);
  std::vector<std::vector<double>> absr(S);
  for (int k = 3; k < NT; k++) {
    if (D.t[k] - D.t[k - 3] != 900) continue;
    for (int s = 0; s < S; s++) {
      float d1 = dev[(size_t)k * S + s], d0 = dev[(size_t)(k - 3) * S + s];
      if (isnan(d1) || isnan(d0)) continue;
      jr[(size_t)k * S + s] = d1 - d0;
      absr[s].push_back(fabsf(d1 - d0));
    }
  }
  for (int s = 0; s < S; s++) T.disp[s] = absr[s].size() >= 24 ? (float)fmax(0.5, median(absr[s])) : NAN;
  // un saut franc (≥ max(A, K × saut médian de la station)) qui DURE : 30 min plus tard (N tranches), l'écart à la
  // référence est encore décalé d'au moins la moitié du saut, du même côté. Fenêtre de vérité : 3 h (comme le banc simulé).
  for (int s = 0; s < S; s++) {
    if (isnan(T.disp[s])) continue;
    float th = fmaxf(g_thAbs, g_thK * T.disp[s]);
    int64_t lastEnd = 0;
    for (int k = 3; k < NT; k++) {
      float r = jr[(size_t)k * S + s];
      if (isnan(r) || fabsf(r) < th || D.t[k] < lastEnd) continue;
      int kp = k + g_thRun;
      if (kp >= NT || D.t[kp] - D.t[k] != (int64_t)g_thRun * VN_RING_STEP_S) continue;
      float before = dev[(size_t)(k - 3) * S + s], after = dev[(size_t)kp * S + s];
      if (isnan(after) || (after - before) * (r > 0 ? 1 : -1) < 0.5f * fabsf(r)) continue;
      // sortie de rupture : la station était vide ou pleine juste avant — un rééquilibrage, pas un incident
      int xb = D.v[k - 3][s], cp = D.st[s].capAt(D.t[k - 3]);
      if (xb == 0 || xb >= cp) { T.rebal++; lastEnd = D.t[k] + 1800; continue; }
      Inc x; x.st = s; x.t0 = D.t[k - 3]; x.t1 = D.t[k] + 3 * 3600; x.sign = r > 0 ? 1 : -1;
      T.inc.push_back(x);
      lastEnd = x.t1;
    }
  }
  std::sort(T.inc.begin(), T.inc.end(), [](const Inc & x, const Inc & y) { return x.st != y.st ? x.st < y.st : x.t0 < y.t0; });
}

// ── Rejeu ────────────────────────────────────────────────────────────
struct DayM {
  int32_t day = 0; int64_t t0 = 0; bool hol = false; int dclass = 0;
  Acc fc[NH], per[NH], prof[NH];
  int falseU = 0, trueU = 0, slots = 0;
  float learn = NAN, conf = NAN, evalFrac = NAN;
};
struct RunOut {
  std::vector<DayM> days;
  std::vector<char> seen;                          // incident vu
  int unusualEp = 0;
  std::map<int, std::vector<std::pair<int64_t, float>>> capDev;   // station agrandie → (t, observé − profil)
  int firstK = 0;
  Model * m = nullptr;                             // le modèle en fin de rejeu (profil le plus mûr)
};
struct Opts {
  std::string hist, prof, stations, verite;
  bool fromProf = true, leak = false;
  int matureDay = -1;                              // --mur J : ligne « mûr » à partir du J-ième jour rejoué (0 = premier)
  std::vector<std::pair<std::string, std::string>> levers;
};
static bool applyLever(const std::string & k, double num, bool v) {
  if (k == "flux_meteo") g_pred.wxFlow = v;
  else if (k == "alerte_disp") g_alertOpt.disp = v;
  else if (k == "alerte_k") g_alertOpt.k = (float)num;
  else if (k == "inertie") g_alertOpt.persist = (int)num;
  else if (k == "alerte_echelle") g_alertOpt.scale = (float)num;
  else if (k == "prevue_meteo") g_pred.wxForecast = v;
  else if (k == "ferie_dimanche") g_pred.holidaySunday = v;
  else if (k == "ferie_appris") g_pred.holidayLearn = (uint8_t)num;
  else if (k == "ferie_alerte") g_alertOpt.holidayScale = (float)num;
  else if (k == "profil_n") g_pred.profDecayN = (int)num;
  else if (k == "rajeunir") g_pred.rejuvN = (int)num;
  else return false;
  return true;
}

static void infoRecs(const Data & D, int64_t t, std::vector<InfoRec> & ir) {
  ir.assign(D.st.size(), InfoRec());
  for (size_t i = 0; i < D.st.size(); i++) {
    InfoRec & r = ir[i]; memset(&r, 0, sizeof(r));
    snprintf(r.sid, sizeof(r.sid), "%s", D.st[i].sid.c_str());
    snprintf(r.name, sizeof(r.name), "%s", D.st[i].name.empty() ? D.st[i].sid.c_str() : D.st[i].name.c_str());
    r.lat = isnan(D.st[i].lat) ? 49.61f : D.st[i].lat; r.lon = isnan(D.st[i].lon) ? 6.13f : D.st[i].lon;
    r.cap = (uint16_t)D.st[i].capAt(t);
  }
}

static bool loadProfInto(Model & m, const std::string & prof) {
  clearTmpFs();
  auto b = readFile(prof);
  if (b.empty()) return false;
  writeFile(std::string(TMP_FS) + "/vn_prof.bin", b);
  uint32_t before = m.learnSamples;
  m.load(0);
  clearTmpFs();
  return m.learnSamples != before || m.learnSince != 0;
}

static void replay(const Data & D, const Opts & o, const Truth & T, int firstK, RunOut & R) {
  static Model * mp = nullptr;
  mp = new Model();                                // un modèle neuf par variante (la mémoire est rendue à la sortie)
  Model & m = *mp;
  m.init();
  R.m = mp;
  std::vector<InfoRec> ir;
  infoRecs(D, D.t[firstK], ir);
  m.applyInfo(ir.data(), (int)ir.size(), D.t[firstK]);
  if (o.fromProf && !o.prof.empty()) loadProfInto(m, o.prof);
  const int S = (int)D.st.size(), NT = (int)D.t.size();
  std::vector<int> mi(S);
  for (int s = 0; s < S; s++) mi[s] = m.find(D.st[s].sid.c_str());
  std::vector<StatusRec> sr(S);
  std::vector<int> capNow(S);
  for (int s = 0; s < S; s++) capNow[s] = ir[s].cap;
  std::vector<int64_t> lastUnus(S, 0);
  std::vector<char> prevU(S, 0);
  R.seen.assign(T.inc.size(), 0);
  R.firstK = firstK;
  std::map<int32_t, int> dayIx;
  // incidents par station (index), pour les consultations rapides
  std::vector<std::vector<int>> incOf(S);
  for (size_t q = 0; q < T.inc.size(); q++) incOf[T.inc[q].st].push_back((int)q);
  std::map<int, int64_t> capChange;                // station → instant du changement de capacité
  for (int s = 0; s < S; s++)
    for (size_t c = 1; c < D.st[s].caps.size(); c++)
      if (D.st[s].caps[c].second != D.st[s].caps[c - 1].second) capChange[s] = D.st[s].caps[c - 1].first;

  struct P { int k; int16_t s; float cur, fc[NH], pr[NH]; };
  std::vector<P> preds;
  for (int k = firstK; k < NT; k++) {
    int64_t t = D.t[k];
    struct tm lt; plat::localTime(t, &lt);
    int32_t dd = localDay(t);
    if (!dayIx.count(dd)) {
      dayIx[dd] = (int)R.days.size(); R.days.push_back(DayM());
      DayM & X = R.days.back(); X.day = dd; X.t0 = t; X.hol = holidayT(t); X.dclass = dayClass(t);
    }
    DayM & X = R.days[dayIx[dd]];
    X.slots++;
    // capacité du moment (agrandissement annoncé par station_information)
    bool capChanged = false;
    for (int s = 0; s < S; s++) { int c = D.st[s].capAt(t); if (c != capNow[s]) { capNow[s] = c; capChanged = true; } }
    if (capChanged) { infoRecs(D, t, ir); m.applyInfo(ir.data(), (int)ir.size(), t); }
    int n = 0;
    for (int s = 0; s < S; s++) {
      uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
      if (x == 255) continue;
      StatusRec & r = sr[n++]; memset(&r, 0, sizeof(r));
      snprintf(r.sid, sizeof(r.sid), "%s", D.st[s].sid.c_str());
      int cap = std::max(capNow[s], (int)x);
      r.bikes = r.elec = x; r.docks = (uint16_t)(cap - x);
      r.installed = r.renting = r.returning = 1;
      r.lastReported = t;
    }
    const NetSample & ns = D.ns[k];
    if (netWxKnown(ns)) {
      m.wx = Weather(); m.wx.valid = true; m.wx.fetchedAt = t;
      m.wx.temp = ns.wxTemp; m.wx.rain = ns.wxRain / 10.f; m.wx.wind = ns.wxWind == WX_NA ? 0 : ns.wxWind;
      m.wx.code = ns.wxCode == WX_NA ? 0 : ns.wxCode; m.wx.nfc = 0;
      wxScore(m.wx);
    } else m.wx = Weather();
    m.applyStatus(sr.data(), n, t);

    // alertes « inhabituel » : épisodes (aucune dans l'heure précédente), vraies si dans un incident construit
    std::vector<char> un(S, 0);
    for (int a = 0; a < m.nAlerts; a++) if (m.alerts[a].type == AL_UNUSUAL) {
      int mi2 = m.alerts[a].st;
      for (int s = 0; s < S; s++) if (mi[s] == mi2) { un[s] = 1; break; }
    }
    for (int s = 0; s < S; s++) {
      if (un[s] && !prevU[s] && t - lastUnus[s] > 3600) {
        R.unusualEp++;
        bool real = false;
        for (int q : incOf[s]) if (t >= T.inc[q].t0 - 1800 && t <= T.inc[q].t1 + 1800) real = true;
        if (real) X.trueU++; else X.falseU++;
      }
      if (un[s]) {
        lastUnus[s] = t;
        for (int q : incOf[s]) if (t >= T.inc[q].t0 && t <= T.inc[q].t1 + 1800) R.seen[q] = 1;
      }
      prevU[s] = un[s];
    }
    // station agrandie : écart du relevé au profil appris, après le changement
    for (auto & cc : capChange) {
      int s = cc.first;
      if (t < cc.second || lt.tm_hour < 7 || lt.tm_hour >= 22 || mi[s] < 0) continue;
      uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
      float a, ne;
      if (x != 255 && m.profileAt(mi[s], t, a, ne)) R.capDev[s].push_back({ t, x - a });
    }
    // prévisions toutes les 15 min, 6 h – 22 h
    if (lt.tm_min % 15 == 0 && lt.tm_hour >= 6 && lt.tm_hour < 22) {
      for (int s = 0; s < S; s++) {
        if (mi[s] < 0) continue;
        const Station & st = m.st[mi[s]];
        if (!st.live || st.status == ST_CLOSED) continue;
        uint8_t x = s < (int)D.v[k].size() ? D.v[k][s] : 255;
        if (x == 255) continue;
        P p; p.k = k; p.s = (int16_t)s; p.cur = st.bikes;
        float cf[NP], tmp[NP];
        m.project(mi[s], t, 5, NP, tmp, cf);
        float p0, n0;
        bool has0 = m.profileAt(mi[s], t, p0, n0);
        for (int h = 0; h < NH; h++) {
          p.fc[h] = HK[h] < VN_FC_STEPS ? st.fc[HK[h]] : tmp[HK[h]];
          float ph, nh2;
          p.pr[h] = st.bikes;
          if (has0 && m.profileAt(mi[s], t + HK[h] * 300, ph, nh2))
            p.pr[h] = fminf((float)st.capEff(), fmaxf(0.f, st.bikes + cf[HK[h]] * (ph - p0)));
        }
        preds.push_back(p);
      }
    }
    if (lt.tm_hour == 12 && lt.tm_min == 0) {
      int ev = 0, op = 0;
      for (int s = 0; s < S; s++) {
        if (mi[s] < 0) continue;
        const Station & st = m.st[mi[s]];
        if (!st.live || st.status == ST_CLOSED) continue;
        op++;
        if (st.anom == AN_NORMAL || st.anom == AN_HIGH || st.anom == AN_LOW) ev++;
      }
      X.learn = m.learnPct(); X.conf = m.netConf; X.evalFrac = op ? 100.f * ev / op : NAN;
    }
  }
  // évaluation : la prévision faite à t contre le relevé RÉEL à t + h (s'il existe)
  for (const P & p : preds) {
    int32_t dd = localDay(D.t[p.k]);
    DayM & X = R.days[dayIx[dd]];
    for (int h = 0; h < NH; h++) {
      auto it = D.at.find(D.t[p.k] + (int64_t)HK[h] * 300);
      if (it == D.at.end()) continue;
      uint8_t tv = p.s < (int)D.v[it->second].size() ? D.v[it->second][p.s] : 255;
      if (tv == 255) continue;
      X.fc[h].add(roundf(p.fc[h]) - tv);
      X.per[h].add(p.cur - tv);
      X.prof[h].add(roundf(p.pr[h]) - tv);
    }
  }
}

// ── Forme des profils appris ─────────────────────────────────────────
enum Shape { SH_RES, SH_BUR, SH_LOI, SH_FLAT, SH_OTHER, SH_YOUNG, SH_COUNT };
static const char * SHN[SH_COUNT] = { "résidentiel (se vide le matin)", "bureaux / école (se remplit le matin)",
                                      "loisirs (week-end après-midi)", "peu marqué", "autre forme", "profil incomplet" };
struct ShapeOut { int shape = SH_YOUNG; float m = NAN, e = NAN, wa = NAN, ampW = NAN, ampE = NAN, mean = NAN, cover = 0; int expect = -1;
                  bool contra = false; std::vector<std::string> flags; float W[65], E[65]; };
static std::string upperAscii(const std::string & s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = s[i];
    if (c == 0xC3 && i + 1 < s.size()) {                  // é è ê ë É … → E, etc. (latin-1 en UTF-8)
      unsigned char d = s[++i] | 0x40;                    // minuscule → les deux casses
      const char * map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTSaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      char r = (d >= 0xC0 && d < 0xC0 + 64) ? map[d - 0xC0] : '?';
      o += (char)toupper((unsigned char)r);
      continue;
    }
    o += (char)toupper(c);
  }
  return o;
}
static int expectedFromName(const std::string & name) {
  std::string u = upperAscii(name);
  auto has = [&](const char * k) { return u.find(k) != std::string::npos; };
  if (has("PARC") || has("STADE") || has("PISCINE") || has("TENNIS") || has("SPORTIF") || has("THERMES")) return SH_LOI;
  if (has("CAMPUS") || has("LYCEE") || has("SCHOUL") || has("ECOLE") || has("UNIVERSIT") || has("BNP") || has("PWC")
      || has("EUROPAPARLAMENT") || has("CLOCHE D'OR")) return SH_BUR;
  return -1;
}
static void analyzeShape(const Model & m, int mi, int cap, ShapeOut & o) {
  // semaine de référence sans jour férié (lundi 19 → dimanche 25 octobre 2026) : seul le jour de semaine compte
  int valid = 0, tot = 0;
  for (int q = 0; q < 65; q++) {
    int mod = 6 * 60 + q * 15;
    double sw = 0, se = 0; int nw = 0, ne = 0;
    for (int d = 0; d < 7; d++) {
      int64_t t = luxT(2026, 10, 19 + d, mod / 60, mod % 60);
      float a, n;
      tot++;
      if (!m.profileAt(mi, t, a, n) || n < 2) continue;
      valid++;
      if (d < 5) { sw += a; nw++; } else { se += a; ne++; }
    }
    o.W[q] = nw >= 3 ? (float)(sw / nw) / cap : NAN;
    o.E[q] = ne >= 1 ? (float)(se / ne) / cap : NAN;
  }
  o.cover = tot ? (float)valid / tot : 0;
  auto W = [&](float h) { int q = (int)lroundf((h - 6) * 4); float s = 0; int n = 0; for (int k = q - 1; k <= q + 1; k++) if (k >= 0 && k < 65 && !isnan(o.W[k])) { s += o.W[k]; n++; } return n ? s / n : NAN; };
  auto E = [&](float h) { int q = (int)lroundf((h - 6) * 4); float s = 0; int n = 0; for (int k = q - 1; k <= q + 1; k++) if (k >= 0 && k < 65 && !isnan(o.E[k])) { s += o.E[k]; n++; } return n ? s / n : NAN; };
  float wmin = 9, wmax = -9, emin = 9, emax = -9; double ws = 0; int wn = 0;
  for (int q = 4; q <= 60; q++) {                  // 7 h – 21 h
    if (!isnan(o.W[q])) { wmin = fminf(wmin, o.W[q]); wmax = fmaxf(wmax, o.W[q]); ws += o.W[q]; wn++; }
    if (!isnan(o.E[q])) { emin = fminf(emin, o.E[q]); emax = fmaxf(emax, o.E[q]); }
  }
  if (o.cover < 0.8f || wn < 40) { o.shape = SH_YOUNG; return; }
  o.ampW = wmax - wmin; o.ampE = emax > emin ? emax - emin : 0; o.mean = (float)(ws / wn);
  o.m = W(9.5f) - W(6.5f); o.e = W(19.5f) - W(15.5f); o.wa = E(15.5f) - E(10.5f);
  if (o.m <= -0.10f && o.e >= 0.06f) o.shape = SH_RES;
  else if (o.m >= 0.10f && o.e <= -0.06f) o.shape = SH_BUR;
  else if (o.ampE >= 0.15f && o.ampE >= 1.5f * o.ampW && o.wa > 0.08f) o.shape = SH_LOI;
  else if (o.ampW < 0.20f && o.ampE < 0.20f) o.shape = SH_FLAT;
  else o.shape = SH_OTHER;
  // anomalies d'exploitation, lisibles sur le profil seul
  float capf = (float)cap;
  if (wmax * capf < 1.0f && o.mean * capf < 0.5f) o.flags.push_back("toujours vide au profil (7 h–21 h)");
  if (wmin * capf > capf - 1.0f) o.flags.push_back("toujours pleine au profil (7 h–21 h)");
  if (cap >= 8 && o.ampW * capf < 1.0f && o.ampE * capf < 1.0f && o.flags.empty()) o.flags.push_back("immobile (moins d'un vélo d'amplitude : capteur figé ?)");
}

// ── Rapport ──────────────────────────────────────────────────────────
static std::string verdict(const std::vector<double> & diff, const char * unit) {
  int n = (int)diff.size();
  char b[200];
  if (n < 7) { snprintf(b, sizeof(b), "**trop court pour conclure** (%d jour%s ; il en faut au moins 14)", n, n > 1 ? "s" : ""); return b; }
  double s = 0, q = 0;
  for (double d : diff) s += d;
  double mu = s / n;
  for (double d : diff) q += (d - mu) * (d - mu);
  double se = sqrt(q / (n - 1) / n);
  const char * w = n < 14 ? " — indicatif seulement (moins de 14 jours)" : "";
  if (fabs(mu) > 2 * se) snprintf(b, sizeof(b), "écart **significatif** : %+.4f %s ± %.4f (%d jours, 2 erreurs-types)%s", mu, unit, se, n, w);
  else snprintf(b, sizeof(b), "**pas d'écart mesurable** : %+.4f %s ± %.4f (%d jours)%s", mu, unit, se, n, w);
  return b;
}

int run(int argc, char ** argv) {
  Opts o;
  int a = 1;
  while (a < argc && strcmp(argv[a], "--reel") != 0) a++;
  if (a + 2 >= argc) { fprintf(stderr, "usage : bench_prevision --reel <vn_hist.bin|dossier> <vn_prof.bin|-> [options]\n"); return 2; }
  o.hist = argv[a + 1]; o.prof = strcmp(argv[a + 2], "-") ? argv[a + 2] : "";
  for (int k = 1; k < argc; k++) {
    if (k == a) { k += 2; continue; }
    if (!strcmp(argv[k], "--stations") && k + 1 < argc) o.stations = argv[++k];
    else if (!strcmp(argv[k], "--depart") && k + 1 < argc) o.fromProf = strcmp(argv[++k], "zero") != 0;
    else if (!strcmp(argv[k], "--avec-fuite")) o.leak = true;
    else if (!strcmp(argv[k], "--verite") && k + 1 < argc) o.verite = argv[++k];
    else if (!strcmp(argv[k], "--verite-seuils") && k + 1 < argc) sscanf(argv[++k], "%f,%f,%d", &g_thAbs, &g_thK, &g_thRun);
    else if (!strcmp(argv[k], "--mur") && k + 1 < argc) o.matureDay = atoi(argv[++k]);
    else if (!strcmp(argv[k], "--levier") && k + 1 < argc) {
      std::string kv = argv[++k]; size_t eq = kv.find('=');
      o.levers.push_back({ kv.substr(0, eq), eq == std::string::npos ? "1" : kv.substr(eq + 1) });
    } else { fprintf(stderr, "option inconnue en mode --reel : %s\n", argv[k]); return 2; }
  }
  sim::g_fsDir = TMP_FS;
  settingsDefaults(g_set);
  const PredictOpts pred0 = g_pred; const AlertOpts alert0 = g_alertOpt;

  // 1. historiques
  Data D;
  std::vector<std::string> hs;
  if (isDir(o.hist)) { findNamed(o.hist, "vn_hist.bin", hs); std::sort(hs.begin(), hs.end()); }
  else hs.push_back(o.hist);
  if (hs.empty()) { fprintf(stderr, "aucun vn_hist.bin dans %s\n", o.hist.c_str()); return 1; }
  static Model scratch;
  scratch.init();
  std::map<int64_t, std::pair<std::vector<uint8_t>, NetSample>> acc;
  for (auto & h : hs) loadHist(h, D, scratch, acc);
  if (acc.empty()) {
    printf("# Banc sur données réelles — AUCUNE donnée exploitable\n\n");
    for (auto & n : D.notes) printf("- %s\n", n.c_str());
    return 1;
  }
  for (auto & e : acc) {
    D.at[e.first] = (int)D.t.size();
    D.t.push_back(e.first);
    std::vector<uint8_t> row = e.second.first; row.resize(D.st.size(), 255);
    D.v.push_back(row); D.ns.push_back(e.second.second);
  }
  for (size_t k = 0; k < D.v.size(); k++) {
    D.v[k].resize(D.st.size(), 255);
    for (size_t s = 0; s < D.st.size(); s++) if (D.v[k][s] != 255) D.st[s].maxSeen = std::max(D.st[s].maxSeen, (int)D.v[k][s]);
  }
  // 2. stations (capacités)
  int csvN = 0;
  if (!o.stations.empty()) {
    if (o.stations.size() > 4 && o.stations.substr(o.stations.size() - 4) == ".csv") { loadStationsCsv(o.stations, D); csvN = 1; }
    else loadStationsJson(o.stations, D);
  } else {
    std::vector<std::string> cs;
    for (auto & h : hs) { std::string c = dirOf(h) + "/vn_stations.csv"; if (access(c.c_str(), R_OK) == 0) cs.push_back(c); }
    for (auto & c : cs) loadStationsCsv(c, D);
    csvN = (int)cs.size();
    if (!csvN) loadStationsJson("../docs/donnees/station_information.json", D);
  }
  for (auto & s : D.st) std::sort(s.caps.begin(), s.caps.end());
  int capGuess = 0;
  for (auto & s : D.st) if (!s.capKnown) { s.cap = std::max(1, s.maxSeen); capGuess++; }
  // 3. profil de départ (aussi référence de la vérité et objet de la vérification de forme)
  static Model mRef;
  mRef.init();
  bool hasProf = false;
  Hdr ph{};
  if (!o.prof.empty()) {
    std::vector<InfoRec> ir; infoRecs(D, D.t.back(), ir);
    mRef.applyInfo(ir.data(), (int)ir.size(), D.t.back());
    auto pb = readFile(o.prof);
    readHdr(pb, ph);
    hasProf = loadProfInto(mRef, o.prof);
    if (!hasProf) D.notes.push_back(o.prof + " : profil refusé par Model::load (en-tête, version ou taille) — départ de zéro");
  }
  if (!hasProf) o.fromProf = false;
  // 4. fuite : tranches déjà apprises par le profil
  int firstK = 0;
  bool leak = false;
  if (o.fromProf && ph.savedAt > 0) {
    while (firstK < (int)D.t.size() && D.t[firstK] <= ph.savedAt) firstK++;
    if (o.leak || (int)D.t.size() - firstK < 288) { if (firstK > 0) leak = true; firstK = 0; }
  }
  const int NT = (int)D.t.size(), S = (int)D.st.size();
  // trous
  std::vector<std::pair<int64_t, int64_t>> gaps;
  for (int k = 1; k < NT; k++) if (D.t[k] - D.t[k - 1] > 3600) gaps.push_back({ D.t[k - 1], D.t[k] });
  double span = (D.t.back() - D.t.front()) / 86400.0;
  long expected = (D.t.back() - D.t.front()) / VN_RING_STEP_S + 1;
  int wxKnown = 0;
  for (auto & n : D.ns) wxKnown += netWxKnown(n);

  printf("# Banc de prévision sur DONNÉES RÉELLES\n\n");
  printf("## 0. Ce qui a été lu\n\n");
  printf("- %d historique(s) lu(s), %d refusé(s) ; %d tranches de 5 min, du %s au %s (%.1f jours, %.0f %% des tranches présentes)\n",
         D.files, D.rejected, NT, fmtT(D.t.front()).c_str(), fmtT(D.t.back()).c_str(), span, 100.0 * NT / expected);
  printf("- %d stations ; capacités : %s%s\n", S, csvN ? "listes des stations du vidage" : "docs/donnees (instantané du 13/10/2026)",
         capGuess ? (" — " + std::to_string(capGuess) + " estimée(s) au maximum de vélos vus").c_str() : "");
  printf("- météo connue sur %.0f %% des tranches%s\n", 100.0 * wxKnown / NT, wxKnown == 0 ? " (historique d'avant le format v2, ou météo absente) : prévision sans terme météo" : "");
  if (D.conflicts) printf("- %d valeurs différentes pour une même tranche entre deux vidages (la dernière lue est gardée)\n", D.conflicts);
  if (!gaps.empty()) {
    printf("- **%zu trou(s) de plus d'1 h** :", gaps.size());
    for (size_t g = 0; g < gaps.size() && g < 8; g++) printf(" %s → %s (%.1f h) ;", fmtT(gaps[g].first).c_str(), fmtT(gaps[g].second).c_str(), (gaps[g].second - gaps[g].first) / 3600.0);
    printf("%s\n", gaps.size() > 8 ? " …" : "");
  }
  if (hasProf) {
    printf("- profil : version %u, apprend depuis le %s (%u tranches apprises), sauvegardé le %s\n", ph.ver, fmtT(ph.learnSince).c_str(),
           ph.learnSamples, fmtT(ph.savedAt).c_str());
    if (ph.ver < 3) printf("  (version %u : relu cellule par cellule, dispersion inconnue — normal pour un profil de la v1.0.8)\n", ph.ver);
  } else printf("- aucun profil de départ%s\n", o.prof.empty() ? "" : " exploitable");
  printf("- départ du modèle : %s\n", o.fromProf ? "profil de la carte" : "de zéro (le modèle apprend pendant le rejeu)");
  if (o.fromProf && !leak && firstK > 0) printf("- %d tranches antérieures au profil écartées (déjà apprises) : rejeu à partir du %s\n", firstK, fmtT(D.t[firstK]).c_str());
  if (leak)
    printf("\n> **FUITE** : le profil a été sauvegardé APRÈS %d des tranches rejouées (%.1f h) : il les a déjà apprises. "
           "Les erreurs ci-dessous sont **optimistes**. Pour une mesure propre : le profil d'un vidage, l'historique du vidage suivant.\n",
           (int)(std::upper_bound(D.t.begin(), D.t.end(), ph.savedAt) - D.t.begin()), (std::min(ph.savedAt, D.t.back()) - D.t.front()) / 3600.0);
  for (auto & n : D.notes) printf("- %s\n", n.c_str());

  // 5. vérité construite
  Truth T;
  buildTruth(D, hasProf ? &mRef : nullptr, T);
  if (getenv("VN_REEL_VERITE_SEULE") && !o.verite.empty()) {    // calibrage de la vérité (sans rejeu)
    auto ib = readFile(o.verite + "/incidents.csv");
    std::string s2((const char *)ib.data(), ib.size());
    int nI = 0, hit = 0; size_t p = 0;
    while (p < s2.size()) {
      size_t e = s2.find('\n', p); if (e == std::string::npos) e = s2.size();
      char sid[16]; long long t0, t1;
      if (sscanf(s2.substr(p, e - p).c_str(), "%15[^;];%lld;%lld", sid, &t0, &t1) == 3) {
        auto it = D.idx.find(sid);
        if (it != D.idx.end() && t0 >= D.t.front() && t1 <= D.t.back() && !holidayT(t0)) {
          nI++; for (auto & c : T.inc) if (c.st == it->second && c.t0 <= t1 && c.t1 >= t0) { hit++; break; }
        }
      }
      p = e + 1;
    }
    int ok2 = 0;
    printf("VERITE seuils=%.1f,%.1f,%d construits=%zu reequilibrages=%ld injectes=%d rappel=%.1f\n", g_thAbs, g_thK, g_thRun, T.inc.size(), T.rebal, nI, nI ? 100.0 * hit / nI : NAN);
    (void)ok2;
    return 0;
  }
  // 6. rejeu de la référence, puis de la variante
  RunOut R0, R1;
  replay(D, o, T, firstK, R0);
  bool hasVar = !o.levers.empty();
  std::string varName;
  if (hasVar) {
    for (auto & l : o.levers) {
      if (!applyLever(l.first, atof(l.second.c_str()), atoi(l.second.c_str()) != 0)) { fprintf(stderr, "levier inconnu : %s\n", l.first.c_str()); return 2; }
      varName += (varName.empty() ? "" : " ") + l.first + "=" + l.second;
    }
    if (hasProf && o.fromProf) loadProfInto(mRef, o.prof);   // (le profil relu dépend de profil_n : relecture)
    replay(D, o, T, firstK, R1);
    g_pred = pred0; g_alertOpt = alert0;
  }
  // jours évaluables : au moins la moitié de la journée relevée et des prévisions notées à +60
  auto evalDays = [&](const RunOut & R, auto pred) { std::vector<int> v; for (size_t d = 0; d < R.days.size(); d++) if (R.days[d].slots >= 144 && R.days[d].fc[2].n >= 50 && pred(R.days[d])) v.push_back((int)d); return v; };
  auto ord = evalDays(R0, [](const DayM & X) { return !X.hol; });
  auto hol = evalDays(R0, [](const DayM & X) { return X.hol; });
  auto all = evalDays(R0, [](const DayM &) { return true; });
  int nEval = (int)all.size();

  printf("\n## 1. Prévision : MAE (vélos) du chiffre affiché — modèle / persistance / profil seul (sans tendance)\n\n");
  printf("Jours évaluables (≥ 12 h relevées) : %d — dont %d ouvrés, %d samedis/dimanches, %d fériés\n\n", nEval,
         (int)evalDays(R0, [](const DayM & X) { return !X.hol && X.dclass == 0; }).size(),
         (int)evalDays(R0, [](const DayM & X) { return !X.hol && X.dclass != 0; }).size(), (int)hol.size());
  printf("| échantillon |");
  for (int h = 0; h < NH; h++) printf(" %s |", HL[h]);
  printf(" n (+60) |\n|---|---|---|---|---|---|---|\n");
  auto line = [&](const char * name, const RunOut & R, const std::vector<int> & ds) {
    Acc f[NH], p[NH], q[NH];
    for (int d : ds) for (int h = 0; h < NH; h++) {
      f[h].s += R.days[d].fc[h].s; f[h].n += R.days[d].fc[h].n;
      p[h].s += R.days[d].per[h].s; p[h].n += R.days[d].per[h].n;
      q[h].s += R.days[d].prof[h].s; q[h].n += R.days[d].prof[h].n;
    }
    printf("| %s |", name);
    for (int h = 0; h < NH; h++) printf(" %.3f / %.3f / %.3f |", f[h].mean(), p[h].mean(), q[h].mean());
    printf(" %ld |\n", f[2].n);
  };
  line("tous les jours", R0, all);
  line("jours ouvrés", R0, evalDays(R0, [](const DayM & X) { return !X.hol && X.dclass == 0; }));
  line("samedis, dimanches", R0, evalDays(R0, [](const DayM & X) { return !X.hol && X.dclass != 0; }));
  std::vector<int> mur;
  if (o.matureDay >= 0) {
    for (int d : all) if (d >= o.matureDay) mur.push_back(d);
    char nm[48]; snprintf(nm, sizeof(nm), "mûr (jour ≥ %d)", o.matureDay + 1);
    line(nm, R0, mur);
  }
  if (!hol.empty()) line("jours fériés", R0, hol);
  printf("\nPar jour (+60 min, modèle / persistance) :");
  for (size_t d = 0; d < R0.days.size() && d < 70; d++)
    printf(" %s%s %.2f/%.2f ;", fmtDay(R0.days[d].t0).c_str(), R0.days[d].hol ? " (férié)" : "", R0.days[d].fc[2].mean(), R0.days[d].per[2].mean());
  printf("\n");

  printf("\n## 2. Comparaisons appariées (mêmes données, écart par jour, erreur-type entre jours)\n\n");
  std::vector<double> dTrend, dPer, dHolOrd;
  for (int d : ord) { dTrend.push_back(R0.days[d].fc[2].mean() - R0.days[d].prof[2].mean()); dPer.push_back(R0.days[d].fc[2].mean() - R0.days[d].per[2].mean()); }
  printf("1. **Le terme de tendance aide-t-il ?** (simulation : non — profil seul 1,060 contre 1,087 à +60 min)  \n"
         "   MAE +60 modèle − profil seul, jours non fériés : %s\n", verdict(dTrend, "vélo").c_str());
  {
    std::vector<double> d15;
    for (int d : ord) d15.push_back(R0.days[d].fc[0].mean() - R0.days[d].prof[0].mean());
    printf("   (à +15 min : %s)\n", verdict(d15, "vélo").c_str());
  }
  printf("2. **La prévision bat-elle la persistance ?** MAE +60 modèle − persistance : %s\n", verdict(dPer, "vélo").c_str());
  printf("3. **Les jours fériés sont-ils le pire cas ?** (simulation : MAE +60 1,31 contre 0,97 pour la persistance)  \n");
  if (hol.empty()) printf("   **non mesurable : aucun jour férié évaluable dans l'historique.**\n");
  else {
    for (int d : hol) {
      // jours ordinaires de même type de profil (dimanche si férié prévu en dimanche) pour comparer
      double s = 0; int n = 0;
      for (int e : ord) if (R0.days[e].dclass == (g_pred.holidaySunday ? 2 : R0.days[d].dclass)) { s += R0.days[e].fc[2].mean(); n++; }
      printf("   %s : MAE +60 %.3f (persistance %.3f, profil seul %.3f) ; jours ordinaires comparables : %.3f sur %d jours\n",
             fmtDay(R0.days[d].t0).c_str(), R0.days[d].fc[2].mean(), R0.days[d].per[2].mean(), R0.days[d].prof[2].mean(), n ? s / n : NAN, n);
    }
    if (hol.size() < 3) printf("   → %zu jour(s) férié(s) : **trop peu pour conclure** sur l'ampleur de l'effet (il en faut plusieurs, en semaine)\n", hol.size());
  }
  if (hasVar) {
    std::vector<int> both;
    for (int d : all) if (d < (int)R1.days.size()) both.push_back(d);
    std::vector<double> d60, d15, dF, dHol;
    for (int d : both) {
      d60.push_back(R1.days[d].fc[2].mean() - R0.days[d].fc[2].mean());
      d15.push_back(R1.days[d].fc[0].mean() - R0.days[d].fc[0].mean());
      dF.push_back((double)R1.days[d].falseU - R0.days[d].falseU);
      if (R0.days[d].hol) dHol.push_back(R1.days[d].fc[2].mean() - R0.days[d].fc[2].mean());
    }
    printf("4. **Variante « %s » contre la référence** :  \n", varName.c_str());
    printf("   MAE +60 : %s  \n   MAE +15 : %s  \n   fausses alertes / jour : %s\n", verdict(d60, "vélo").c_str(), verdict(d15, "vélo").c_str(), verdict(dF, "alerte").c_str());
    if (!dHol.empty()) printf("   jours fériés, MAE +60 : %s\n", verdict(dHol, "vélo").c_str());
  }

  printf("\n## 3. Alertes « inhabituel » contre la vérité construite\n\n");
  printf("Vérité : %zu sauts francs et durables (|Δ15 min observé − Δ15 min de référence| ≥ 5 vélos, encore décalé de moitié 30 min "
         "plus tard, jours non fériés ; fenêtre 3 h) ; %ld sorties de rupture écartées (rééquilibrages probables) ; "
         "référence a posteriori : %.0f %% des tranches par les autres jours, %.0f %% par le profil de départ, %.0f %% sans référence\n\n",
         T.inc.size(), T.rebal, 100.0 * T.refLoo / fmax(1, T.refLoo + T.refProf + T.refNone), 100.0 * T.refProf / fmax(1, T.refLoo + T.refProf + T.refNone),
         100.0 * T.refNone / fmax(1, T.refLoo + T.refProf + T.refNone));
  auto alerts = [&](const char * name, const RunOut & R) {
    double f = 0, tr = 0; int nd = 0;
    for (int d : all) { f += R.days[d].falseU; tr += R.days[d].trueU; nd++; }
    int seen = 0, tot = 0;
    for (size_t q = 0; q < T.inc.size(); q++) { if (T.inc[q].t0 < D.t[R.firstK]) continue; tot++; seen += R.seen[q]; }
    printf("| %s | %.1f | %.1f | %s |\n", name, nd ? f / nd : NAN, nd ? tr / nd : NAN,
           tot ? (std::to_string((int)lround(100.0 * seen / tot)) + " % (" + std::to_string(seen) + "/" + std::to_string(tot) + ")").c_str() : "—");
  };
  printf("| modèle | fausses / jour | vraies / jour | rappel des écarts francs |\n|---|---|---|---|\n");
  alerts("référence", R0);
  if (hasVar) alerts(varName.c_str(), R1);
  if (nEval < 7) printf("\n%d jour(s) : trop peu pour juger le bruit de l'alerte (la simulation en voyait ≈ 159 par jour, à confirmer).\n", nEval);

  printf("\n## 4. Maturation (à midi)\n\n| jour | couverture learnPct | confiance +1 h (parc) | stations évaluées « inhabituel » |\n|---|---|---|---|\n");
  for (size_t d = 0; d < R0.days.size(); d++) {
    if (!(d < 3 || d == 4 || d == 6 || d == 9 || d == 13 || d == 20 || d == 27 || d == 41 || d == 55 || d == 83 || d + 1 == R0.days.size())) continue;
    if (isnan(R0.days[d].learn)) continue;
    printf("| %zu (%s) | %.0f %% | %.0f %% | %.0f %% |\n", d + 1, fmtDay(R0.days[d].t0).c_str(), R0.days[d].learn, R0.days[d].conf, R0.days[d].evalFrac);
  }

  printf("\n## 5. Absorption d'un changement de capacité\n\n");
  if (R0.capDev.empty()) printf("Aucun changement de capacité dans les listes des stations des vidages : **absorption non mesurable** sur ces données.\n");
  for (auto & cd : R0.capDev) {
    const RSt & s = D.st[cd.first];
    int64_t tc = 0; int c0 = 0, c1 = 0;
    for (size_t c = 1; c < s.caps.size(); c++) if (s.caps[c].second != s.caps[c - 1].second) { tc = s.caps[c - 1].first; c0 = s.caps[c - 1].second; c1 = s.caps[c].second; }
    printf("Station %s (%s) : %d → %d bornes entre le %s et le vidage suivant. Écart moyen relevé − profil (7 h–22 h) par semaine :", s.sid.c_str(), s.name.c_str(), c0, c1, fmtT(tc).c_str());
    double sum[20] = {}; int n[20] = {};
    for (auto & p : cd.second) { int w = (int)((p.first - tc) / (7 * 86400)); if (w >= 0 && w < 20) { sum[w] += p.second; n[w]++; } }
    for (int w = 0; w < 20; w++) if (n[w]) printf(" s%d %+.2f ;", w + 1, sum[w] / n[w]);
    printf("\n");
  }

  // 7. forme des profils
  printf("\n## 6. Forme des profils appris%s\n\n", hasProf && ph.savedAt >= D.t.back() ? " (profil de la carte)" : " (profil en fin de rejeu)");
  // le profil le plus mûr : celui de la carte s'il a été sauvegardé après la dernière tranche rejouée (un seul
  // vidage), sinon le modèle de référence en fin de rejeu (parti du profil de la carte, ou de zéro)
  const bool cardProf = hasProf && ph.savedAt >= D.t.back();
  const Model * mp = cardProf ? &mRef : R0.m;
  std::vector<ShapeOut> sh(S);
  int cnt[SH_COUNT] = {};
  for (int s = 0; s < S; s++) {
    int mi = mp->find(D.st[s].sid.c_str());
    if (mi < 0) continue;
    analyzeShape(*mp, mi, std::max(1, D.st[s].capAt(D.t.back())), sh[s]);
    cnt[sh[s].shape]++;
    sh[s].expect = expectedFromName(D.st[s].name);
    int e = sh[s].expect, g = sh[s].shape;
    sh[s].contra = e >= 0 && g != SH_YOUNG &&
                   ((e == SH_BUR && g == SH_RES) || (e == SH_RES && g == SH_BUR) ||
                    (e == SH_LOI && (g == SH_RES || g == SH_BUR) && !(sh[s].ampE > sh[s].ampW)));
  }
  printf("| forme | stations |\n|---|---|\n");
  for (int k = 0; k < SH_COUNT; k++) printf("| %s | %d |\n", SHN[k], cnt[k]);
  // voisinage : corrélation de la courbe ouvrée avec les 3 stations les plus proches (≤ 700 m)
  auto corr = [&](const ShapeOut & x, const ShapeOut & y) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0; int n = 0;
    for (int q = 4; q <= 60; q++) if (!isnan(x.W[q]) && !isnan(y.W[q])) { sx += x.W[q]; sy += y.W[q]; sxx += x.W[q] * x.W[q]; syy += y.W[q] * y.W[q]; sxy += x.W[q] * y.W[q]; n++; }
    if (n < 30) return (double)NAN;
    double vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
    return vx > 1e-6 && vy > 1e-6 ? (sxy - sx * sy / n) / sqrt(vx * vy) : (double)NAN;
  };
  std::vector<std::string> neigh(S);
  for (int s = 0; s < S; s++) {
    if (sh[s].shape == SH_YOUNG || isnan(D.st[s].lat) || sh[s].ampW < 0.15f) continue;
    std::vector<std::pair<float, int>> nb;
    for (int t2 = 0; t2 < S; t2++) if (t2 != s && sh[t2].shape != SH_YOUNG && !isnan(D.st[t2].lat) && sh[t2].ampW >= 0.15f) {
      float dm = haversineM(D.st[s].lat, D.st[s].lon, D.st[t2].lat, D.st[t2].lon);
      if (dm <= 700) nb.push_back({ dm, t2 });
    }
    std::sort(nb.begin(), nb.end());
    if (nb.size() < 2) continue;
    if (nb.size() > 3) nb.resize(3);
    double c = 0, cn = 0, cc = 0; int ncc = 0;
    for (auto & p : nb) { double r = corr(sh[s], sh[p.second]); if (!isnan(r)) { c += r; cn++; } }
    for (size_t i = 0; i < nb.size(); i++) for (size_t j = i + 1; j < nb.size(); j++) { double r = corr(sh[nb[i].second], sh[nb[j].second]); if (!isnan(r)) { cc += r; ncc++; } }
    if (cn >= 2 && c / cn <= -0.3 && ncc && cc / ncc >= 0.3) {
      char b[96]; snprintf(b, sizeof(b), "contredit ses %d voisines (corrélation %.2f ; elles entre elles %.2f)", (int)cn, c / cn, cc / ncc);
      neigh[s] = b;
    }
  }
  int nFlag = 0;
  printf("\nStations à regarder (le profil réel contredit le type attendu, ou une anomalie se lit sur le profil) :\n\n"
         "| station | nom | attendu (d'après le nom) | forme apprise | matin Δ | soir Δ | week-end Δ | signal |\n|---|---|---|---|---|---|---|---|\n");
  for (int s = 0; s < S; s++) {
    std::string sig;
    if (sh[s].contra) sig = "contredit son type";
    for (auto & f : sh[s].flags) sig += (sig.empty() ? "" : " ; ") + f;
    if (!neigh[s].empty()) sig += (sig.empty() ? "" : " ; ") + neigh[s];
    if (sig.empty()) continue;
    nFlag++;
    printf("| %s | %s | %s | %s | %+.2f | %+.2f | %+.2f | %s |\n", D.st[s].sid.c_str(), D.st[s].name.c_str(),
           sh[s].expect >= 0 ? SHN[sh[s].expect] : "—", SHN[sh[s].shape], sh[s].m, sh[s].e, sh[s].wa, sig.c_str());
  }
  if (!nFlag) printf("| — | aucune | | | | | | |\n");
  printf("\n(Δ en fraction de la capacité : matin = 9 h 30 − 6 h 30 en semaine, soir = 19 h 30 − 15 h 30, week-end = 15 h 30 − 10 h 30. "
         "Le type « attendu » ne vient que du nom — parc, stade, campus, lycée, bureaux nommés — ; le voisinage est un indice, pas une preuve.)\n");

  // 8. contrôle de la chaîne simulée (vérité connue)
  double vtPrec = NAN, vtRec = NAN, shAcc = NAN; int trap = -1;
  if (!o.verite.empty()) {
    printf("\n## 7. Contrôle à vérité connue (chaîne simulée)\n\n");
    auto ib = readFile(o.verite + "/incidents.csv");
    std::vector<Inc> vi;
    std::string s((const char *)ib.data(), ib.size());
    size_t p = 0;
    while (p < s.size()) {
      size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
      std::string ln = s.substr(p, e - p); p = e + 1;
      char sid[16]; long long t0, t1;
      if (sscanf(ln.c_str(), "%15[^;];%lld;%lld", sid, &t0, &t1) == 3) {
        auto it = D.idx.find(sid);
        if (it != D.idx.end() && t0 >= D.t.front() && t1 <= D.t.back() && !holidayT(t0)) { Inc x; x.st = it->second; x.t0 = t0; x.t1 = t1; x.sign = 0; vi.push_back(x); }
      }
    }
    int hit = 0, ok2 = 0;
    for (auto & x : vi) { bool f = false; for (auto & c : T.inc) if (c.st == x.st && c.t0 <= x.t1 && c.t1 >= x.t0) f = true; hit += f; }
    for (auto & c : T.inc) { bool f = false; for (auto & x : vi) if (c.st == x.st && c.t0 <= x.t1 + 1800 && c.t1 >= x.t0) f = true; ok2 += f; }
    vtRec = vi.empty() ? NAN : 100.0 * hit / vi.size(); vtPrec = T.inc.empty() ? NAN : 100.0 * ok2 / T.inc.size();
    printf("Vérité construite contre incidents injectés (jours non fériés de l'historique) : %zu injectés, %zu construits ; "
           "%.0f %% des injectés retrouvés, %.0f %% des construits sont un injecté (le reste : sauts nés du hasard des trajets ; "
           "les sorties de rupture — le camion — sont déjà écartées)\n\n",
           vi.size(), T.inc.size(), vtRec, vtPrec);
    auto tb = readFile(o.verite + "/types.csv");
    std::string ts((const char *)tb.data(), tb.size());
    int agree = 0, judged = 0;
    static const char * KN[6] = { "résidentiel", "bureaux", "gare", "école", "parc", "mixte" };
    static const int EXP[6] = { SH_RES, SH_BUR, SH_RES, SH_BUR, SH_LOI, SH_FLAT };
    int conf[6][SH_COUNT] = {};
    p = 0;
    while (p < ts.size()) {
      size_t e = ts.find('\n', p); if (e == std::string::npos) e = ts.size();
      std::string ln = ts.substr(p, e - p); p = e + 1;
      char sid[16]; int k;
      if (sscanf(ln.c_str(), "%15[^;];%d", sid, &k) != 2 || k < 0 || k > 5) continue;
      auto it = D.idx.find(sid);
      if (it == D.idx.end() || sh[it->second].shape == SH_YOUNG) continue;
      conf[k][sh[it->second].shape]++;
      judged++;
      int g = sh[it->second].shape;
      agree += g == EXP[k] || (k == 5 && g == SH_OTHER);
    }
    shAcc = judged ? 100.0 * agree / judged : NAN;
    printf("Forme apprise contre type vrai (%d stations) : %.0f %% conformes (mixte : « peu marqué » ou « autre »)\n\n| type vrai |", judged, shAcc);
    for (int g = 0; g < SH_COUNT; g++) printf(" %s |", SHN[g]);
    printf("\n|---|---|---|---|---|---|---|\n");
    for (int k = 0; k < 6; k++) { printf("| %s |", KN[k]); for (int g = 0; g < SH_COUNT; g++) printf(" %d |", conf[k][g]); printf("\n"); }
    auto it = D.idx.find("1000");
    if (it != D.idx.end()) { trap = sh[it->second].contra ? 1 : 0; printf("\nStation piège 1000 (résidentielle nommée « Parc ») : %s\n", trap ? "signalée" : "**non signalée**"); }
  }

  // synthèse machine
  auto mean60 = [&](const RunOut & R, const std::vector<int> & ds, int which) { double s = 0; long n = 0;
    for (int d : ds) { const Acc & x = which == 0 ? R.days[d].fc[2] : which == 1 ? R.days[d].per[2] : R.days[d].prof[2]; s += x.s; n += x.n; } return n ? s / n : NAN; };
  double fA = 0; for (int d : all) fA += R0.days[d].falseU;
  int seen = 0, tot = 0;
  for (size_t q = 0; q < T.inc.size(); q++) { if (T.inc[q].t0 < D.t[R0.firstK]) continue; tot++; seen += R0.seen[q]; }
  printf("\nSYNTHESE_REEL jours=%d jours_feries=%zu tranches=%d trous=%zu fuite=%d mae60=%.4f per60=%.4f prof60=%.4f mae15=%.4f prof15=%.4f "
         "mae60_mur=%.4f fausses_j=%.2f incidents=%zu rappel=%.1f verite_rappel=%.1f verite_precision=%.1f formes_ok=%.1f piege=%d signalees=%d\n",
         nEval, hol.size(), NT, gaps.size(), leak ? 1 : 0, mean60(R0, all, 0), mean60(R0, all, 1), mean60(R0, all, 2),
         [&] { double s = 0; long n = 0; for (int d : all) { s += R0.days[d].fc[0].s; n += R0.days[d].fc[0].n; } return n ? s / n : NAN; }(),
         [&] { double s = 0; long n = 0; for (int d : all) { s += R0.days[d].prof[0].s; n += R0.days[d].prof[0].n; } return n ? s / n : NAN; }(),
         mean60(R0, mur, 0), nEval ? fA / nEval : NAN, T.inc.size(), tot ? 100.0 * seen / tot : NAN, vtRec, vtPrec, shAcc, trap, nFlag);
  if (nEval < 14) {
    printf("\n> **Historique trop court pour conclure** (%d jour(s) évaluable(s)). Pour que les comparaisons aient un sens : vider la carte "
           "**chaque jour** (l'anneau ne garde que 24 h) pendant **au moins 3 semaines** (14 jours évaluables, dont 2 week-ends complets), "
           "et **jusqu'en janvier** pour voir des jours fériés en semaine (25/12 et 01/01 tombent un vendredi en 2026–2027 ; le 01/11 est un dimanche).\n", nEval);
  }
  return 0;
}

// ── Export « carte simulée » (bench_prevision --exporter) ────────────
void exportDump(const Model & m, const char * dir, int64_t t) {
  struct tm lt; plat::localTime(t, &lt);
  char stamp[32]; strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
  std::string d = std::string(dir);
  mkdir(d.c_str(), 0755);
  d += "/archive"; mkdir(d.c_str(), 0755);
  d += "/"; d += stamp; mkdir(d.c_str(), 0755);
  DumpSnap s;
  if (!dumpSnapshot(m, s)) return;
  auto put = [&](const char * name, const uint8_t * b, size_t n) {
    FILE * f = fopen((d + name).c_str(), "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
  };
  put("/vn_hist.bin", s.blob.hist, s.blob.histLen);
  put("/vn_prof.bin", s.blob.prof, s.blob.profLen);
  if (s.blob.days) put("/vn_days.bin", s.blob.days, s.blob.daysLen);
  if (s.csv) put("/vn_stations.csv", s.csv, s.csvLen);
  dumpFree(s);
}

} // namespace reel
