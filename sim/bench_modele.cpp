// ── Banc du modèle de prévision sur les données réelles du serveur (docs/mesures-modele.md) ─────
// Rejoue en marche avant <données>/historique/AAAA-MM-JJ.jsonl[.gz] (l'anneau 5 min de la production, tranche
// par tranche) à travers le VRAI cœur (Model::applyStatus : profils appris depuis zéro comme en production le
// 26/10/2026 à 8 h 21, coefficients du modèle 2 appris en ligne). À chaque quart d'heure de 6 h à 22 h — les
// instants que le serveur note (online/lib/forecast.mjs) — il garde, pour chaque station évaluable, ce que le
// cœur voyait À CET INSTANT. Le futur n'est jamais lu pendant le rejeu : la tranche k n'est lue qu'au pas k ; la
// vérité (l'anneau à t + h) ne sert qu'à noter, après coup, et aux apprentissages en ligne une fois échue.
//
// Colonnes notées, appariées (mêmes prévisions pour toutes) : persistance, profil seul et modèle actuel (1.3.0)
// tels que la production les a ENREGISTRÉS (<données>/prevision), modèle neuf (le cœur de cette branche, rejoué),
// et les candidats (réfutés ou non retenus), appris en ligne de la même façon sur les mêmes prévisions.
//
//   bench_modele <données> [--rapport F.md] [--lignes F.tsv] [--tirages B] [--sans-fuite] [--test AAAA-MM-JJ]
//                [--aveugle DIR]
//
//   --test        premier jour de la période de test (défaut 2026-10-29) : avant = mise au point
//   --sans-fuite  contrôle de non-fuite : rejeux tronqués et futur brouillé, comparés au rejeu complet
//   --lignes      chaque prévision notée, ses entrées et sa vérité (exploration)
//   --aveugle     instantanés GBFS bruts collectés APRÈS le gel du modèle (status-T.json, info-T.json,
//                 types-T.json ; T = epoch de lecture), lus par les parseurs du cœur comme sur la carte et
//                 rejoués à la suite de l'historique : période de test à l'aveugle, notée à part. Sans journal
//                 de production, ses colonnes de référence sont rejouées (persistance, profil seul, règle 1.3.0
//                 au pas de 5 min) et sa vérité est l'anneau rejoué.
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/weather.h"
#include "../src/core/gbfs.h"
#include "../src/core/calendar.h"
#include "../src/core/days.h"
#include "../src/platform/platform.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <algorithm>
#include <functional>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

static const int NH = 5;
static const int HK[NH] = { 3, 6, 12, 24, 48 };          // horizons en tranches de 5 min (= FC_HMIN / 5)
static const char * HL[NH] = { "+15", "+30", "+60", "+120", "+240" };
static const int NP = 49;

// ── Lecture des données ──────────────────────────────────────────────
struct StInfo { std::string sid, name; int cap = 0; float lat = 49.61f, lon = 6.13f; };
struct Slot {
  int64_t t = 0;
  int list = -1;
  std::vector<int> b;                               // vélos par colonne de la liste, ‑1 = absent
  bool wx = false; float temp = 0, rain = 0, wind = 0; int code = 0;
  std::vector<StatusRec> raw;                       // période à l'aveugle : station_status lu par le parseur du cœur
  int64_t tRead = 0;                                // ... et son heure de lecture (0 = historique de production)
};
struct Hist { std::vector<std::vector<StInfo>> lists; std::vector<Slot> slots; };

static bool readText(const std::string & path, std::string & out) {
  std::string cmd = path.size() > 3 && path.substr(path.size() - 3) == ".gz" ? "gzip -dc '" + path + "'" : "cat '" + path + "'";
  FILE * f = popen(cmd.c_str(), "r");
  if (!f) return false;
  char buf[65536];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  return pclose(f) == 0;
}
// jour → fichier ; le clair fait foi s'il existe aussi en .gz (règle du serveur)
static std::map<std::string, std::string> dayFiles(const std::string & dir) {
  std::map<std::string, std::string> files;
  DIR * d = opendir(dir.c_str());
  if (!d) return files;
  while (struct dirent * e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 16 || n.compare(10, 6, ".jsonl")) continue;
    std::string day = n.substr(0, 10);
    if (!files.count(day) || n.size() == 16) files[day] = dir + "/" + n;
  }
  closedir(d);
  return files;
}
template <class F> static void forLines(const std::string & text, F fn) {
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    if (e > p) fn(text.c_str() + p, e - p);
    p = e + 1;
  }
}

static bool loadHistory(const std::string & dir, Hist & H) {
  for (auto & f : dayFiles(dir)) {
    std::string text;
    if (!readText(f.second, text)) { fprintf(stderr, "illisible : %s\n", f.second.c_str()); return false; }
    int list = -1;
    forLines(text, [&](const char * s, size_t len) {
      JsonDocument doc;
      if (deserializeJson(doc, s, len) != DeserializationError::Ok) return;
      if (doc["st"].is<JsonArray>() && !doc["t"].is<long long>()) {
        std::vector<StInfo> L;
        for (JsonArray a : doc["st"].as<JsonArray>()) {
          StInfo x; x.sid = a[0].as<const char *>(); x.name = a[1] | ""; x.cap = a[2] | 0; x.lat = a[3] | 49.61f; x.lon = a[4] | 6.13f;
          L.push_back(x);
        }
        H.lists.push_back(L);
        list = (int)H.lists.size() - 1;
      } else if (doc["t"].is<long long>() && list >= 0) {
        Slot sl; sl.t = doc["t"].as<long long>(); sl.list = list;
        for (JsonVariant v : doc["b"].as<JsonArray>()) sl.b.push_back(v.isNull() ? -1 : v.as<int>());
        JsonArray w = doc["w"].as<JsonArray>();
        if (!w.isNull() && !w[0].isNull() && !w[1].isNull()) {
          sl.wx = true; sl.temp = w[0].as<float>(); sl.rain = w[1].as<float>();
          sl.wind = w[3].isNull() ? 0 : w[3].as<float>(); sl.code = w[4].isNull() ? 0 : w[4].as<int>();
        }
        if (H.slots.empty() || sl.t > H.slots.back().t) H.slots.push_back(sl);
      }
    });
  }
  return !H.slots.empty();
}

// Journal des prévisions de la production : une entrée par (quart d'heure, station) notée
//   v = [relevé, modèle ×5, profil seul ×5, échéance vide, échéance pleine, capacité utile] ; null = non évaluée
struct ProdRow { int v[14]; };
struct ProdTick { std::vector<std::string> sid; std::vector<int> capE; };    // capE ‑1 = notée null
static bool loadProd(const std::string & dir, std::map<std::pair<int64_t, std::string>, ProdRow> & P,
                     std::map<int64_t, ProdTick> & T) {
  for (auto & f : dayFiles(dir)) {
    std::string text;
    if (!readText(f.second, text)) return false;
    std::vector<std::string> sid;
    forLines(text, [&](const char * s, size_t len) {
      JsonDocument doc;
      if (deserializeJson(doc, s, len) != DeserializationError::Ok) return;
      if (doc["sid"].is<JsonArray>()) { sid.clear(); for (JsonVariant v : doc["sid"].as<JsonArray>()) sid.push_back(v.as<const char *>()); return; }
      if (!doc["slot"].is<long long>() || sid.empty()) return;
      const int64_t slot = doc["slot"].as<long long>();
      ProdTick & k = T[slot];
      k.sid = sid; k.capE.clear();
      int i = 0;
      for (JsonVariant v : doc["st"].as<JsonArray>()) {
        if (v.isNull()) k.capE.push_back(-1);
        else {
          JsonArray a = v.as<JsonArray>();
          ProdRow r;
          for (int q = 0; q < 14; q++) r.v[q] = a[q].as<int>();
          k.capE.push_back(r.v[13]);
          if (i < (int)sid.size()) P[{ slot, sid[i] }] = r;
        }
        i++;
      }
    });
  }
  return true;
}


// ── Période à l'aveugle : instantanés GBFS bruts, parseurs du cœur (comme src/net/net_esp32.cpp) ──
// Une tranche de 5 min par lecture de station_status (la première de la tranche, comme l'anneau de la carte) ;
// station_information et vehicle_types : le dernier lu avant (ou avec) ce station_status.
static bool readFile(const std::string & path, std::string & out) {
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[65536]; size_t k; out.clear();
  while ((k = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, k);
  fclose(f);
  return true;
}
static int loadRaw(const std::string & dir, Hist & H, int64_t after) {
  std::vector<std::pair<int64_t, std::string>> files;
  DIR * d = opendir(dir.c_str());
  if (!d) return -1;
  while (struct dirent * e = readdir(d)) {
    std::string n = e->d_name;
    long long t = 0; char kind[16] = {};
    if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") || sscanf(n.c_str(), "%15[a-z]-%lld.json", kind, &t) != 2) continue;
    files.push_back({ (int64_t)t * 4 + (!strcmp(kind, "types") ? 0 : !strcmp(kind, "info") ? 1 : 2), dir + "/" + n });
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  static InfoRec ir[VN_MAX_ST];
  static StatusRec sr[VN_MAX_ST];
  ElecTypes et;
  int list = -1, added = 0;
  std::string text;
  for (auto & f : files) {
    const int64_t t = f.first / 4;
    const int kind = (int)(f.first % 4);
    if (!readFile(f.second, text)) continue;
    if (kind == 0) { gbfsParseTypes(text.data(), text.size(), et); continue; }
    if (kind == 1) {
      const int ni = gbfsParseInfo(text.data(), text.size(), ir, VN_MAX_ST);
      if (ni <= 0) continue;
      std::vector<StInfo> L;
      for (int i = 0; i < ni; i++) { StInfo x; x.sid = ir[i].sid; x.name = ir[i].name; x.cap = ir[i].cap; x.lat = ir[i].lat; x.lon = ir[i].lon; L.push_back(x); }
      H.lists.push_back(L);
      list = (int)H.lists.size() - 1;
      continue;
    }
    if (list < 0) continue;
    int64_t lu = 0;
    const int ns = gbfsParseStatus(text.data(), text.size(), sr, VN_MAX_ST, et, &lu);
    if (ns <= 0) continue;
    Slot sl; sl.t = t - t % 300; sl.tRead = t; sl.list = list;
    if (sl.t <= after || (!H.slots.empty() && sl.t <= H.slots.back().t)) continue;   // une lecture par tranche, après l'historique
    sl.raw.assign(sr, sr + ns);
    for (const StInfo & x : H.lists[list]) {
      int b = -1;
      for (int k = 0; k < ns; k++) if (x.sid == sr[k].sid) { b = sr[k].bikes; break; }
      sl.b.push_back(b);
    }
    H.slots.push_back(sl);
    added++;
  }
  return added;
}

// ── Archives de production (instantanés 6 h / 18 h) : profils appris par la production à cet instant ──
struct Arch { std::string name; int64_t savedAt = 0; uint32_t samples = 0; uint16_t ver = 0, nSt = 0; std::vector<uint8_t> prof; };
static std::vector<Arch> loadArchives(const std::string & dir) {
  std::vector<Arch> out;
  DIR * d = opendir(dir.c_str());
  if (!d) return out;
  std::vector<std::string> names;
  while (struct dirent * e = readdir(d)) if (e->d_name[0] != '.') names.push_back(e->d_name);
  closedir(d);
  std::sort(names.begin(), names.end());
  for (auto & n : names) {
    FILE * f = fopen((dir + "/" + n + "/vn_prof.bin").c_str(), "rb");
    if (!f) continue;
    Arch a; a.name = n;
    uint8_t buf[65536]; size_t k;
    while ((k = fread(buf, 1, sizeof(buf), f)) > 0) a.prof.insert(a.prof.end(), buf, buf + k);
    fclose(f);
    if (a.prof.size() < 40) continue;
    memcpy(&a.ver, a.prof.data() + 4, 2); memcpy(&a.nSt, a.prof.data() + 6, 2);
    memcpy(&a.samples, a.prof.data() + 24, 4); memcpy(&a.savedAt, a.prof.data() + 32, 8);
    out.push_back(a);
  }
  return out;
}
struct ArchCmp { std::string name; long stations = 0, stSame = 0, cells = 0, cellSame = 0, meanSame = 0; uint32_t samplesProd = 0, samplesRep = 0; bool done = false; };
// Profils du rejeu à l'instant de l'archive contre ceux de la production : cellule par cellule (moyenne ×256, n,
// dispersion et son compte), stations appariées par sid
static void compareArchive(const Model & m, const Arch & a, ArchCmp & c) {
  c.name = a.name; c.samplesProd = a.samples; c.samplesRep = m.learnSamples; c.done = true;
  const size_t cell = 4, stride = 7 * 96;
  if (a.ver < 3 || a.prof.size() < 40 + (size_t)a.nSt * (8 + stride * cell)) return;
  const uint8_t * sids = a.prof.data() + 40, * cells = sids + (size_t)a.nSt * 8;
  for (int j = 0; j < a.nSt; j++) {
    char sid[9] = {}; memcpy(sid, sids + (size_t)j * 8, 8);
    const int i = m.find(sid);
    if (i < 0) continue;
    c.stations++;
    const uint8_t * pa = cells + (size_t)j * stride * cell, * pr = (const uint8_t *)(m.prof + (size_t)i * stride);
    long same = 0;
    for (size_t q = 0; q < stride; q++) {
      const uint8_t * x = pa + q * cell, * y = pr + q * cell;
      c.cells++;
      if (!memcmp(x, y, cell)) { c.cellSame++; same++; }
      if (x[0] == y[0] && x[1] == y[1] && (x[2] & 0x1F) == (y[2] & 0x1F)) c.meanSame++;
    }
    c.stSame += same == (long)stride;
  }
}

// ── Rejeu ────────────────────────────────────────────────────────────
struct Row {
  int64_t t, slot;
  int g;                         // station (index global, par sid)
  int day, hour, cls;            // jour (0 = premier jour de l'historique), heure locale, classe de jour du modèle 2
  int cur, capE;
  float fc[NH];                  // prévision du cœur rejoué (non arrondie, bornée)
  float pr[NH];                  // profil seul (règle de vn_forecast_json)
  int etaE, etaF;                // échéances du cœur rejoué
  bool has0; float P0;
  bool hasH[NH]; float Ph[NH]; float cf[NH];
  float l15, l30;                // relevés 15 et 30 min plus tôt (anneau), NaN si absents
  float P5[13];                  // profil à t + 5k min (k = 0…12), NaN si inconnu (exploration)
  // candidat « pool médian » : le profil relu avec la médiane des autres jours de même type (au lieu de leur
  // moyenne pondérée par n), mêmes cellules au même instant ; NaN si inconnu
  float P0m, Phm[NH];
  // candidat « profil de quartier » : mouvement prévu moyen (part de la capacité utile) des 6 stations les plus
  // proches notées au même instant, × capacité utile de la station (neighbourPass, après le rejeu)
  float nb[NH];
};
struct Run {
  std::vector<Row> rows;
  std::vector<int64_t> slotT;                       // toutes les tranches rejouées
  std::vector<std::vector<uint8_t>> ring;           // anneau rejoué par tranche, par station globale (255 = absent)
  std::map<int64_t, std::array<float, 2 * FC_NC * NH>> coef;   // couples appliqués par le cœur, par quart d'heure noté
  size_t profBytes = 0;                             // taille de /vn_prof.bin du modèle à la fin du rejeu
};

static std::vector<std::string> g_sid;               // station globale → sid
static std::map<std::string, int> g_six;
static int gIndex(const std::string & sid) {
  auto it = g_six.find(sid);
  if (it != g_six.end()) return it->second;
  g_six[sid] = (int)g_sid.size(); g_sid.push_back(sid);
  return (int)g_sid.size() - 1;
}
static int64_t g_day0 = 0;                           // minuit local du premier jour de l'historique
static int dayIndex(int64_t t) { return (int)((localDay(t) - localDay(g_day0))); }

struct Perturb { int64_t cut = INT64_MAX; };        // historique tronqué après cut (contrôle de non-fuite)
struct Levers { uint8_t rule = FC_RULE_V2, eta = FC_RULE_V1; bool dayClass = true; };

static void infoOf(const std::vector<StInfo> & L, std::vector<InfoRec> & ir) {
  ir.assign(L.size(), InfoRec());
  for (size_t i = 0; i < L.size(); i++) {
    InfoRec & r = ir[i];
    memset(&r, 0, sizeof(r));
    snprintf(r.sid, sizeof(r.sid), "%s", L[i].sid.c_str());
    snprintf(r.name, sizeof(r.name), "%s", L[i].name.c_str());
    r.lat = L[i].lat; r.lon = L[i].lon; r.cap = (uint16_t)L[i].cap;
  }
}

// ── Profil relu par le banc (candidat « pool médian ») ──
// Copie de cellValue / smoothAt / profileLocal / profDM de src/core/model.cpp (statiques là-bas), avec un
// seul changement possible : med = true remplace la moyenne pondérée du pool par la médiane des cellules des
// autres jours de même type. Avec med = false, la copie doit redonner le profil du cœur au bit près
// (contrôle publié dans la section 0).
static const int BQ = 96;                           // = DAYQ
static const float B_FULL_N = 6.f;                  // = PROF_FULL_N
static bool bCell(const ProfCell * p, int dow, int q, bool med, float & v, float & nEff) {
  const ProfCell & own = p[dow * BQ + q];
  if (own.n >= B_FULL_N) { v = own.a / 256.f; nEff = own.n; return true; }
  bool weekend = (dow == 0 || dow == 6);
  float sum = 0, pn = 0, vals[7]; int nv = 0;
  for (int d = 0; d < 7; d++) {
    if (d == dow || ((d == 0 || d == 6) != weekend)) continue;
    const ProfCell & c = p[d * BQ + q];
    if (!c.n) continue;
    sum += (c.a / 256.f) * c.n;
    pn += c.n;
    vals[nv++] = c.a / 256.f;
  }
  if (!own.n && pn <= 0) return false;
  if (pn <= 0) { v = own.a / 256.f; nEff = own.n; return true; }
  float pool = sum / pn;
  if (med) {
    for (int i = 1; i < nv; i++) for (int j = i; j > 0 && vals[j - 1] > vals[j]; j--) std::swap(vals[j - 1], vals[j]);
    pool = nv % 2 ? vals[nv / 2] : 0.5f * (vals[nv / 2 - 1] + vals[nv / 2]);
  }
  float w = own.n / B_FULL_N;
  v = w * (own.a / 256.f) + (1 - w) * pool;
  nEff = own.n + 0.5f * (pn > 12 ? 12 : pn);
  return true;
}
static bool bSmooth(const ProfCell * p, int dow, int q, bool med, float & v, float & nEff) {
  float sv = 0, sw = 0, sn = 1e9f;
  static const float W[3] = { 0.25f, 0.5f, 0.25f };
  for (int o = -1; o <= 1; o++) {
    int qq = q + o, d = dow;
    if (qq < 0) { qq += BQ; d = (d + 6) % 7; }
    if (qq >= BQ) { qq -= BQ; d = (d + 1) % 7; }
    float cv, cn;
    if (!bCell(p, d, qq, med, cv, cn)) continue;
    sv += W[o + 1] * cv; sw += W[o + 1];
    if (cn < sn) sn = cn;
  }
  if (sw < 0.5f) return false;
  v = sv / sw;
  nEff = sn;
  return true;
}
static bool bProfileAt(const Model & m, int s, int64_t t, bool med, float & avg) {
  if (s < 0 || s >= m.n) return false;
  struct tm lt;
  plat::localTime(t, &lt);
  int dow = lt.tm_wday, mod = lt.tm_hour * 60 + lt.tm_min;
  if (g_pred.holidaySunday && isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)) dow = 0;
  const ProfCell * p = m.prof + (size_t)s * 7 * BQ;
  int q = mod / 15;
  float f = (mod % 15) / 15.f, v0, n0, v1, n1;
  bool a = bSmooth(p, dow, q, med, v0, n0);
  int q1 = q + 1, d1 = dow;
  if (q1 >= BQ) { q1 = 0; d1 = (dow + 1) % 7; }
  bool b = bSmooth(p, d1, q1, med, v1, n1);
  if (a && b) { avg = v0 + (v1 - v0) * f; return true; }
  if (a) { avg = v0; return true; }
  if (b) { avg = v1; return true; }
  return false;
}
static long g_bProfN = 0, g_bProfBad = 0;           // contrôle de la copie (med = false) contre Model::profileAt

// Brouillage déterministe d'un relevé futur (contrôle de non-fuite) : n'importe quelle valeur plausible
static int scrambleB(int cap, int64_t t, int col) {
  uint32_t h = (uint32_t)(t / 300) * 2654435761u ^ (uint32_t)(col + 7) * 40503u;
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  int c = cap > 0 ? cap : 20;
  return (int)(h % (uint32_t)(c + 1));
}

// hook : appelé après chaque tranche rejouée (contrôle des archives)
static void replay(const Hist & H, const std::map<int64_t, ProdTick> & ticks, const Levers & lv, const Perturb & pt, Run & R,
                   std::function<void(const Model &, int64_t)> hook = nullptr) {
  g_pred = PredictOpts();
  g_pred.fcRule = lv.rule; g_pred.etaRule = lv.eta; g_pred.fcDayClass = lv.dayClass;
  Model * mp = new Model();                         // un modèle neuf par rejeu (jamais libéré : banc, ~1 Mo chacun)
  Model & m = *mp;
  if (!m.init()) { fprintf(stderr, "init du modèle impossible\n"); exit(1); }
  std::map<std::string, int> capNow, closedNow;     // dernier état connu par sid (journal de production, à t)
  std::vector<InfoRec> ir;
  std::vector<StatusRec> sr;
  int lastList = -1;
  R = Run();
  for (size_t k = 0; k < H.slots.size(); k++) {
    const Slot & S = H.slots[k];
    if (S.t > pt.cut) break;                        // historique tronqué : rien au-delà
    const std::vector<StInfo> & L = H.lists[S.list];
    const int64_t t = S.tRead ? S.tRead : S.t + 30;  // première lecture du quart d'heure en production : ~30 s après le début
    sim::g_epoch0 = t; sim::g_ms = 0;
    if (S.list != lastList || k % 12 == 0) { infoOf(L, ir); m.applyInfo(ir.data(), (int)ir.size(), t); lastList = S.list; }
    auto it = ticks.find(S.t);
    if (it != ticks.end())
      for (size_t j = 0; j < it->second.sid.size(); j++) {
        const std::string & id = it->second.sid[j];
        int c = it->second.capE[j];
        bool inRing = false;
        for (size_t i = 0; i < L.size() && i < S.b.size(); i++) if (L[i].sid == id) { inRing = S.b[i] >= 0; break; }
        if (c >= 0) { capNow[id] = c; closedNow[id] = 0; }
        else if (inRing) closedNow[id] = 1;         // valeur dans l'anneau mais notée null : fermée
      }
    sr.clear();
    if (!S.raw.empty()) sr = S.raw;                 // à l'aveugle : le station_status lu, tel que le cœur le parse
    else for (size_t i = 0; i < L.size() && i < S.b.size(); i++) {
      if (S.b[i] < 0) continue;
      StatusRec r; memset(&r, 0, sizeof(r));
      snprintf(r.sid, sizeof(r.sid), "%s", L[i].sid.c_str());
      auto ci = capNow.find(L[i].sid);
      int capI = ci != capNow.end() ? ci->second : L[i].cap;
      int b = S.b[i];
      int cap = std::max(capI, b);
      r.bikes = r.elec = (uint16_t)b; r.docks = (uint16_t)(cap - b);
      auto cl = closedNow.find(L[i].sid);
      const bool closed = cl != closedNow.end() && cl->second;
      r.installed = closed ? 0 : 1; r.renting = r.returning = closed ? 0 : 1;
      r.lastReported = t;
      sr.push_back(r);
    }
    if (S.wx) {
      m.wx = Weather(); m.wx.valid = true; m.wx.fetchedAt = t;
      m.wx.temp = S.temp; m.wx.rain = S.rain; m.wx.wind = S.wind; m.wx.code = S.code; m.wx.nfc = 0;
      wxScore(m.wx);
    } else m.wx = Weather();
    m.applyStatus(sr.data(), (int)sr.size(), t);
    if (hook) hook(m, S.t);
    static const char * traceLots = getenv("VN_TRACE_LOTS");     // diagnostic : comptes d'apprentissage à une tranche
    if (traceLots && S.t == (int64_t)atoll(traceLots) && lv.rule == FC_RULE_V2 && lv.dayClass) {
      for (int c = 0; c < FC_NC; c++) {
        fprintf(stderr, "lots classe %d à %lld :", c, (long long)S.t);
        for (int h = 0; h < FC_NH; h++) fprintf(stderr, " %s %.2f (%.0f prév.)", HL[h], m.fcL->lots[c][h], m.fcL->w[c][h]);
        fprintf(stderr, "\n");
      }
    }

    // anneau rejoué (vérité des apprentissages en ligne, comme dans le cœur)
    const int head = m.ringIndex(0);
    std::vector<uint8_t> rowG;
    if (head >= 0 && m.ringT[head] == S.t) {
      const uint8_t * row = m.ring + (size_t)head * VN_MAX_ST;
      for (int i = 0; i < m.n; i++) {
        int g = gIndex(m.st[i].sid);
        if ((int)rowG.size() <= g) rowG.resize(g + 1, 255);
        rowG[g] = row[i];
      }
    }
    R.slotT.push_back(S.t);
    R.ring.push_back(rowG);

    struct tm lt; plat::localTime(t, &lt);
    if (lt.tm_min % 15 || lt.tm_hour < 6 || lt.tm_hour >= 22) continue;
    if (head < 0 || m.ringT[head] != S.t) continue;
    {
      std::array<float, 2 * FC_NC * NH> c{};
      for (int q = 0; q < FC_NC; q++) for (int h = 0; h < NH; h++) { c[q * NH + h] = m.fcA[q][h]; c[FC_NC * NH + q * NH + h] = m.fcB[q][h]; }
      R.coef[S.t] = c;
    }
    const uint8_t * row = m.ring + (size_t)head * VN_MAX_ST;
    static float tmp[NP], cf[NP];
    for (int i = 0; i < m.n; i++) {
      const Station & st = m.st[i];
      if (row[i] == 255 || !st.active || !st.live || st.status == ST_CLOSED) continue;
      m.project(i, t, 5, NP, tmp, cf);
      Row r{};
      r.t = t; r.slot = S.t; r.g = gIndex(st.sid);
      r.day = dayIndex(t); r.hour = lt.tm_hour; r.cls = m.fcClassAt(t);
      r.cur = st.bikes; r.capE = st.capEff();
      float n0 = 0;
      r.has0 = m.profileAt(i, t, r.P0, n0);
      for (int h = 0; h < NH; h++) {
        float nh = 0;
        r.hasH[h] = m.profileAt(i, t + (int64_t)HK[h] * 300, r.Ph[h], nh);
        r.cf[h] = cf[HK[h]];
        r.fc[h] = HK[h] < VN_FC_STEPS ? st.fc[HK[h]] : tmp[HK[h]];
        float p = st.bikes;
        if (r.has0 && r.hasH[h]) p = fminf((float)st.capEff(), fmaxf(0.f, st.bikes + cf[HK[h]] * (r.Ph[h] - r.P0)));
        r.pr[h] = p;
      }
      r.etaE = st.etaEmpty; r.etaF = st.etaFull;
      auto lag = [&](int back) {
        int ix = m.ringIndex(back);
        if (ix < 0 || m.ringT[ix] != S.t - (int64_t)back * 300) return NAN;
        uint8_t v = m.ring[(size_t)ix * VN_MAX_ST + i];
        return v == 255 ? NAN : (float)v;
      };
      r.l15 = lag(3); r.l30 = lag(6);
      for (int q = 0; q <= 12; q++) { float pk, nk; r.P5[q] = m.profileAt(i, t + (int64_t)q * 300, pk, nk) ? pk : NAN; }
      {
        float v;
        r.P0m = bProfileAt(m, i, t, true, v) ? v : NAN;
        for (int h = 0; h < NH; h++) r.Phm[h] = bProfileAt(m, i, t + (int64_t)HK[h] * 300, true, v) ? v : NAN;
        for (int h = -1; h < NH; h++) {                // la copie sans médiane = le cœur, au bit près
          const int64_t th = t + (h < 0 ? 0 : (int64_t)HK[h] * 300);
          float pc, nc, pb;
          const bool hc = m.profileAt(i, th, pc, nc), hb = bProfileAt(m, i, th, false, pb);
          g_bProfN++; g_bProfBad += hc != hb || (hc && memcmp(&pc, &pb, sizeof(float)));
        }
      }
      R.rows.push_back(r);
    }
  }
  Model::Blob b;
  if (m.serialize(b)) { R.profBytes = b.profLen; plat::psFree(b.prof); plat::psFree(b.hist); plat::psFree(b.days); }
  g_pred = PredictOpts();
}

// Voisins : les KNB stations les plus proches de chacune (coordonnées de la PREMIÈRE liste qui la décrit : une
// station n'est notée qu'après y être apparue, la liste d'après n'est donc jamais lue)
static const int KNB = 6;
static std::vector<std::vector<int>> neighbours(const Hist & H) {
  const int ns = (int)g_sid.size();
  std::vector<float> lat(ns, NAN), lon(ns, NAN);
  for (auto & L : H.lists) for (auto & x : L) { const int g = gIndex(x.sid); if (g < ns && !std::isfinite(lat[g])) { lat[g] = x.lat; lon[g] = x.lon; } }
  std::vector<std::vector<int>> nb(ns);
  for (int g = 0; g < ns; g++) {
    if (!std::isfinite(lat[g])) continue;
    std::vector<std::pair<float, int>> d;
    for (int k = 0; k < ns; k++) if (k != g && std::isfinite(lat[k])) d.push_back({ haversineM(lat[g], lon[g], lat[k], lon[k]), k });
    std::sort(d.begin(), d.end());
    for (int k = 0; k < KNB && k < (int)d.size(); k++) nb[g].push_back(d[k].second);
  }
  return nb;
}
// Rangées d'un même quart d'heure seulement : aucune donnée postérieure à l'instant de la prévision
static void neighbourPass(Run & R, const std::vector<std::vector<int>> & nbr) {
  for (size_t r0 = 0; r0 < R.rows.size();) {
    size_t r1 = r0;
    while (r1 < R.rows.size() && R.rows[r1].slot == R.rows[r0].slot) r1++;
    std::map<int, size_t> at;
    for (size_t i = r0; i < r1; i++) at[R.rows[i].g] = i;
    for (size_t i = r0; i < r1; i++) {
      Row & r = R.rows[i];
      for (int h = 0; h < NH; h++) {
        float s = 0; int c = 0;
        if (r.g < (int)nbr.size())
          for (int j : nbr[r.g]) {
            auto it = at.find(j);
            if (it == at.end()) continue;
            const Row & q = R.rows[it->second];
            if (!(q.has0 && q.hasH[h])) continue;
            s += (q.Ph[h] - q.P0) / std::max(1, q.capE); c++;
          }
        r.nb[h] = c ? s / c * r.capE : 0.f;
      }
    }
    r0 = r1;
  }
}

// ── Vérité (historique de production, comme la notation du serveur) ──
struct Truth {
  std::map<int64_t, int> ix;                        // tranche → index
  std::vector<std::vector<int16_t>> b;              // [tranche][station globale], ‑1 = absent
  void build(const Hist & H) {
    b.assign(H.slots.size(), {});
    for (size_t k = 0; k < H.slots.size(); k++) {
      const Slot & S = H.slots[k];
      ix[S.t] = (int)k;
      const auto & L = H.lists[S.list];
      for (size_t i = 0; i < L.size() && i < S.b.size(); i++) {
        int g = gIndex(L[i].sid);
        if ((int)b[k].size() <= g) b[k].resize(g + 1, -1);
        b[k][g] = (int16_t)S.b[i];
      }
    }
  }
  int at(int64_t slot, int g) const {
    auto it = ix.find(slot);
    if (it == ix.end()) return -1;
    const auto & v = b[it->second];
    return g < (int)v.size() ? v[g] : -1;
  }
};

// ── Candidats de banc : apprentissages en ligne causaux, mêmes rangées ──
// Chaque candidat prévoit à partir des entrées de la rangée (relevé, profil, confiance…) et, s'il apprend, ne juge
// une prévision qu'à son échéance (tranche t + h de l'anneau rejoué), comme le cœur : lots émis aux quarts
// d'heure notés, erreur absolue de la prévision arrondie et bornée calculée en entiers, oubli par classe de jour
// à chaque lot émis, seuil de FC_MIN_LOTS quarts d'heure jugés. Seules les rangées dont le profil est connu à t et
// à t + h apprennent (les autres se prévoient par la persistance), comme le cœur.
struct Feat { float x[3]; };
struct Variant {
  std::string name, note;
  int kind = 0;                                     // 0 = grille (a, b[, d]) ; 1 = moindres carrés ; 2 = règle fixe
  std::function<Feat(const Row &, int)> feat;
  bool dayClass = true;                             // un jeu par classe de jour (ouvré | week-end et férié)
  int nsub = 1;                                     // sous-classes par classe de jour
  std::function<int(const Row &, int)> sub;         // sous-classe d'une rangée à l'horizon h
  bool trend = false;                               // grille à 3 dimensions : d sur x[2]
  float halfLife = 7.f, minLots = FC_MIN_LOTS, pen = FC_PEN, lam = 1000.f;
  // > 0 (grille seulement) : chaque sous-classe choisit son couple sur ses erreurs + κ prévisions fictives
  // de la classe de jour entière (erreur moyenne de la classe × κ) ; le seuil de lots est celui de la classe
  float kappa = 0;
  std::function<float(const Row &, int)> fixed;
};
static const int GA = FC_NA, GB = FC_NB, GD = 16;  // d = (id − 5) / 10 : −0,5 … 1,0
static const int GD_ORDER[GD] = { 5, 4, 6, 3, 7, 2, 8, 1, 9, 0, 10, 11, 12, 13, 14, 15 };   // |d| croissant
static inline int16_t q64i(float v) { return (int16_t)lroundf(fminf(32767.f, fmaxf(-32767.f, v * FC_FIX))); }
static inline bool informative(const Row & r, int h) { return r.has0 && r.hasH[h]; }

struct VarOut { std::vector<std::array<float, NH>> f; std::map<int64_t, std::array<float, 2 * FC_NC * NH>> coef; };

static void runVariant(const Variant & V, const Run & R, VarOut & O) {
  const size_t N = R.rows.size();
  O.f.assign(N, {});
  const int NCL = V.dayClass ? FC_NC : 1, G = NCL * V.nsub;
  const int ND = V.trend ? GD : 1, NGc = GA * GB * ND;
  std::vector<std::vector<std::vector<float>>> err(NH, std::vector<std::vector<float>>(G, std::vector<float>(V.kind == 0 ? NGc : 0, 0)));
  std::vector<std::vector<float>> w(NH, std::vector<float>(G, 0)), lots(NH, std::vector<float>(G, 0));
  std::vector<std::vector<std::array<double, 9>>> Gm(NH, std::vector<std::array<double, 9>>(G, std::array<double, 9>{}));
  std::vector<std::vector<std::array<double, 3>>> rv(NH, std::vector<std::array<double, 3>>(G, std::array<double, 3>{}));
  std::vector<std::vector<std::array<float, 3>>> coef(NH, std::vector<std::array<float, 3>>(G, std::array<float, 3>{ 0, 0, 0 }));
  struct Lot { int64_t t; size_t from, to; int cls; };
  std::vector<Lot> pend;
  const float decay = powf(0.5f, 1.f / (V.halfLife * 64.f));
  auto groupOf = [&](const Row & r, int h) { return (V.dayClass ? r.cls : 0) * V.nsub + (V.sub ? V.sub(r, h) : 0); };
  const bool shrink = V.kind == 0 && V.kappa > 0;
  // classe de jour entière (rétrécissement) : erreurs, prévisions et lots cumulés sur ses sous-classes
  std::vector<std::vector<std::vector<float>>> errC(NH, std::vector<std::vector<float>>(NCL, std::vector<float>(shrink ? NGc : 0, 0)));
  std::vector<std::vector<float>> wC(NH, std::vector<float>(NCL, 0)), lotsC(NH, std::vector<float>(NCL, 0));
  auto select = [&](int h, int g) {
    coef[h][g] = { 0, 0, 0 };
    const int c = g / V.nsub;
    if (shrink ? !(lotsC[h][c] >= V.minLots) || !(wC[h][c] > 0) : !(lots[h][g] >= V.minLots) || !(w[h][g] > 0)) return;
    if (V.kind == 0) {
      const float inv = shrink ? 1.f / (w[h][g] + V.kappa) : 1.f / w[h][g], kc = shrink ? V.kappa / wC[h][c] : 0.f;
      float best = 0; int bj = -1;
      for (int ia = 0; ia < GA; ia++)
        for (int ib = 0; ib < GB; ib++)
          for (int od = 0; od < ND; od++) {
            const int id = V.trend ? GD_ORDER[od] : 0;
            const int j = (ia * GB + ib) * ND + id;
            const float d = V.trend ? (id - 5) / 10.f : 0.f;
            const float e = shrink ? err[h][g][j] + kc * errC[h][c][j] : err[h][g][j];
            const float v = e * inv + V.pen * (ia / 10.f + ib / 20.f + fabsf(d));
            if (bj < 0 || v < best) { best = v; bj = j; }
          }
      const int id = bj % ND, ib = (bj / ND) % GB, ia = bj / ND / GB;
      coef[h][g] = { ia / 10.f, ib / 20.f, V.trend ? (id - 5) / 10.f : 0.f };
    } else if (V.kind == 1) {
      const int K = V.trend ? 3 : 2;
      double A[3][4];
      for (int a = 0; a < K; a++) { for (int b = 0; b < K; b++) A[a][b] = Gm[h][g][a * 3 + b] + (a == b ? V.lam : 0); A[a][K] = rv[h][g][a]; }
      for (int c = 0; c < K; c++) {
        int p = c; for (int r = c + 1; r < K; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        for (int q = 0; q <= K; q++) std::swap(A[c][q], A[p][q]);
        if (fabs(A[c][c]) < 1e-12) continue;
        for (int r = 0; r < K; r++) if (r != c) { double f = A[r][c] / A[c][c]; for (int q = c; q <= K; q++) A[r][q] -= f * A[c][q]; }
      }
      for (int a = 0; a < K; a++) coef[h][g][a] = fabs(A[a][a]) > 1e-12 ? (float)(A[a][K] / A[a][a]) : 0.f;
    }
  };
  std::vector<std::vector<uint32_t>> lotErr(G, std::vector<uint32_t>(V.kind == 0 ? NGc : 0));
  std::vector<uint32_t> cnt(G);
  size_t r0 = 0;
  for (size_t k = 0; k < R.slotT.size(); k++) {
    const int64_t T = R.slotT[k];
    // 1. échéances : lots émis à T − h, jugés contre l'anneau rejoué à T
    const auto & ring = R.ring[k];
    for (int h = 0; h < NH && V.kind != 2; h++) {
      const int64_t at = T - (int64_t)HK[h] * 300;
      for (const Lot & L : pend) {
        if (L.t != at) continue;
        for (int g = 0; g < G; g++) { if (V.kind == 0) std::fill(lotErr[g].begin(), lotErr[g].end(), 0u); cnt[g] = 0; }
        for (size_t i = L.from; i < L.to; i++) {
          const Row & r = R.rows[i];
          const int y = r.g < (int)ring.size() ? ring[r.g] : 255;
          if (y == 255 || !informative(r, h)) continue;
          const int g = groupOf(r, h);
          const Feat F = V.feat(r, h);
          const int cur = std::min(r.cur, 254), cap = std::min(r.capE, 255);
          if (V.kind == 0) {
            const int32_t x1 = q64i(F.x[0]), x2 = q64i(F.x[1]), x3 = V.trend ? q64i(F.x[2]) : 0, hi = cap * 1280;
            uint32_t * le = lotErr[g].data();
            for (int ia = 0; ia < GA; ia++)
              for (int ib = 0; ib < GB; ib++) {
                const int32_t base = cur * 1280 + 2 * ia * x1 + ib * x2;
                for (int id = 0; id < ND; id++) {
                  int32_t v = base + (V.trend ? 2 * (id - 5) * x3 : 0);
                  v = v < 0 ? 0 : v > hi ? hi : v;
                  const int32_t d = (v + 640) / 1280 - y;
                  le[(ia * GB + ib) * ND + id] += (uint32_t)(d < 0 ? -d : d);
                }
              }
          } else {
            const double yy = (double)y - cur;
            const int K = V.trend ? 3 : 2;
            for (int a = 0; a < K; a++) { rv[h][g][a] += F.x[a] * yy; for (int b = 0; b < K; b++) Gm[h][g][a * 3 + b] += (double)F.x[a] * F.x[b]; }
          }
          cnt[g]++;
        }
        std::vector<char> touched(NCL, 0);
        for (int g = 0; g < G; g++) {
          if (!cnt[g]) continue;
          if (V.kind == 0) for (int j = 0; j < NGc; j++) err[h][g][j] += (float)lotErr[g][j];
          w[h][g] += (float)cnt[g]; lots[h][g] += 1;
          if (shrink) {
            const int c = g / V.nsub;
            for (int j = 0; j < NGc; j++) errC[h][c][j] += (float)lotErr[g][j];
            wC[h][c] += (float)cnt[g];
            touched[c] = 1;
          } else select(h, g);
        }
        // rétrécissement : le lot compte une fois pour la classe ; la classe ayant bougé, chacune de ses
        // sous-classes rechoisit son couple
        for (int c = 0; c < NCL && shrink; c++)
          if (touched[c]) { lotsC[h][c] += 1; for (int g = c * V.nsub; g < (c + 1) * V.nsub; g++) select(h, g); }
      }
    }
    // 2. quart d'heure noté : prévoir avec les coefficients du moment, puis oublier (classe du lot) et émettre
    if (r0 < N && R.rows[r0].slot == T) {
      size_t r1 = r0;
      while (r1 < N && R.rows[r1].slot == T) r1++;
      for (size_t i = r0; i < r1; i++) {
        const Row & r = R.rows[i];
        for (int h = 0; h < NH; h++) {
          float f = (float)r.cur;
          if (V.kind == 2) f = V.fixed(r, h);
          else if (informative(r, h)) {
            const Feat F = V.feat(r, h);
            const auto & c = coef[h][groupOf(r, h)];
            f = r.cur + c[0] * F.x[0] + c[1] * F.x[1] + (V.trend ? c[2] * F.x[2] : 0.f);
          }
          O.f[i][h] = fminf((float)r.capE, fmaxf(0.f, f));
        }
      }
      if (V.kind != 2) {
        std::array<float, 2 * FC_NC * NH> cc{};
        for (int q = 0; q < NCL && q < FC_NC; q++) for (int h = 0; h < NH; h++) { cc[q * NH + h] = coef[h][q * V.nsub][0]; cc[FC_NC * NH + q * NH + h] = coef[h][q * V.nsub][1]; }
        O.coef[T] = cc;
        const int cl = V.dayClass ? R.rows[r0].cls : 0;
        for (int h = 0; h < NH; h++) {
          for (int g = cl * V.nsub; g < (cl + 1) * V.nsub; g++) {
            for (auto & e : err[h][g]) e *= decay;
            w[h][g] *= decay; lots[h][g] *= decay;
            for (auto & x : Gm[h][g]) x *= decay;
            for (auto & x : rv[h][g]) x *= decay;
          }
          if (shrink) { for (auto & e : errC[h][cl]) e *= decay; wC[h][cl] *= decay; lotsC[h][cl] *= decay; }
        }
        pend.push_back({ T, r0, r1, cl });
        while (pend.size() > (size_t)FC_PEND) pend.erase(pend.begin());
      }
      r0 = r1;
    }
  }
}

// ── Notation ─────────────────────────────────────────────────────────
struct Col { std::string name; std::vector<std::array<int, NH>> p; };   // prévisions arrondies, ‑1 = absente
struct Scored {                                     // rangées notées (présentes en production et au rejeu)
  std::vector<size_t> ri;                           // index dans Run::rows
  std::vector<std::array<int, NH>> y;               // vérité, ‑1 = inconnue
  std::vector<const ProdRow *> pv;
};

static uint64_t s_rng = 1;
static void reseed() { s_rng = 0x9E3779B97F4A7C15ull; }    // chaque tableau tire la même suite : reproductible
static inline uint64_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

// Agrégats par quart d'heure (grappe) : n, Σ|e| et Σe² de chaque colonne → bootstrap par blocs de quarts
// d'heure consécutifs (les stations d'un même instant, et les instants voisins, partagent leurs erreurs).
struct SlotAgg { int64_t slot; long n; std::vector<double> s, q; };
static void aggregate(const Scored & S, const Run & R, const std::vector<Col> & C, int h, std::function<bool(const Row &)> keep,
                      std::vector<SlotAgg> & out) {
  out.clear();
  for (size_t k = 0; k < S.ri.size(); k++) {
    const Row & r = R.rows[S.ri[k]];
    const int y = S.y[k][h];
    if (y < 0 || !keep(r)) continue;
    bool all = true;
    for (const Col & c : C) all &= c.p[k][h] >= 0;
    if (!all) continue;
    if (out.empty() || out.back().slot != r.slot) out.push_back({ r.slot, 0, std::vector<double>(C.size(), 0), std::vector<double>(C.size(), 0) });
    SlotAgg & a = out.back();
    a.n++;
    for (size_t c = 0; c < C.size(); c++) { double e = C[c].p[k][h] - y; a.s[c] += fabs(e); a.q[c] += e * e; }
  }
}
struct Tot { long n = 0; std::vector<double> s, q; double mae(int c) const { return n ? s[c] / n : NAN; } double rmse(int c) const { return n ? sqrt(q[c] / n) : NAN; } };
static Tot totalOf(const std::vector<SlotAgg> & A, size_t nc) {
  Tot t; t.s.assign(nc, 0); t.q.assign(nc, 0);
  for (auto & a : A) { t.n += a.n; for (size_t c = 0; c < nc; c++) { t.s[c] += a.s[c]; t.q[c] += a.q[c]; } }
  return t;
}
static double pctl(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  double x = p * (v.size() - 1); size_t i = (size_t)x;
  return i + 1 < v.size() ? v[i] + (v[i + 1] - v[i]) * (x - i) : v[i];
}
// IC à 95 % (percentiles) de la MAE et de la RMSE de chaque colonne et de leurs écarts appariés (colonne − réf.),
// blocs mobiles de L quarts d'heure consécutifs
struct Boot {
  std::vector<double> lo, hi, rlo, rhi;             // MAE, RMSE par colonne
  std::vector<std::vector<double>> dlo, dhi, drlo, drhi;   // [c][ref] : MAE(c) − MAE(ref), RMSE(c) − RMSE(ref)
};
static Boot blockBoot(const std::vector<SlotAgg> & A, int nc, int L, int B) {
  reseed();
  Boot o;
  o.lo.assign(nc, NAN); o.hi = o.rlo = o.rhi = o.lo;
  o.dlo.assign(nc, std::vector<double>(nc, NAN)); o.dhi = o.drlo = o.drhi = o.dlo;
  const int S = (int)A.size();
  if (S < 2) return o;
  if (L > S) L = S;
  std::vector<std::vector<double>> m(nc, std::vector<double>(B)), r(nc, std::vector<double>(B));
  const int nb = (S + L - 1) / L;
  for (int b = 0; b < B; b++) {
    double n = 0; std::vector<double> s(nc, 0), q(nc, 0);
    for (int j = 0; j < nb; j++) {
      int st = (int)(rnd() % (uint64_t)(S - L + 1));
      for (int x = st; x < st + L; x++) { n += A[x].n; for (int c = 0; c < nc; c++) { s[c] += A[x].s[c]; q[c] += A[x].q[c]; } }
    }
    for (int c = 0; c < nc; c++) { m[c][b] = s[c] / n; r[c][b] = sqrt(q[c] / n); }
  }
  for (int c = 0; c < nc; c++) {
    o.lo[c] = pctl(m[c], 0.025); o.hi[c] = pctl(m[c], 0.975);
    o.rlo[c] = pctl(r[c], 0.025); o.rhi[c] = pctl(r[c], 0.975);
    for (int x = 0; x < nc; x++) {
      std::vector<double> d(B), e(B);
      for (int b = 0; b < B; b++) { d[b] = m[c][b] - m[x][b]; e[b] = r[c][b] - r[x][b]; }
      o.dlo[c][x] = pctl(d, 0.025); o.dhi[c][x] = pctl(d, 0.975);
      o.drlo[c][x] = pctl(e, 0.025); o.drhi[c][x] = pctl(e, 0.975);
    }
  }
  return o;
}
// Bootstrap « par prévision » (tirage indépendant de chaque prévision) — la lecture littérale, trop étroite ici
static void iidBoot(const Scored & S, const Run & R, const std::vector<Col> & C, int h, std::function<bool(const Row &)> keep, int c1, int c0,
                    int B, double & lo, double & hi) {
  reseed();
  std::vector<double> d;
  for (size_t k = 0; k < S.ri.size(); k++) {
    if (S.y[k][h] < 0 || !keep(R.rows[S.ri[k]]) || C[c1].p[k][h] < 0 || C[c0].p[k][h] < 0) continue;
    d.push_back(fabs((double)C[c1].p[k][h] - S.y[k][h]) - fabs((double)C[c0].p[k][h] - S.y[k][h]));
  }
  std::vector<double> m(B);
  const size_t n = d.size();
  for (int b = 0; b < B; b++) { double s = 0; for (size_t i = 0; i < n; i++) s += d[rnd() % n]; m[b] = s / n; }
  lo = pctl(m, 0.025); hi = pctl(m, 0.975);
}
// Cases heure par heure : grappes = quarts d'heure (toutes les stations d'un instant ensemble), tirées
// indépendamment — garde la corrélation entre stations d'un même instant, pas celle entre instants voisins
static void tickBoot(const std::vector<SlotAgg> & A, int c1, int c0, int B, double & mean, double & lo, double & hi, long & n) {
  reseed();
  double s = 0; n = 0;
  for (auto & a : A) { s += a.s[c1] - a.s[c0]; n += a.n; }
  mean = n ? s / n : NAN; lo = hi = NAN;
  const size_t S = A.size();
  if (S < 2) return;
  std::vector<double> m(B);
  for (int b = 0; b < B; b++) {
    double x = 0, c = 0;
    for (size_t i = 0; i < S; i++) { const SlotAgg & a = A[rnd() % S]; x += a.s[c1] - a.s[c0]; c += a.n; }
    m[b] = x / c;
  }
  lo = pctl(m, 0.025); hi = pctl(m, 0.975);
}

static std::string fr(double v, int d = 4) {
  if (!std::isfinite(v)) return "—";
  char b[32]; snprintf(b, sizeof(b), "%.*f", d, v);
  std::string s = b;
  if (s == "-0" || s.find_first_not_of("-0.") == std::string::npos) { s.erase(0, s[0] == '-'); }
  for (auto & c : s) if (c == '.') c = ',';
  return s;
}
static std::string frs(double v, int d = 4) {
  std::string s = fr(v, d);
  if (!std::isfinite(v)) return s;
  if (s[0] == '-') return "−" + s.substr(1);
  return (s.find_first_not_of("0,") == std::string::npos ? "" : "+") + s;
}
static std::string verdictOf(double lo, double hi) {
  if (!std::isfinite(lo)) return "—";
  return hi < 0 ? "**gagne**" : lo > 0 ? "**perd**" : "non concluant";
}
static std::string dayName(int d) {
  int64_t t = g_day0 + (int64_t)d * 86400 + 12 * 3600;
  struct tm lt; plat::localTime(t, &lt);
  static const char * J[] = { "dim", "lun", "mar", "mer", "jeu", "ven", "sam" };
  char b[32]; snprintf(b, sizeof(b), "%s %02d/%02d", J[lt.tm_wday], lt.tm_mday, lt.tm_mon + 1);
  return b;
}

int main(int argc, char ** argv) {
  if (argc < 2) { fprintf(stderr, "usage : bench_modele <données> [--rapport F.md] [--lignes F.tsv] [--tirages B] [--sans-fuite] [--test AAAA-MM-JJ]\n"); return 2; }
  std::string dataDir = argv[1], repPath, tsvPath, testDay = "2026-10-29", blindDir;
  int B = 2000;
  bool leak = false;
  for (int a = 2; a < argc; a++) {
    if (!strcmp(argv[a], "--rapport") && a + 1 < argc) repPath = argv[++a];
    else if (!strcmp(argv[a], "--lignes") && a + 1 < argc) tsvPath = argv[++a];
    else if (!strcmp(argv[a], "--tirages") && a + 1 < argc) B = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--test") && a + 1 < argc) testDay = argv[++a];
    else if (!strcmp(argv[a], "--sans-fuite")) leak = true;
    else if (!strcmp(argv[a], "--aveugle") && a + 1 < argc) blindDir = argv[++a];
    else { fprintf(stderr, "option inconnue : %s\n", argv[a]); return 2; }
  }
  sim::g_fsDir = "/tmp/vn_modele_fs";
  settingsDefaults(g_set);
  FILE * out = repPath.empty() ? stdout : fopen(repPath.c_str(), "w");
  if (!out) { fprintf(stderr, "écriture impossible : %s\n", repPath.c_str()); return 1; }

  Hist H;
  if (!loadHistory(dataDir + "/historique", H)) { fprintf(stderr, "aucun historique lisible dans %s/historique\n", dataDir.c_str()); return 1; }
  std::map<std::pair<int64_t, std::string>, ProdRow> P;
  std::map<int64_t, ProdTick> ticks;
  if (!loadProd(dataDir + "/prevision", P, ticks) || P.empty()) { fprintf(stderr, "aucun journal de prévisions dans %s/prevision\n", dataDir.c_str()); return 1; }
  std::vector<Arch> arch = loadArchives(dataDir + "/archive");
  const int64_t prodEnd = H.slots.back().t;         // dernière tranche de l'historique de production
  int64_t blindStart = INT64_MAX;                   // première tranche de la période à l'aveugle
  int blindSlots = 0;
  if (!blindDir.empty()) {
    blindSlots = loadRaw(blindDir, H, prodEnd);
    if (blindSlots < 0) { fprintf(stderr, "aveugle : dossier illisible %s\n", blindDir.c_str()); return 1; }
    for (auto & x : H.slots) if (x.tRead) { blindStart = x.t; break; }
  }
  {
    struct tm lt; plat::localTime(H.slots.front().t, &lt);
    lt.tm_hour = 12; lt.tm_min = lt.tm_sec = 0; lt.tm_isdst = -1;
    g_day0 = (int64_t)mktime(&lt) - 12 * 3600;
  }
  int testIdx;
  {
    int y, mo, d;
    if (sscanf(testDay.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) { fprintf(stderr, "--test AAAA-MM-JJ\n"); return 2; }
    struct tm lt = {}; lt.tm_year = y - 1900; lt.tm_mon = mo - 1; lt.tm_mday = d; lt.tm_hour = 12; lt.tm_isdst = -1;
    testIdx = dayIndex((int64_t)mktime(&lt));
  }
  Truth T; T.build(H);
  const std::vector<std::vector<int>> nbr = neighbours(H);

  // ── Rejeux du cœur ──
  //   R  : modèle neuf (réglages livrés) — et, au passage, profils comparés à chaque archive de production
  //   R1 : règle 1.3.0 rejouée au pas de 5 min (fidélité du rejeu : la tendance au pas de 1 min n'est pas rejouable)
  //   RG : modèle neuf sans classes de jour (une seule classe : la structure de la session précédente)
  //   RE : modèle neuf, échéances lues sur sa propre prévision (levier etaRule = 2)
  std::vector<ArchCmp> ac(arch.size());
  Run R, R1, RG, RE;
  int wxUseEnd = -1; float wxAlphaEnd = NAN;        // météo apprise à la fin de l'historique de production
  replay(H, ticks, Levers(), Perturb(), R, [&](const Model & m, int64_t slotT) {
    for (size_t k = 0; k < arch.size(); k++)
      if (!ac[k].done && arch[k].savedAt - arch[k].savedAt % 300 == slotT) compareArchive(m, arch[k], ac[k]);
    if (slotT == prodEnd) { wxUseEnd = m.wxUse; wxAlphaEnd = m.wxAlpha; }
  });
  neighbourPass(R, nbr);
  const long bProfN = g_bProfN, bProfBad = g_bProfBad;
  { Levers l; l.rule = FC_RULE_V1; replay(H, ticks, l, Perturb(), R1); }
  { Levers l; l.dayClass = false; replay(H, ticks, l, Perturb(), RG); }
  { Levers l; l.eta = FC_RULE_V2; replay(H, ticks, l, Perturb(), RE); }
  auto sameRows = [](const Run & a, const Run & b) {
    if (a.rows.size() != b.rows.size()) return false;
    for (size_t i = 0; i < a.rows.size(); i++) if (a.rows[i].slot != b.rows[i].slot || a.rows[i].g != b.rows[i].g) return false;
    return true;
  };
  if (!sameRows(R, R1) || !sameRows(R, RG) || !sameRows(R, RE)) { fprintf(stderr, "rejeux non alignés\n"); return 1; }

  // ── Candidats (sur les rangées de R) ──
  auto dP = [](const Row & r, int h) { return r.Ph[h] - r.P0; };
  auto dA = [](const Row & r, int) { return r.P0 - (float)std::min(r.cur, 254); };
  auto trendTerm = [](const Row & r, int h) {                 // tendance 30 min (anneau), amortie comme la règle 1.3.0
    if (!std::isfinite(r.l30)) return 0.f;
    const float slope = fminf(0.6f, fmaxf(-0.6f, (r.cur - r.l30) / 30.f)), hm = 5.f * HK[h];
    return slope * 25.f * (1 - expf(-hm / 25.f));
  };
  std::vector<Variant> V;
  auto base = [&]() { Variant v; v.feat = [&](const Row & r, int h) { Feat f{}; f.x[0] = dP(r, h); f.x[1] = dA(r, h); return f; }; return v; };
  { Variant v = base(); v.name = "réplique du cœur (contrôle)"; V.push_back(v); }
  { Variant v = base(); v.name = "par bloc horaire"; v.note = "4 jeux par classe de jour : 6–10 h, 10–16 h, 16–20 h, 20–22 h"; v.nsub = 4;
    v.sub = [](const Row & r, int) { return r.hour < 10 ? 0 : r.hour < 16 ? 1 : r.hour < 20 ? 2 : 3; }; V.push_back(v); }
  { Variant v = base(); v.name = "par ampleur du mouvement"; v.note = "2 jeux par classe de jour et par horizon : |ΔP| < 2 vélos, ≥ 2"; v.nsub = 2;
    v.sub = [&](const Row & r, int h) { return fabsf(dP(r, h)) >= 2.f ? 1 : 0; }; V.push_back(v); }
  { Variant v = base(); v.name = "pondéré par la confiance"; v.note = "a·c·ΔP + b·c·(P0 − relevé)";
    v.feat = [&](const Row & r, int h) { Feat f{}; f.x[0] = r.cf[h] * dP(r, h); f.x[1] = r.cf[h] * dA(r, h); return f; }; V.push_back(v); }
  { Variant v = base(); v.name = "+ tendance apprise"; v.note = "grille (a, b, d), d·tendance 30 min amortie, d ∈ [−0,5 ; 1]"; v.trend = true;
    v.feat = [&](const Row & r, int h) { Feat f{}; f.x[0] = dP(r, h); f.x[1] = dA(r, h); f.x[2] = trendTerm(r, h); return f; }; V.push_back(v); }
  { Variant v = base(); v.name = "moindres carrés (a, b)"; v.note = "erreur quadratique, rappel λ = 1000 vers la persistance"; v.kind = 1; V.push_back(v); }
  { Variant v; v.kind = 2; v.name = "mélange par la confiance (fixe)"; v.note = "(1 − c)·relevé + c·profil(t + h)";
    v.fixed = [](const Row & r, int h) { return r.has0 && r.hasH[h] ? (1 - r.cf[h]) * r.cur + r.cf[h] * r.Ph[h] : (float)r.cur; }; V.push_back(v); }
  { Variant v; v.kind = 2; v.name = "profil sans confiance (fixe)"; v.note = "relevé + ΔP";
    v.fixed = [&](const Row & r, int h) { return r.has0 && r.hasH[h] ? r.cur + dP(r, h) : (float)r.cur; }; V.push_back(v); }
  { Variant v; v.kind = 2; v.name = "niveau du profil (fixe)"; v.note = "profil(t + h)";
    v.fixed = [](const Row & r, int h) { return r.has0 && r.hasH[h] ? r.Ph[h] : (float)r.cur; }; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : demi-vie 3,5 j"; v.halfLife = 3.5f; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : demi-vie 28 j"; v.halfLife = 28.f; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : seuil 8 quarts d'heure (avant correctif)"; v.minLots = 8; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : seuil 16 quarts d'heure"; v.minLots = 16; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : seuil 32 quarts d'heure"; v.minLots = 32; V.push_back(v); }
  { Variant v = base(); v.name = "sensibilité : seuil 128 quarts d'heure"; v.minLots = 128; V.push_back(v); }
  // Pistes du brief B (docs/mesures-modele.md) : réfutées ou non retenues, gardées au banc avec leurs chiffres
  const int NSG = (int)g_sid.size();
  for (float k : { 50.f, 200.f, 800.f }) {
    Variant v = base(); char b[64]; snprintf(b, sizeof(b), "par station, rétréci κ = %.0f", k); v.name = b;
    v.nsub = NSG; v.sub = [](const Row & r, int) { return r.g; }; v.kappa = k;
    if (k == 200.f) v.note = "un couple par station, classe de jour et horizon, choisi sur les erreurs de la station + κ prévisions fictives de la classe ; κ = 50, 200, 800";
    V.push_back(v);
  }
  { Variant v = base(); v.name = "profil de quartier"; v.trend = true;
    v.note = "grille (a, b, d), d·mouvement prévu moyen des 6 stations les plus proches (part de capacité × capacité de la station)";
    v.feat = [&](const Row & r, int h) { Feat f{}; f.x[0] = dP(r, h); f.x[1] = dA(r, h); f.x[2] = r.nb[h]; return f; }; V.push_back(v); }
  { Variant v = base(); v.name = "pool médian (profil robuste)";
    v.note = "le profil relu avec la médiane des autres jours de même type au lieu de leur moyenne pondérée (identique tant que le pool a ≤ 2 jours)";
    v.feat = [&](const Row & r, int h) { Feat f{}; f.x[0] = r.Phm[h] - r.P0m; f.x[1] = r.P0m - (float)std::min(r.cur, 254); return f; }; V.push_back(v); }
  { Variant v = base(); v.name = "oubli court : demi-vie 1 j"; V.push_back(v); V.back().halfLife = 1.f; }
  { Variant v = base(); v.name = "oubli court : demi-vie 0,25 j (seuil 16)"; v.halfLife = 0.25f; v.minLots = 16;
    v.note = "adaptation à la journée en cours : ≈ 4 h de mémoire"; V.push_back(v); }
  std::vector<VarOut> VO(V.size());
  for (size_t v = 0; v < V.size(); v++) runVariant(V[v], R, VO[v]);

  if (getenv("VN_TRACE_WE")) {                      // trace : couples de la classe week-end, samedi 18 h → dimanche
    for (auto & c : R.coef) {
      struct tm lt; plat::localTime(c.first + 30, &lt);
      if (dayIndex(c.first + 30) < 5 || (dayIndex(c.first + 30) == 5 && lt.tm_hour < 18) || lt.tm_min) continue;
      fprintf(stderr, "%s %02d h :", dayName(dayIndex(c.first + 30)).c_str(), lt.tm_hour);
      for (int h = 0; h < NH; h++) fprintf(stderr, " %s %.1f;%.2f", HL[h], c.second[NH + h], c.second[FC_NC * NH + NH + h]);
      fprintf(stderr, "\n");
    }
  }
  // ── Contrôles du rejeu ──
  long both = 0, curBad = 0, prSame[NH] = {}, v1Same[NH] = {}, repBad = 0, repN = 0, coefBad = 0, etaSameV1 = 0, etaN = 0, nBlind = 0;
  Scored S;
  std::map<int64_t, size_t> ringIx;                 // tranche → index de l'anneau rejoué (vérité à l'aveugle)
  for (size_t k = 0; k < R.slotT.size(); k++) ringIx[R.slotT[k]] = k;
  std::vector<ProdRow> blindPv;                     // colonnes de référence rejouées (adresses stables : réservé)
  blindPv.reserve(R.rows.size());
  for (size_t i = 0; i < R.rows.size(); i++) {
    const Row & r = R.rows[i];
    for (int h = 0; h < NH; h++) { repN++; repBad += memcmp(&VO[0].f[i][h], &r.fc[h], sizeof(float)) != 0; }
    etaN++; etaSameV1 += r.etaE == R1.rows[i].etaE && r.etaF == R1.rows[i].etaF;
    if (r.slot >= blindStart) {
      ProdRow pv;
      pv.v[0] = r.cur;
      for (int h = 0; h < NH; h++) { pv.v[1 + h] = (int)roundf(R1.rows[i].fc[h]); pv.v[6 + h] = (int)roundf(r.pr[h]); }
      pv.v[11] = R1.rows[i].etaE; pv.v[12] = R1.rows[i].etaF; pv.v[13] = r.capE;
      blindPv.push_back(pv);
      S.ri.push_back(i); S.pv.push_back(&blindPv.back());
      std::array<int, NH> y;
      for (int h = 0; h < NH; h++) {
        auto it = ringIx.find(r.slot + (int64_t)HK[h] * 300);
        const int v = it == ringIx.end() || r.g >= (int)R.ring[it->second].size() ? 255 : R.ring[it->second][r.g];
        y[h] = v == 255 ? -1 : v;
      }
      S.y.push_back(y);
      nBlind++;
      continue;
    }
    auto it = P.find({ r.slot, g_sid[r.g] });
    if (it == P.end()) continue;
    both++;
    const ProdRow & pv = it->second;
    curBad += pv.v[0] != r.cur;
    for (int h = 0; h < NH; h++) { prSame[h] += pv.v[6 + h] == (int)roundf(r.pr[h]); v1Same[h] += pv.v[1 + h] == (int)roundf(R1.rows[i].fc[h]); }
    S.ri.push_back(i); S.pv.push_back(&pv);
    std::array<int, NH> y;
    for (int h = 0; h < NH; h++) y[h] = T.at(r.slot + (int64_t)HK[h] * 300, r.g);
    S.y.push_back(y);
  }
  for (auto & c : R.coef) { auto it = VO[0].coef.find(c.first); if (it == VO[0].coef.end() || memcmp(it->second.data(), c.second.data(), sizeof(float) * c.second.size())) coefBad++; }

  // ── Colonnes notées ──
  std::vector<Col> C;
  auto addCol = [&](const std::string & name, std::function<int(size_t, int)> f) {
    Col c; c.name = name; c.p.resize(S.ri.size());
    for (size_t k = 0; k < S.ri.size(); k++) for (int h = 0; h < NH; h++) c.p[k][h] = f(k, h);
    C.push_back(c);
  };
  addCol("persistance", [&](size_t k, int) { return S.pv[k]->v[0]; });
  addCol("profil seul", [&](size_t k, int h) { return S.pv[k]->v[6 + h]; });
  addCol("modèle actuel (1.3.0)", [&](size_t k, int h) { return S.pv[k]->v[1 + h]; });
  addCol("modèle neuf", [&](size_t k, int h) { return (int)roundf(R.rows[S.ri[k]].fc[h]); });
  const int CP = 0, CPR = 1, CM = 2, CN = 3, NBASE = 4;
  addCol("sans classes de jour", [&](size_t k, int h) { return (int)roundf(RG.rows[S.ri[k]].fc[h]); });
  const int CG = 4;
  const int CV0 = (int)C.size();
  for (size_t v = 1; v < V.size(); v++) addCol(V[v].name, [&, v](size_t k, int h) { return (int)roundf(VO[v].f[S.ri[k]][h]); });

  auto inTest = [&](const Row & r) { return r.day >= testIdx && r.slot < blindStart; };
  auto inDev = [&](const Row & r) { return r.day < testIdx; };
  auto all = [&](const Row & r) { return r.slot < blindStart; };
  auto inBlind = [&](const Row & r) { return r.slot >= blindStart; };
  const int Lh[NH] = { 4, 4, 4, 8, 16 };             // longueur des blocs (quarts d'heure) : max(1 h, horizon)

  // ── Rapport ──
  fprintf(out, "# Banc du modèle de prévision — données réelles\n\n");
  {
    struct tm a, b; plat::localTime(H.slots.front().t, &a); plat::localTime(H.slots.back().t, &b);
    char sa[40], sb[40]; strftime(sa, sizeof(sa), "%d/%m/%Y %H:%M", &a); strftime(sb, sizeof(sb), "%d/%m/%Y %H:%M", &b);
    fprintf(out, "Historique : %zu tranches de 5 min, du %s au %s (heure de Luxembourg) ; journal de production : %zu prévisions (quart d'heure × station).  \n",
            H.slots.size(), sa, sb, P.size());
  }
  fprintf(out, "Mise au point : jours < %s ; test : jours ≥ %s, jusqu'à la fin de l'historique de production. Bootstrap : %d tirages, graine fixe.\n\n", dayName(testIdx).c_str(), dayName(testIdx).c_str(), B);
  if (blindSlots > 0) {
    struct tm a, b; plat::localTime(blindStart, &a); plat::localTime(H.slots.back().t, &b);
    char sa[40], sb[40]; strftime(sa, sizeof(sa), "%d/%m/%Y %H:%M", &a); strftime(sb, sizeof(sb), "%d/%m/%Y %H:%M", &b);
    fprintf(out, "Période à l'aveugle : %d tranches de 5 min lues en direct du %s au %s (`%s`), rejouées à la suite de l'historique ; %ld prévisions.\n\n",
            blindSlots, sa, sb, blindDir.c_str(), nBlind);
  }

  fprintf(out, "## 0. Contrôles du rejeu\n\n");
  fprintf(out, "- prévisions communes production / rejeu : %ld ; relevé différent : %ld\n", both, curBad);
  fprintf(out, "- météo à la fin de l'historique de production : α appris = %s, météo %s — la part météo de la prévision est nulle si α = 0 "
               "(wxFactors) : la période à l'aveugle, rejouée sans météo, se prévoit alors comme la production l'aurait fait\n",
          fr(wxAlphaEnd, 4).c_str(), wxUseEnd == WXU_OK ? "fraîche" : "absente ou périmée");
  fprintf(out, "- profil seul identique à la production :");
  for (int h = 0; h < NH; h++) fprintf(out, " %s %s %%", HL[h], fr(100.0 * prSame[h] / std::max(1L, both), 2).c_str());
  fprintf(out, "\n- règle 1.3.0 rejouée au pas de 5 min, identique à la production (tendance au pas de 1 min non rejouable) :");
  for (int h = 0; h < NH; h++) fprintf(out, " %s %s %%", HL[h], fr(100.0 * v1Same[h] / std::max(1L, both), 1).c_str());
  fprintf(out, "\n- réplique de banc ≠ cœur : %ld prévisions sur %ld ; couples appliqués différents : %ld quarts d'heure sur %zu\n", repBad, repN, coefBad, R.coef.size());
  fprintf(out, "- échéances vide / pleine du modèle neuf identiques à celles de la règle 1.3.0 rejouée : %ld sur %ld\n", etaSameV1, etaN);
  fprintf(out, "- profil relu par le banc (copie de profileLocal, candidat « pool médian » sans médiane) différent du cœur : %ld lectures sur %ld\n", bProfBad, bProfN);
  fprintf(out, "- `/vn_prof.bin` en fin de rejeu (%d stations) : %zu octets (v5) ; la même chose en v4 : %zu octets ; règle 1.3.0 rejouée : %zu octets\n\n",
          (int)g_sid.size(), R.profBytes, R.profBytes - sizeof(FcLearn), R1.profBytes);
  fprintf(out, "Profils du rejeu contre les profils archivés par la production (cellule = moyenne ×256, n, dispersion, compte de dispersion ; 672 par station) :\n\n");
  fprintf(out, "| archive | relevés appris (production / rejeu) | stations comparées | stations identiques | cellules identiques | moyenne et n identiques |\n|---|---|---|---|---|---|\n");
  for (auto & c : ac) {
    if (!c.done) { fprintf(out, "| %s | — | non atteinte par le rejeu | | | |\n", c.name.c_str()); continue; }
    fprintf(out, "| %s | %u / %u | %ld | %ld | %ld / %ld (%s %%) | %ld (%s %%) |\n", c.name.c_str(), c.samplesProd, c.samplesRep, c.stations, c.stSame,
            c.cellSame, c.cells, fr(100.0 * c.cellSame / std::max(1L, c.cells), 3).c_str(), c.meanSame, fr(100.0 * c.meanSame / std::max(1L, c.cells), 3).c_str());
  }
  fprintf(out, "\n");

  auto mainTable = [&](const char * title, std::function<bool(const Row &)> keep) {
    fprintf(out, "## %s\n\n", title);
    std::vector<Boot> bo(NH);
    std::vector<Tot> tt(NH);
    for (int h = 0; h < NH; h++) {
      std::vector<SlotAgg> ag; aggregate(S, R, C, h, keep, ag);
      bo[h] = blockBoot(ag, NBASE + 1, Lh[h], B);     // les 4 colonnes et « sans classes de jour »
      tt[h] = totalOf(ag, C.size());
    }
    fprintf(out, "MAE (vélos) [IC 95 %%, blocs de quarts d'heure]\n\n| horizon | n | persistance | profil seul | modèle actuel (1.3.0) | modèle neuf |\n|---|---|---|---|---|---|\n");
    for (int h = 0; h < NH; h++) {
      fprintf(out, "| %s min | %ld |", HL[h], tt[h].n);
      for (int c = 0; c < NBASE; c++) fprintf(out, " %s [%s ; %s] |", fr(tt[h].mae(c)).c_str(), fr(bo[h].lo[c], 3).c_str(), fr(bo[h].hi[c], 3).c_str());
      fprintf(out, "\n");
    }
    fprintf(out, "\nRMSE (vélos) [IC 95 %%]\n\n| horizon | n | persistance | profil seul | modèle actuel (1.3.0) | modèle neuf |\n|---|---|---|---|---|---|\n");
    for (int h = 0; h < NH; h++) {
      fprintf(out, "| %s min | %ld |", HL[h], tt[h].n);
      for (int c = 0; c < NBASE; c++) fprintf(out, " %s [%s ; %s] |", fr(tt[h].rmse(c)).c_str(), fr(bo[h].rlo[c], 3).c_str(), fr(bo[h].rhi[c], 3).c_str());
      fprintf(out, "\n");
    }
    fprintf(out, "\nÉcart apparié de MAE, modèle neuf − référence [IC 95 %%] (négatif = le modèle neuf se trompe moins) :\n\n| horizon | − persistance | − profil seul | − modèle actuel |\n|---|---|---|---|\n");
    for (int h = 0; h < NH; h++) {
      fprintf(out, "| %s min |", HL[h]);
      for (int ref : { CP, CPR, CM }) {
        const double d = tt[h].mae(CN) - tt[h].mae(ref);
        fprintf(out, " %s [%s ; %s] (%s %%) %s |", frs(d).c_str(), frs(bo[h].dlo[CN][ref]).c_str(), frs(bo[h].dhi[CN][ref]).c_str(),
                frs(100 * d / tt[h].mae(ref), 1).c_str(), verdictOf(bo[h].dlo[CN][ref], bo[h].dhi[CN][ref]).c_str());
      }
      fprintf(out, "\n");
    }
    fprintf(out, "\nÉcart apparié de RMSE, modèle neuf − référence [IC 95 %%] :\n\n| horizon | − persistance | − profil seul | − modèle actuel |\n|---|---|---|---|\n");
    for (int h = 0; h < NH; h++) {
      fprintf(out, "| %s min |", HL[h]);
      for (int ref : { CP, CPR, CM }) {
        const double d = tt[h].rmse(CN) - tt[h].rmse(ref);
        fprintf(out, " %s [%s ; %s] (%s %%) %s |", frs(d).c_str(), frs(bo[h].drlo[CN][ref]).c_str(), frs(bo[h].drhi[CN][ref]).c_str(),
                frs(100 * d / tt[h].rmse(ref), 1).c_str(), verdictOf(bo[h].drlo[CN][ref], bo[h].drhi[CN][ref]).c_str());
      }
      fprintf(out, "\n");
    }
    fprintf(out, "\n");
  };
  mainTable("1. Période de test", inTest);
  if (nBlind) mainTable("1b. Période à l'aveugle (collectée après le gel du modèle ; « modèle actuel » = règle 1.3.0 rejouée au pas de 5 min)", inBlind);
  mainTable("2. Période de mise au point (les choix y ont été faits)", inDev);
  mainTable("3. Toute la période (pour information : mêle mise au point et test)", all);

  // ── Largeur des IC ──
  fprintf(out, "## 4. Largeur des IC selon le rééchantillonnage (test)\n\n");
  fprintf(out, "| horizon | écart | par prévision (indépendantes) | blocs de L quarts d'heure | blocs de 2L |\n|---|---|---|---|---|\n");
  for (int h = 0; h < NH; h++) {
    std::vector<SlotAgg> ag; aggregate(S, R, C, h, inTest, ag);
    Boot b1 = blockBoot(ag, NBASE, Lh[h], B), b2 = blockBoot(ag, NBASE, 2 * Lh[h], B);
    for (int ref : { CP, CPR }) {
      double lo, hi; iidBoot(S, R, C, h, inTest, CN, ref, B, lo, hi);
      fprintf(out, "| %s min | neuf − %s | [%s ; %s] | [%s ; %s] (L = %d) | [%s ; %s] |\n", HL[h], C[ref].name.c_str(), frs(lo).c_str(), frs(hi).c_str(),
              frs(b1.dlo[CN][ref]).c_str(), frs(b1.dhi[CN][ref]).c_str(), Lh[h], frs(b2.dlo[CN][ref]).c_str(), frs(b2.dhi[CN][ref]).c_str());
    }
  }
  fprintf(out, "\n");

  // ── Par jour ──
  fprintf(out, "## 5. Par jour — MAE persistance / profil seul / modèle actuel / **modèle neuf**\n\n| jour | n (+60) |");
  for (int h = 0; h < NH; h++) fprintf(out, " %s min |", HL[h]);
  fprintf(out, "\n|---|---|---|---|---|---|---|\n");
  int dmax = 0; for (auto & r : R.rows) dmax = std::max(dmax, r.day);
  for (int d = 0; d <= dmax; d++) {
    bool blindDay = false;
    for (auto & r : R.rows) if (r.day == d && r.slot >= blindStart) { blindDay = true; break; }
    fprintf(out, "| %s%s |", dayName(d).c_str(), blindDay ? " (aveugle)" : d >= testIdx ? " (test)" : "");
    for (int h = 0; h < NH; h++) {
      std::vector<SlotAgg> ag; aggregate(S, R, C, h, [&](const Row & r) { return r.day == d; }, ag);
      Tot t = totalOf(ag, C.size());
      if (h == 0) { std::vector<SlotAgg> a60; aggregate(S, R, C, 2, [&](const Row & r) { return r.day == d; }, a60); fprintf(out, " %ld |", totalOf(a60, C.size()).n); }
      if (!t.n) { fprintf(out, " — |"); continue; }
      fprintf(out, " %s / %s / %s / **%s** |", fr(t.mae(CP), 3).c_str(), fr(t.mae(CPR), 3).c_str(), fr(t.mae(CM), 3).c_str(), fr(t.mae(CN), 3).c_str());
    }
    fprintf(out, "\n");
  }
  fprintf(out, "\n");

  // ── Par heure (test) ──
  for (int per = 0; per < (nBlind ? 2 : 1); per++)
  for (int ref : { CP, CPR, CM }) {
    std::function<bool(const Row &)> inPer = per ? std::function<bool(const Row &)>(inBlind) : std::function<bool(const Row &)>(inTest);
    fprintf(out, "## 6%s. Par heure de la journée (%s) — écart de MAE modèle neuf − %s [IC 95 %%, grappes = quarts d'heure]\n\n| heure | n (+60) |",
            per ? (ref == CP ? "d" : ref == CPR ? "e" : "f") : (ref == CP ? "a" : ref == CPR ? "b" : "c"), per ? "aveugle" : "test", C[ref].name.c_str());
    for (int h = 0; h < NH; h++) fprintf(out, " %s min |", HL[h]);
    fprintf(out, "\n|---|---|---|---|---|---|---|\n");
    for (int hr = 6; hr < 22; hr++) {
      auto keep = [&](const Row & r) { return inPer(r) && r.hour == hr; };
      fprintf(out, "| %02d h |", hr);
      for (int h = 0; h < NH; h++) {
        std::vector<SlotAgg> ag; aggregate(S, R, C, h, keep, ag);
        double m, lo, hi; long n;
        tickBoot(ag, CN, ref, B, m, lo, hi, n);
        if (h == 0) { std::vector<SlotAgg> a60; aggregate(S, R, C, 2, keep, a60); fprintf(out, " %ld |", totalOf(a60, C.size()).n); }
        const char * mark = hi < 0 ? " ✓" : lo > 0 ? " ✗" : "";
        fprintf(out, " %s [%s ; %s]%s |", frs(m, 3).c_str(), frs(lo, 3).c_str(), frs(hi, 3).c_str(), mark);
      }
      fprintf(out, "\n");
    }
    fprintf(out, "\n✓ = le modèle neuf gagne (IC < 0), ✗ = il perd (IC > 0), sinon non concluant. Grappes = quarts d'heure tirés indépendamment : la corrélation entre quarts d'heure voisins n'est pas prise en compte, ces IC sont plus étroits que ceux des sections 1 et 2.\n\n");
  }

  // ── Candidats ──
  auto candTable = [&](const char * title, std::function<bool(const Row &)> keep) {
    fprintf(out, "## %s\n\nMAE · écart de MAE au modèle neuf [IC 95 %%, blocs] (positif = le candidat se trompe plus que le modèle neuf)\n\n| candidat |", title);
    for (int h = 0; h < NH; h++) fprintf(out, " %s min |", HL[h]);
    fprintf(out, "\n|---|---|---|---|---|---|\n");
    std::vector<Tot> tt(NH); std::vector<Boot> bo(NH);
    for (int h = 0; h < NH; h++) {
      std::vector<SlotAgg> ag; aggregate(S, R, C, h, keep, ag);
      tt[h] = totalOf(ag, C.size()); bo[h] = blockBoot(ag, (int)C.size(), Lh[h], B / 4);
    }
    for (int c = CG; c < (int)C.size(); c++) {
      fprintf(out, "| %s |", C[c].name.c_str());
      for (int h = 0; h < NH; h++)
        fprintf(out, " %s · %s [%s ; %s] |", fr(tt[h].mae(c)).c_str(), frs(tt[h].mae(c) - tt[h].mae(CN)).c_str(), frs(bo[h].dlo[c][CN]).c_str(), frs(bo[h].dhi[c][CN]).c_str());
      fprintf(out, "\n");
    }
    fprintf(out, "\n");
  };
  candTable("7a. Candidats — mise au point", inDev);
  candTable("7b. Candidats — test", inTest);
  candTable("7c. Candidats — dimanche 01/11 (férié, 1er dimanche : profil du week-end tiré du seul samedi)", [&](const Row & r) { return r.day == 6; });
  fprintf(out, "Candidats : *sans classes de jour* = modèle neuf avec une seule classe (rejeu du cœur, levier `fcDayClass`).");
  for (size_t v = 1; v < V.size(); v++) if (!V[v].note.empty()) fprintf(out, " *%s* : %s.", V[v].name.c_str(), V[v].note.c_str());
  fprintf(out, "\n\n");

  // ── Profil inconnu : le modèle actuel n'y est que sa tendance ──
  fprintf(out, "## 8. Prévisions sans profil (c = 0 : lundi 26/10, samedi 31/10) — la colonne « modèle actuel » y est la tendance seule\n\n");
  fprintf(out, "| horizon | n | persistance | modèle actuel = relevé + tendance amortie | écart [IC 95 %%, blocs] |\n|---|---|---|---|---|\n");
  for (int h = 0; h < NH; h++) {
    std::vector<SlotAgg> ag; aggregate(S, R, C, h, [&](const Row & r) { return !(r.has0 && r.hasH[h]) && (r.day == 0 || r.day == 5); }, ag);
    Tot t = totalOf(ag, C.size()); Boot b = blockBoot(ag, NBASE, Lh[h], B);
    fprintf(out, "| %s min | %ld | %s | %s | %s [%s ; %s] (%s %%) |\n", HL[h], t.n, fr(t.mae(CP)).c_str(), fr(t.mae(CM)).c_str(), frs(t.mae(CM) - t.mae(CP)).c_str(),
            frs(b.dlo[CM][CP]).c_str(), frs(b.dhi[CM][CP]).c_str(), frs(100 * (t.mae(CM) / t.mae(CP) - 1), 1).c_str());
  }
  fprintf(out, "\n");

  // ── Alertes « vide / pleine dans l'heure » (règle de ForecastLog.score) ──
  fprintf(out, "## 9. Alertes « vide / pleine dans l'heure » (échéance ≤ 60 min), règle de la note du serveur\n\n");
  fprintf(out, "| période | échéances | vide : annoncées / confirmées (précision) | vide : réelles / annoncées (rappel) | pleine : précision | pleine : rappel |\n|---|---|---|---|---|---|\n");
  for (int per = 0; per < 2; per++)
    for (int which = 0; which < 3; which++) {
      long eP = 0, eH = 0, eR = 0, eC = 0, fP = 0, fH = 0, fR = 0, fC = 0;
      for (size_t k = 0; k < S.ri.size(); k++) {
        const Row & r = R.rows[S.ri[k]];
        if (r.slot >= blindStart || (per == 0) != inTest(r)) continue;
        const ProdRow & pv = *S.pv[k];
        const Row & re = RE.rows[S.ri[k]];
        const int etaE = which == 0 ? pv.v[11] : which == 1 ? r.etaE : re.etaE, etaF = which == 0 ? pv.v[12] : which == 1 ? r.etaF : re.etaF;
        const int cap = pv.v[13], cur = pv.v[0];
        bool sawE = false, sawF = false; int known = 0;
        for (int q = 1; q <= 12; q++) {
          int x = T.at(r.slot + (int64_t)q * 300, r.g);
          if (x < 0) continue;
          known++;
          if (x == 0) sawE = true;
          if (cap > 0 && x >= cap) sawF = true;
        }
        if (known < 6) continue;
        const bool pE = etaE > 0 && etaE <= 60, pF = etaF > 0 && etaF <= 60;
        if (cur > 0) { if (pE) { eP++; eH += sawE; } if (sawE) { eR++; eC += pE; } }
        if (cap > 0 && cur < cap) { if (pF) { fP++; fH += sawF; } if (sawF) { fR++; fC += pF; } }
      }
      static const char * W[] = { "production 1.3.0 (enregistrées)", "modèle neuf : trajectoire 1.3.0 (livré)", "lues sur la prévision du modèle neuf (non livré)" };
      fprintf(out, "| %s | %s | %ld / %ld (%s %%) | %ld / %ld (%s %%) | %ld / %ld (%s %%) | %ld / %ld (%s %%) |\n", per ? "mise au point" : "test",
              W[which], eP, eH, fr(100.0 * eH / std::max(1L, eP), 1).c_str(), eR, eC, fr(100.0 * eC / std::max(1L, eR), 1).c_str(),
              fP, fH, fr(100.0 * fH / std::max(1L, fP), 1).c_str(), fR, fC, fr(100.0 * fC / std::max(1L, fR), 1).c_str());
    }
  fprintf(out, "\nLa trajectoire 1.3.0 du rejeu a une tendance au pas de 5 min (l'anneau) ; sur la carte et le serveur, au pas du relevé (60 s), exactement comme en production.\n\n");

  // ── Coefficients appris par le cœur ──
  fprintf(out, "## 10. Couples (a ; b) appliqués par le cœur, dernier quart d'heure noté de chaque jour\n\n| jour | classe |");
  for (int h = 0; h < NH; h++) fprintf(out, " %s min |", HL[h]);
  fprintf(out, "\n|---|---|---|---|---|---|---|\n");
  {
    std::map<int, std::pair<int64_t, std::array<float, 2 * FC_NC * NH>>> last;
    for (auto & c : R.coef) last[dayIndex(c.first + 30)] = c;
    for (auto & d : last)
      for (int q = 0; q < FC_NC; q++) {
        fprintf(out, "| %s | %s |", dayName(d.first).c_str(), q ? "week-end et férié" : "ouvré");
        for (int h = 0; h < NH; h++) fprintf(out, " %s ; %s |", fr(d.second.second[q * NH + h], 1).c_str(), fr(d.second.second[FC_NC * NH + q * NH + h], 2).c_str());
        fprintf(out, "\n");
      }
  }
  fprintf(out, "\n");

  // ── Non-fuite ──
  if (leak) {
    fprintf(out, "## 11. Contrôle de non-fuite temporelle\n\n");
    fprintf(out, "Chaque rejeu ci-dessous ne voit pas l'historique après l'instant de coupe T (tronqué), ou le voit **brouillé** "
                 "(chaque relevé postérieur à T remplacé par une valeur quelconque, chaque capacité du journal postérieure à T décalée). "
                 "Toute prévision émise à t ≤ T doit être identique au bit près à celle du rejeu complet : modèle neuf, sans classes de jour, "
                 "profil seul, échéances, et chaque candidat.\n\n"
                 "| coupe T | rejeu | prévisions émises à t ≤ T comparées | différentes | rangées manquantes |\n|---|---|---|---|---|\n");
    std::vector<std::string> cuts = { "2026-10-27 12:00", "2026-10-29 00:00", "2026-10-30 17:00", "2026-10-31 09:30", "2026-11-01 10:00" };
    if (nBlind) {                                   // et au milieu de la période à l'aveugle
      const int64_t mid = blindStart + (H.slots.back().t - blindStart) / 2;
      struct tm lt; plat::localTime(mid - mid % 900, &lt);
      char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &lt); cuts.push_back(b);
    }
    for (const std::string & cstr : cuts) {
      const char * cs = cstr.c_str();
      int y, mo, d, hh, mi;
      sscanf(cs, "%d-%d-%d %d:%d", &y, &mo, &d, &hh, &mi);
      struct tm lt = {}; lt.tm_year = y - 1900; lt.tm_mon = mo - 1; lt.tm_mday = d; lt.tm_hour = hh; lt.tm_min = mi; lt.tm_isdst = -1;
      const int64_t cut = (int64_t)mktime(&lt);
      for (int mode = 0; mode < 2; mode++) {
        Perturb pt;
        Hist H2 = H;
        std::map<int64_t, ProdTick> t2 = ticks;
        if (mode == 0) pt.cut = cut;
        else {
          for (auto & s : H2.slots) if (s.t > cut) {
            for (size_t i = 0; i < s.b.size(); i++) if (s.b[i] >= 0) s.b[i] = scrambleB(H2.lists[s.list][i].cap, s.t, (int)i);
            for (size_t i = 0; i < s.raw.size(); i++) {      // relevés bruts (période à l'aveugle) : brouillés aussi
              StatusRec & r = s.raw[i];
              const int cap = r.bikes + r.docks, b = scrambleB(cap, s.t, (int)i);
              r.bikes = (uint16_t)b; r.elec = (uint16_t)(r.elec < b ? r.elec : b); r.docks = (uint16_t)(cap - b);
            }
          }
          for (auto & k : t2) if (k.first > cut) for (auto & c : k.second.capE) if (c >= 0) c = c + 3;
        }
        Run R2, RG2;
        replay(H2, t2, Levers(), pt, R2);
        neighbourPass(R2, nbr);
        { Levers l; l.dayClass = false; replay(H2, t2, l, pt, RG2); }
        std::vector<VarOut> VO2(V.size());
        for (size_t v = 0; v < V.size(); v++) runVariant(V[v], R2, VO2[v]);
        std::map<std::pair<int64_t, int>, size_t> ix;
        for (size_t i = 0; i < R2.rows.size(); i++) ix[{ R2.rows[i].slot, R2.rows[i].g }] = i;
        long cmp = 0, diff = 0, miss = 0;
        for (size_t i = 0; i < R.rows.size(); i++) {
          const Row & a = R.rows[i];
          if (a.slot > cut) continue;
          auto it = ix.find({ a.slot, a.g });
          if (it == ix.end()) { miss++; continue; }
          const Row & b = R2.rows[it->second];
          bool same = a.cur == b.cur && a.capE == b.capE && a.etaE == b.etaE && a.etaF == b.etaF;
          for (int h = 0; h < NH; h++) {
            same &= !memcmp(&a.fc[h], &b.fc[h], sizeof(float)) && !memcmp(&a.pr[h], &b.pr[h], sizeof(float))
                    && !memcmp(&RG.rows[i].fc[h], &RG2.rows[it->second].fc[h], sizeof(float));
            for (size_t v = 0; v < V.size(); v++) same &= !memcmp(&VO[v].f[i][h], &VO2[v].f[it->second][h], sizeof(float));
          }
          cmp++; diff += !same;
        }
        fprintf(out, "| %s | %s | %ld | %ld | %ld |\n", cs, mode ? "futur brouillé" : "historique tronqué", cmp, diff, miss);
        fflush(out);
      }
    }
    fprintf(out, "\n");
  }

  if (!tsvPath.empty()) {
    FILE * f = fopen(tsvPath.c_str(), "w");
    if (f) {
      fprintf(f, "slot\tsid\tjour\theure\tminute\tclasse\tcur\tcapE\thas0\tP0\tl15\tl30\tetaE\tetaF\tprodEtaE\tprodEtaF");
      for (int q = 1; q <= 12; q++) fprintf(f, "\tP5_%d", q);
      for (int q = 1; q <= 12; q++) fprintf(f, "\tY5_%d", q);
      for (int h = 0; h < NH; h++) fprintf(f, "\tverite%s\tpers%s\tprofil%s\tactuel%s\tneuf%s\thasH%s\tPh%s\tcf%s", HL[h], HL[h], HL[h], HL[h], HL[h], HL[h], HL[h], HL[h]);
      fprintf(f, "\n");
      for (size_t k = 0; k < S.ri.size(); k++) {
        const Row & r = R.rows[S.ri[k]];
        struct tm lt; plat::localTime(r.t, &lt);
        fprintf(f, "%lld\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.4f\t%.0f\t%.0f\t%d\t%d\t%d\t%d", (long long)r.slot, g_sid[r.g].c_str(), r.day, r.hour, lt.tm_min,
                r.cls, r.cur, r.capE, (int)r.has0, r.P0, r.l15, r.l30, r.etaE, r.etaF, S.pv[k]->v[11], S.pv[k]->v[12]);
        for (int q = 1; q <= 12; q++) fprintf(f, "\t%.4f", r.P5[q]);
        for (int q = 1; q <= 12; q++) fprintf(f, "\t%d", T.at(r.slot + (int64_t)q * 300, r.g));
        for (int h = 0; h < NH; h++)
          fprintf(f, "\t%d\t%d\t%d\t%d\t%d\t%d\t%.4f\t%.4f", S.y[k][h], C[CP].p[k][h], C[CPR].p[k][h], C[CM].p[k][h], C[CN].p[k][h],
                  (int)r.hasH[h], r.Ph[h], r.cf[h]);
        fprintf(f, "\n");
      }
      fclose(f);
    }
  }
  if (out != stdout) fclose(out);
  fprintf(stderr, "%zu tranches rejouées, %zu rangées, %zu notées ; relevé ≠ production : %ld ; réplique ≠ cœur : %ld ; candidats : %d colonnes\n",
          H.slots.size(), R.rows.size(), S.ri.size(), curBad, repBad, (int)C.size() - CV0 + 1);
  return 0;
}
