// ── Banc de mesure de la prévision (sans UI) ─────────────────────────
// Rejoue un parc synthétique de 143 stations sur plusieurs mois, avec une météo
// synthétique, à travers le VRAI modèle (src/core/model.cpp) tel qu'il tourne sur
// la carte : applyStatus() toutes les 5 min, Weather rafraîchie toutes les 15 min.
//
//   make bench_prevision
//   ./bench_prevision [--jours N] [--graine S] [--effet nul|moyen|fort] [--meteo ok|absente]
//                     [--levier nom=0|1]… [--court]
//
// Ce qu'il mesure (voir docs/mesures-prevision.md) :
//   1. MAE de la prévision à +15 / +30 / +60 / +120 min, par temps sec et par temps de pluie,
//      contre la persistance (« rien ne bouge ») et contre un ORACLE qui connaît l'effet exact
//      de la météo sur la demande (plafond de ce qu'un terme météo pourrait gagner) ;
//   2. les alertes « inhabituel » par jour de pluie / jour sec, séparées en vraies (incident
//      injecté) et fausses, et le rappel sur les incidents ;
//   3. la maturation du profil (couverture learnPct, confiance moyenne) au fil des jours ;
//   4. l'absorption d'un agrandissement 20 → 30 bornes (station jumelle de 30 bornes témoin) ;
//   5. le bruit : apparitions d'alertes par jour, par type.
//
// La vérité (niveaux des stations) ne dépend JAMAIS du modèle : à graine égale, deux
// réglages du modèle voient exactement le même monde, et leurs écarts sont appariés.
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/weather.h"
#include "../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <string>

namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern const char * g_fsDir; }
using namespace vn;

// ── Aléa déterministe (un flux par usage : la vérité ne dépend pas de l'ordre des appels) ──
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) { next(); next(); }
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  double u() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  double n() { double a = u() + 1e-12, b = u(); return sqrt(-2 * log(a)) * cos(6.283185307 * b); }
  int poisson(double l) {
    if (l <= 0) return 0;
    if (l > 30) { int k = (int)lround(l + sqrt(l) * n()); return k < 0 ? 0 : k; }
    double L = exp(-l), p = 1; int k = 0;
    do { k++; p *= u(); } while (p > L);
    return k - 1;
  }
};
static double hash01(uint64_t a, uint64_t b) { Rng r(a * 1000003ull + b * 7919ull + 17); return r.u(); }

// ── Calendrier ───────────────────────────────────────────────────────
static int64_t lux(int y, int mo, int d, int h = 0, int mi = 0) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  return (int64_t)mktime(&t);
}
// Jours fériés légaux du Luxembourg (fixes + mobiles autour de Pâques)
static void easter(int y, int & mo, int & d) {
  int a = y % 19, b = y / 100, c = y % 100, dd = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
  int h = (19 * a + b - dd - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
  int m = (a + 11 * h + 22 * l) / 451;
  mo = (h + l - 7 * m + 114) / 31; d = ((h + l - 7 * m + 114) % 31) + 1;
}
static bool isHoliday(const struct tm & lt) {
  int y = lt.tm_year + 1900, mo = lt.tm_mon + 1, d = lt.tm_mday;
  static const int FIX[][2] = { { 1, 1 }, { 5, 1 }, { 5, 9 }, { 6, 23 }, { 8, 15 }, { 11, 1 }, { 12, 25 }, { 12, 26 } };
  for (auto & f : FIX) if (f[0] == mo && f[1] == d) return true;
  int em, ed; easter(y, em, ed);
  struct tm e = {}; e.tm_year = y - 1900; e.tm_mon = em - 1; e.tm_mday = ed; e.tm_hour = 12; e.tm_isdst = -1;
  time_t te = mktime(&e);
  for (int off : { 1, 39, 50 }) {                 // lundi de Pâques, Ascension, lundi de Pentecôte
    time_t x = te + (time_t)off * 86400; struct tm lx; localtime_r(&x, &lx);
    if (lx.tm_mon + 1 == mo && lx.tm_mday == d) return true;
  }
  return false;
}

// ── Monde synthétique ────────────────────────────────────────────────
enum Kind { K_RES, K_BUR, K_GARE, K_ECOLE, K_PARC, K_MIXTE, K_COUNT };
static const char * KNAME[K_COUNT] = { "résidentiel", "bureaux", "gare", "école", "parc", "mixte" };
// Sensibilité de la demande à la météo, par type (le parc, loisir, y est le plus sensible)
static const float KSENS[K_COUNT] = { 0.45f, 0.40f, 0.35f, 0.25f, 0.90f, 0.60f };

static inline float S(float x, float a, float b) {
  if (x <= a) return 0;
  if (x >= b) return 1;
  float t = (x - a) / (b - a); return t * t * (3 - 2 * t);
}
// Remplissage « cible » (fraction de la capacité) : courbe de forme par type, jour ouvré / week-end
static float shapeF(int k, bool weekend, int dow, float h) {
  if (!weekend) switch (k) {
    case K_RES:   return 0.80f - 0.60f * S(h, 7, 9) + 0.60f * S(h, 17, 20);
    case K_BUR:   return 0.20f + 0.65f * S(h, 7.5f, 9.5f) - 0.65f * S(h, 16.5f, 19);
    case K_GARE:  return 0.60f - 0.45f * S(h, 6.5f, 8.5f) + 0.45f * S(h, 16.5f, 18.5f);
    case K_ECOLE: return dow == 3 ? 0.30f + 0.50f * S(h, 7.5f, 8.2f) - 0.50f * S(h, 12, 12.6f)
                                  : 0.30f + 0.50f * S(h, 7.5f, 8.2f) - 0.50f * S(h, 15.8f, 16.5f);
    case K_PARC:  return 0.40f + 0.15f * S(h, 11.5f, 12.5f) - 0.15f * S(h, 13.5f, 14.5f);
    default:      return 0.50f + 0.12f * sinf(6.2832f * (h - 9) / 24);
  }
  switch (k) {
    case K_RES:   return 0.80f - 0.30f * S(h, 10, 13) + 0.30f * S(h, 17, 20);
    case K_BUR:   return 0.20f + 0.10f * S(h, 10, 12) - 0.10f * S(h, 17, 19);
    case K_GARE:  return 0.60f - 0.15f * S(h, 9, 11) + 0.15f * S(h, 17, 19);
    case K_ECOLE: return 0.30f;
    case K_PARC:  return 0.30f + 0.55f * S(h, 11, 15) - 0.55f * S(h, 16, 19);
    default:      return 0.50f + 0.15f * S(h, 13, 15) - 0.15f * S(h, 17, 18.5f);
  }
}
// Activité brute (trajets par heure, en fraction de la capacité), hors flux net
static float activity(int k, bool weekend, float h) {
  float a = h < 6 ? 0.01f : 0.04f;
  if (!weekend) a += 0.08f * expf(-powf((h - 8.2f) / 1.1f, 2)) + 0.07f * expf(-powf((h - 17.8f) / 1.4f, 2))
                     + 0.03f * expf(-powf((h - 12.5f) / 1.2f, 2));
  else a += 0.05f * expf(-powf((h - 15) / 3, 2));
  if (k == K_PARC && weekend) a *= 1.8f;
  if (k == K_GARE) a *= 1.5f;
  return a;
}

struct Hour { float rain, temp, wind; int code; float bad; };
// Pénalité « vraie » de la météo sur la demande (0 = idéal, 1 = exécrable)
static float trueBad(float rain, float temp, float wind, int code) {
  float b = 0;
  if (rain >= 0.1f) b += fminf(0.85f, 0.35f + 0.25f * rain);
  if (wind > 25) b += fminf(0.2f, (wind - 25) / 50);
  if (temp < 2) b += temp < -2 ? 0.25f : 0.12f;
  if (code >= 95) b += 0.2f;
  return fminf(1.f, b);
}

struct SimSt {
  int kind, cap, capFrom = 0, capChangeDay = -1;
  float amp, phase, sens, mAvg = 1;
  int bikes;
  int64_t badSince = 0;            // vide ou pleine depuis (camion de régulation)
  bool closed = false;
  char sid[8];
  float lat, lon;
};

struct Cfg {
  int days = 168;
  uint64_t seed = 1;
  float effect = 1.f;              // 0 = la pluie ne change rien à la demande
  const char * effName = "moyen";
  bool wxAvailable = true;
  bool brief = false;
  bool perfectFc = false;          // prévision météo = la vérité (plafond de l'étape 4)
};

static const int NST = 143;
static const int CHANGED = NST - 2, TWIN = NST - 1, CLOSED_ST = 7;
static const int CHANGE_DAY = 56;

struct World {
  Cfg cfg;
  int64_t T0;
  std::vector<Hour> hours;          // vérité météo horaire (jours + 6 h de marge)
  SimSt st[NST];
  Rng rFlow{ 1 }, rInc{ 2 };
  double depTot = 0;                // calibrage : retraits simulés
  double emptyAt1430 = 0, fullAt1430 = 0; int n1430 = 0;

  float F(int i, int64_t t) const {
    struct tm lt; plat::localTime(t, &lt);
    bool hol = isHoliday(lt);
    int dow = hol ? 0 : lt.tm_wday;
    bool we = dow == 0 || dow == 6;
    float h = lt.tm_hour + lt.tm_min / 60.f - st[i].phase;
    if (h < 0) h += 24;
    float base = shapeF(st[i].kind, we, dow, 3.f);
    float f = base + st[i].amp * (shapeF(st[i].kind, we, dow, h) - base);
    f = 0.05f + 0.85f * f;                       // parc moins rempli (instantané réel : ~1/3 à 14 h 30)
    return fminf(0.97f, fmaxf(0.03f, f));
  }
  const Hour & hourAt(int64_t t) const {
    int64_t k = (t - T0) / 3600;
    if (k < 0) k = 0;
    if (k >= (int64_t)hours.size()) k = hours.size() - 1;
    return hours[k];
  }
  float mult(int i, int64_t t) const {
    float m = 1.f - cfg.effect * st[i].sens * hourAt(t).bad;
    return fmaxf(0.05f, m);
  }
};

static void buildWeather(World & w) {
  Rng r(w.cfg.seed * 31 + 7);
  int nh = w.cfg.days * 24 + 48;
  w.hours.resize(nh);
  bool raining = false;
  float tAr = 0, wAr = 0, intens = 0;
  for (int k = 0; k < nh; k++) {
    int64_t t = w.T0 + (int64_t)k * 3600;
    struct tm lt; plat::localTime(t, &lt);
    float doy = lt.tm_yday + lt.tm_hour / 24.f;
    // Saison : printemps humide → été plus sec mais orageux ; ~10–13 % des heures pluvieuses
    float season = 0.5f + 0.5f * cosf(6.2832f * (doy - 20) / 365);     // 1 en hiver, 0 en été
    float pStart = 0.020f + 0.020f * season, pStop = 0.22f - 0.06f * season;
    if (!raining && r.u() < pStart) { raining = true; intens = (float)exp(-0.4 + 0.8 * r.n()); }
    else if (raining && r.u() < pStop) raining = false;
    Hour & H = w.hours[k];
    tAr = 0.85f * tAr + 1.2f * (float)r.n();
    wAr = 0.8f * wAr + 3.5f * (float)r.n();
    float tSeas = 10.5f - 7.5f * cosf(6.2832f * (doy - 15) / 365);
    float diur = 4.5f * sinf(6.2832f * (lt.tm_hour - 9) / 24);
    H.temp = tSeas + diur + tAr - (raining ? 2.f : 0.f);
    H.wind = fmaxf(0.f, 12 + wAr + (raining ? 7 : 0));
    H.rain = raining ? fmaxf(0.1f, intens * (float)exp(0.35 * r.n())) : 0.f;
    if (H.rain > 0) {
      if (H.temp < 1) H.code = 73;
      else if (H.rain > 4 && season < 0.4f && r.u() < 0.3) H.code = 95;
      else H.code = H.rain < 0.5f ? 61 : H.rain < 2.5f ? 63 : 65;
    } else {
      H.code = r.u() < 0.4 ? 3 : r.u() < 0.6 ? 2 : 0;
    }
    H.bad = trueBad(H.rain, H.temp, H.wind, H.code);
  }
}

// Ce que le serveur météo annonce au temps t : actuel exact, prévision horaire bruitée
// (calage temporel imparfait, intensité approximative, probabilité de pluie floue).
static void forecastAt(const World & w, int64_t t, Weather & out) {
  out = Weather();
  const Hour & now = w.hourAt(t);
  out.valid = true;
  out.fetchedAt = t;
  out.temp = now.temp; out.rain = now.rain; out.wind = now.wind; out.code = now.code;
  int64_t h0 = (t - w.T0) / 3600;
  out.nfc = 6;
  for (int i = 0; i < 6; i++) {
    int64_t H = h0 + i;
    // erreur de calage : jusqu'à ±1 h, plus probable loin dans le temps
    double e = hash01(H * 13 + 1, (uint64_t)(h0 / 3) + 99);
    int shift = i == 0 ? 0 : (e < 0.08 * i ? -1 : e > 1 - 0.08 * i ? 1 : 0);
    if (w.cfg.perfectFc) shift = 0;
    int64_t src = H + shift;
    if (src < 0) src = 0;
    if (src >= (int64_t)w.hours.size()) src = w.hours.size() - 1;
    const Hour & tr = w.hours[src];
    WxHour & f = out.fc[i];
    f.h = (int8_t)i;
    f.temp = tr.temp + 0.3f * i * (float)(hash01(H, 5) - 0.5);
    f.wind = fmaxf(0.f, tr.wind * (float)(0.85 + 0.3 * hash01(H, 6)));
    double ra = tr.rain * exp(0.5 * (hash01(H, 7) - 0.5) * 2);
    double fa = hash01(H * 3 + i, 8);                  // fausse alerte / pluie manquée
    if (tr.rain <= 0 && fa < 0.04 + 0.02 * i) ra = 0.2 + 0.6 * hash01(H, 9);
    if (tr.rain > 0 && fa < 0.03 * i) ra = 0;
    if (w.cfg.perfectFc) { ra = tr.rain; f.temp = tr.temp; f.wind = tr.wind; }
    f.rain = (float)ra;
    f.prob = (int16_t)(w.cfg.perfectFc ? (ra > 0 ? 100 : 0) : ra > 0 ? 55 + 40 * (1 - i / 6.0) * hash01(H, 10) : 20 * hash01(H, 11));
    f.code = (int16_t)(ra > 0 ? (ra < 0.5 ? 61 : ra < 2.5 ? 63 : 65) : tr.code > 3 ? 3 : tr.code);
    if (w.cfg.perfectFc) f.code = (int16_t)tr.code;
  }
  wxScore(out);
}

static void buildStations(World & w) {
  Rng r(w.cfg.seed * 97 + 3);
  // Répartition : 52 résidentiel, 34 bureaux, 2 gares, 8 écoles, 12 parcs, 33 mixtes (141) + 2 spéciales
  static const int NK[K_COUNT] = { 52, 34, 2, 8, 12, 33 };
  int i = 0;
  for (int k = 0; k < K_COUNT; k++)
    for (int c = 0; c < NK[k]; c++, i++) {
      SimSt & s = w.st[i];
      s.kind = k;
      s.cap = k == K_GARE ? 35 : 8 + (int)(r.u() * 21);
      s.amp = 0.8f + 0.4f * (float)r.u();
      s.phase = 0.5f * ((float)r.u() - 0.5f);
      s.sens = KSENS[k] * (0.7f + 0.6f * (float)r.u());
    }
  // Station agrandie 20 → 30 au jour CHANGE_DAY, et sa jumelle (mêmes paramètres, 30 bornes dès le début)
  for (int j : { CHANGED, TWIN }) {
    SimSt & s = w.st[j];
    s.kind = K_RES; s.cap = j == CHANGED ? 20 : 30; s.amp = 1; s.phase = 0; s.sens = KSENS[K_RES];
  }
  w.st[CHANGED].capFrom = 20; w.st[CHANGED].capChangeDay = CHANGE_DAY;
  for (int j = 0; j < NST; j++) {
    SimSt & s = w.st[j];
    snprintf(s.sid, sizeof(s.sid), "%d", 1000 + j);
    s.lat = 49.58f + 0.06f * (float)r.u(); s.lon = 6.09f + 0.09f * (float)r.u();
    s.bikes = (int)lroundf(w.F(j, w.T0) * s.cap);
    // demande moyenne en journée (pour l'oracle) : moyenne de m sur 7 h–22 h
    double sum = 0; int cnt = 0;
    for (size_t k = 0; k < w.hours.size(); k++) {
      int64_t t = w.T0 + (int64_t)k * 3600; struct tm lt; plat::localTime(t, &lt);
      if (lt.tm_hour < 7 || lt.tm_hour >= 22) continue;
      sum += w.mult(j, t); cnt++;
    }
    s.mAvg = cnt ? (float)(sum / cnt) : 1.f;
  }
}

// Un pas de 5 min de la vérité
static void stepWorld(World & w, int64_t t, int dayIdx, int64_t & lastTruck) {
  struct tm lt; plat::localTime(t, &lt);
  bool hol = isHoliday(lt);
  int dow = hol ? 0 : lt.tm_wday;
  bool we = dow == 0 || dow == 6;
  float h = lt.tm_hour + lt.tm_min / 60.f;
  for (int i = 0; i < NST; i++) {
    SimSt & s = w.st[i];
    if (i == CHANGED && dayIdx == s.capChangeDay && lt.tm_hour == 0 && lt.tm_min == 0) {
      s.cap = 30;                                     // agrandissement : l'exploitant remplit à proportion
      s.bikes = (int)lroundf(s.bikes * 1.5f);
    }
    s.closed = (i == CLOSED_ST && dayIdx >= 30 && dayIdx < 34);
    if (s.closed) continue;
    float f0 = w.F(i, t), f1 = w.F(i, t + 300);
    float m = w.mult(i, t);
    float act = activity(s.kind, we, h) * s.cap / 12.f;     // par pas de 5 min
    float net = (f1 - f0) * s.cap;
    float lamDep = m * (act / 2 + fmaxf(0.f, -net));
    float lamArr = m * (act / 2 + fmaxf(0.f, net));
    int dep = w.rFlow.poisson(lamDep), arr = w.rFlow.poisson(lamArr);
    if (dep > s.bikes) dep = s.bikes;
    s.bikes -= dep;
    w.depTot += dep;
    if (arr > s.cap - s.bikes) arr = s.cap - s.bikes;
    s.bikes += arr;
    // Régulation de nuit (03:00) : 70 % de l'écart à la cible
    if (lt.tm_hour == 3 && lt.tm_min == 0) s.bikes += (int)lroundf(0.7f * (w.F(i, t) * s.cap - s.bikes));
    bool bad = s.bikes == 0 || s.bikes == s.cap;
    if (!bad) s.badSince = 0; else if (!s.badSince) s.badSince = t;
  }
  if (lt.tm_hour == 14 && lt.tm_min == 30 && !we) {
    for (int i = 0; i < NST; i++) if (!w.st[i].closed) { w.emptyAt1430 += w.st[i].bikes == 0; w.fullAt1430 += w.st[i].bikes == w.st[i].cap; }
    w.n1430++;
  }
  // Camion de jour : au plus un passage toutes les 20 min, sur la station en rupture depuis le plus longtemps (≥ 30 min)
  if (lt.tm_hour >= 7 && lt.tm_hour < 21 && t - lastTruck >= 1200) {
    int best = -1; int64_t since = t - 1800;
    for (int i = 0; i < NST; i++) if (!w.st[i].closed && w.st[i].badSince && w.st[i].badSince <= since) { since = w.st[i].badSince; best = i; }
    if (best >= 0) { w.st[best].bikes = w.st[best].cap / 2; w.st[best].badSince = 0; lastTruck = t; }
  }
}

// ── Mesures ──────────────────────────────────────────────────────────
static const int NH = 5;
static const int HK[NH] = { 3, 6, 12, 24, 48 };      // pas de 5 min : +15, +30, +60, +120 (fiche), +240 (courbe 24 h)
static const char * HL[NH] = { "+15 min", "+30 min", "+60 min", "+120 min", "+240 min" };
static const int NP = 49;                             // pas projetés (Model::project, même règle que la fiche)

struct Acc {
  double s = 0, q = 0; long n = 0;
  void add(double e) { s += fabs(e); q += e * e; n++; }
  double mean() const { return n ? s / n : NAN; }
  double rmse() const { return n ? sqrt(q / n) : NAN; }
};
// Le chiffre affiché par la fiche station est la prévision arrondie : c'est lui qu'on note.
static inline float shown(float v) { return roundf(v); }

struct Pred { int16_t st; int32_t step; float cur, fc[NH], orc[NH], prof[NH]; };

int main(int argc, char ** argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  World w;
  Cfg & cfg = w.cfg;
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--jours") && a + 1 < argc) cfg.days = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--graine") && a + 1 < argc) cfg.seed = strtoull(argv[++a], nullptr, 10);
    else if (!strcmp(argv[a], "--effet") && a + 1 < argc) {
      cfg.effName = argv[++a];
      cfg.effect = !strcmp(cfg.effName, "nul") ? 0.f : !strcmp(cfg.effName, "fort") ? 1.6f : 1.f;
    } else if (!strcmp(argv[a], "--meteo") && a + 1 < argc) cfg.wxAvailable = strcmp(argv[++a], "absente") != 0;
    else if (!strcmp(argv[a], "--court")) cfg.brief = true;
    else if (!strcmp(argv[a], "--prevision-parfaite")) cfg.perfectFc = true;
    else if (!strcmp(argv[a], "--levier") && a + 1 < argc) {
      const char * kv = argv[++a];
      const char * eq = strchr(kv, '=');
      std::string k = eq ? std::string(kv, eq - kv) : std::string(kv);
      bool v = eq ? atoi(eq + 1) != 0 : true;
      if (k == "flux_meteo") g_pred.wxFlow = v;
      else if (k == "prevue_meteo") g_pred.wxForecast = v;
      else { fprintf(stderr, "levier inconnu : %s\n", k.c_str()); return 1; }
    }
    else { fprintf(stderr, "option inconnue : %s\n", argv[a]); return 1; }
  }
  sim::g_fsDir = "/tmp/vn_bench_fs";
  settingsDefaults(g_set);
  w.T0 = lux(2026, 3, 2);                              // lundi 2 mars 2026, 00:00 (Luxembourg)
  w.rFlow = Rng(cfg.seed * 1009 + 11);
  w.rInc = Rng(cfg.seed * 2003 + 13);
  buildWeather(w);
  buildStations(w);

  static Model m;
  if (!m.init()) { fprintf(stderr, "init modèle\n"); return 1; }
  static InfoRec ir[NST];
  static StatusRec sr[NST];
  auto info = [&]() {
    for (int i = 0; i < NST; i++) {
      InfoRec & r = ir[i]; memset(&r, 0, sizeof(r));
      strcpy(r.sid, w.st[i].sid);
      snprintf(r.name, sizeof(r.name), "%s %d", KNAME[w.st[i].kind], i);
      r.lat = w.st[i].lat; r.lon = w.st[i].lon; r.cap = (uint16_t)w.st[i].cap;
    }
  };
  info();
  m.applyInfo(ir, NST, w.T0);

  const int steps = cfg.days * 288;
  std::vector<int16_t> truth((size_t)(steps + 30) * NST, -1);   // vélos réels par pas (‑1 = fermée)
  std::vector<Pred> preds;
  preds.reserve((size_t)cfg.days * 64 * NST);
  // Incidents (vraies anomalies) : fenêtres par station
  struct Inc { int st; int64_t t0, t1; bool seen; };
  std::vector<Inc> incs;
  // Alertes : apparitions par jour
  std::vector<uint32_t> prevMask(NST, 0);
  std::vector<int64_t> lastUnus(NST, 0);              // dernière minute où « inhabituel » était actif
  std::vector<int64_t> lastSoon[2] = { std::vector<int64_t>(NST, 0), std::vector<int64_t>(NST, 0) };
  std::vector<int> prevTruth(NST, -1);
  long rupt[2] = {}, ruptAnt[2] = {};                 // ruptures réelles (7 h–22 h, j mûrs) / annoncées dans l'heure d'avant
  struct Day { int falseUnus = 0, trueUnus = 0, onset[AL_TYPE_COUNT] = {}, soonOk[2] = {}, soonN[2] = {}; int rainH = 0; bool hol = false;
               float learn = 0, conf = 0, anomConf = 0, evalFrac = 0, alpha = 0, alphaRaw = 0, alphaSE = 0, nRain = 0; int src = 0; float chBias = NAN; double chErr = 0; int chN = 0; double twErr = 0; int twN = 0; };
  std::vector<Day> days(cfg.days);
  struct Soon { int st, type; int64_t t; };
  std::vector<Soon> soons;
  int64_t lastTruck = 0;

  for (int k = 0; k < steps; k++) {
    int64_t t = w.T0 + (int64_t)k * VN_RING_STEP_S;
    int d = k / 288;
    struct tm lt; plat::localTime(t, &lt);
    if (k % 288 == 0) {
      days[d].hol = isHoliday(lt);
      for (int hh = 7; hh < 22; hh++) if (w.hourAt(t + hh * 3600).rain >= 0.2f) days[d].rainH++;
    }
    if (d == CHANGE_DAY && k % 288 == 0) {             // l'agrandissement apparaît aussi dans station_information
      w.st[CHANGED].cap = 30; info(); m.applyInfo(ir, NST, t);
    }
    stepWorld(w, t, d, lastTruck);
    // Incident : ~2 par jour entre 9 h et 19 h, décalage franc d'une station (évènement, dépôt groupé…)
    if (lt.tm_hour >= 9 && lt.tm_hour < 19 && w.rInc.u() < 2.0 / 120) {
      int i = (int)(w.rInc.u() * (NST - 2));
      SimSt & s = w.st[i];
      int mag = std::max(6, (int)lroundf(0.5f * s.cap));
      int sign = w.rInc.u() < 0.5 ? -1 : 1;
      int nb = std::min(s.cap, std::max(0, s.bikes + sign * mag));
      if (!s.closed && abs(nb - s.bikes) >= 5) { s.bikes = nb; incs.push_back({ i, t, t + 3 * 3600, false }); }
    }
    for (int i = 0; i < NST; i++) {
      const SimSt & s = w.st[i];
      truth[(size_t)k * NST + i] = s.closed ? -1 : (int16_t)s.bikes;
      StatusRec & r = sr[i]; memset(&r, 0, sizeof(r));
      strcpy(r.sid, s.sid);
      r.bikes = r.elec = (uint16_t)s.bikes; r.docks = (uint16_t)(s.cap - s.bikes);
      r.installed = 1; r.renting = r.returning = !s.closed;
      r.lastReported = t;
    }
    if (k % 3 == 0) {                                   // météo toutes les 15 min
      if (cfg.wxAvailable) forecastAt(w, t, m.wx);
      else { m.wx = Weather(); }
    }
    m.applyStatus(sr, NST, t);

    // ── Alertes : apparitions
    uint32_t mask[NST] = {};
    for (int a = 0; a < m.nAlerts; a++) mask[m.alerts[a].st] |= 1u << m.alerts[a].type;
    for (int i = 0; i < NST; i++) {
      uint32_t nw = mask[i] & ~prevMask[i];
      for (int ty = 0; ty < AL_TYPE_COUNT; ty++) if (nw & (1u << ty)) {
        days[d].onset[ty]++;
        if (ty == AL_UNUSUAL && t - lastUnus[i] > 3600) {   // un épisode : pas d'alerte dans l'heure précédente
          bool real = false;
          for (auto & in : incs) if (in.st == i && t >= in.t0 && t < in.t1) { real = true; }
          if (real) days[d].trueUnus++; else days[d].falseUnus++;
        }
        if (ty == AL_SOON_EMPTY || ty == AL_SOON_FULL) soons.push_back({ i, ty, t });
      }
      if (mask[i] & (1u << AL_UNUSUAL)) lastUnus[i] = t;
      if (mask[i] & (1u << AL_SOON_EMPTY)) lastSoon[0][i] = t;
      if (mask[i] & (1u << AL_SOON_FULL)) lastSoon[1][i] = t;
      {
        int v = truth[(size_t)k * NST + i], pv = prevTruth[i];
        bool mat = d >= (cfg.days >= 84 ? 56 : cfg.days / 2) && lt.tm_hour >= 7 && lt.tm_hour < 22;
        if (mat && pv > 0 && v == 0) { rupt[0]++; ruptAnt[0] += lastSoon[0][i] && t - lastSoon[0][i] <= 3600 + 300; }
        int cp = w.st[i].cap;
        if (mat && pv >= 0 && pv < cp && v == cp) { rupt[1]++; ruptAnt[1] += lastSoon[1][i] && t - lastSoon[1][i] <= 3600 + 300; }
        prevTruth[i] = v;
      }
      if (mask[i] & (1u << AL_UNUSUAL))
        for (auto & in : incs) if (in.st == i && t >= in.t0 && t < in.t1) in.seen = true;
      prevMask[i] = mask[i];
    }

    // ── Prévisions : toutes les 15 min, 6 h – 22 h
    if (k % 3 == 0 && lt.tm_hour >= 6 && lt.tm_hour < 22) {
      for (int i = 0; i < NST; i++) {
        const Station & s = m.st[i];
        if (!s.live || s.status == ST_CLOSED) continue;
        Pred p; p.st = (int16_t)i; p.step = k; p.cur = s.bikes;
        float cf[NP], tmp[NP];
        m.project(i, t, 5, NP, tmp, cf);
        float p0, n0;
        bool has0 = m.profileAt(i, t, p0, n0);
        for (int h = 0; h < NH; h++) {
          p.fc[h] = HK[h] < VN_FC_STEPS ? s.fc[HK[h]] : tmp[HK[h]];
          // Oracle : la même règle, avec l'écart au profil mis à l'échelle de la demande réelle
          // moyenne sur l'horizon (rapportée à la demande moyenne qui a façonné le profil).
          float ph, nh2;
          float o = p.fc[h];
          p.prof[h] = s.bikes;
          if (has0 && m.profileAt(i, t + HK[h] * 300, ph, nh2)) {
            p.prof[h] = fminf((float)s.capEff(), fmaxf(0.f, s.bikes + cf[HK[h]] * (ph - p0)));
            double mm = 0; for (int q = 0; q < HK[h]; q++) mm += w.mult(i, t + q * 300);
            mm /= HK[h];
            o += cf[HK[h]] * (float)(mm / w.st[i].mAvg - 1.0) * (ph - p0);
            o = fminf((float)s.capEff(), fmaxf(0.f, o));
          }
          p.orc[h] = o;
        }
        preds.push_back(p);
      }
    }

    // ── Maturation (chaque jour à midi) et station agrandie
    if (lt.tm_hour == 12 && lt.tm_min == 0) {
      Day & D = days[d];
      D.learn = m.learnPct(); D.conf = m.netConf;
      D.alpha = m.wxAlpha; D.alphaRaw = m.wxAlphaRaw; D.alphaSE = m.wxAlphaSE; D.src = m.wxSrc; D.nRain = m.wxG.nRain;
      int ev = 0, op = 0; double ac = 0;
      for (int i = 0; i < NST; i++) {
        const Station & s = m.st[i];
        if (!s.live || s.status == ST_CLOSED) continue;
        op++; ac += s.anomConf;
        if (s.anom == AN_NORMAL || s.anom == AN_HIGH || s.anom == AN_LOW) ev++;
      }
      D.anomConf = op ? (float)(ac / op) : 0; D.evalFrac = op ? 100.f * ev / op : 0;
      // Biais du profil de la station agrandie contre sa jumelle (7 h – 22 h, les 7 jours)
      double bias = 0; int nb = 0;
      for (int dd = 0; dd < 7; dd++) for (int q = 7 * 4; q < 22 * 4; q++) {
        int64_t tq = t + (int64_t)dd * 86400 + (int64_t)(q * 15 - 12 * 60) * 60;
        float a, na, b, nb2;
        if (m.profileAt(CHANGED, tq, a, na) && m.profileAt(TWIN, tq, b, nb2)) { bias += fabs(a - b); nb++; }
      }
      D.chBias = nb ? (float)(bias / nb) : NAN;
    }
  }

  // ── Évaluation a posteriori des prévisions
  struct Cell { Acc fc[NH], per[NH], orc[NH], prof[NH], raw[NH]; };
  Cell all, dry, wet, mature, matureWet, matureDry, hol, onset, ending;
  Cell byBlock[8];
  const int matureFrom = cfg.days >= 84 ? 56 : cfg.days / 2;
  for (const Pred & p : preds) {
    int d = p.step / 288;
    for (int h = 0; h < NH; h++) {
      int tk = p.step + HK[h];
      if (tk >= steps) continue;
      int tv = truth[(size_t)tk * NST + p.st];
      if (tv < 0) continue;
      int64_t t = w.T0 + (int64_t)p.step * VN_RING_STEP_S;
      float badMax = 0;
      for (int q = 0; q <= HK[h]; q++) badMax = fmaxf(badMax, w.hourAt(t + q * 300).bad);
      bool isWet = badMax > 0.1f;
      float badNow = w.hourAt(t).bad, badEnd = w.hourAt(t + HK[h] * 300).bad;
      bool isOnset = w.hourAt(t).rain <= 0 && w.hourAt(t + HK[h] * 300).rain >= 0.1f;   // sec maintenant, pluie à t+h
      bool isEnd = w.hourAt(t).rain >= 0.1f && w.hourAt(t + HK[h] * 300).rain <= 0;     // pluie maintenant, sec à t+h
      (void)badNow; (void)badEnd;
      double ef = shown(p.fc[h]) - tv, ep = p.cur - tv, eo = shown(p.orc[h]) - tv;
      auto add = [&](Cell & c) { c.fc[h].add(ef); c.per[h].add(ep); c.orc[h].add(eo); c.prof[h].add(shown(p.prof[h]) - tv); c.raw[h].add(p.fc[h] - tv); };
      add(all);
      add(isWet ? wet : dry);
      if (days[d].hol) add(hol);
      if (d >= matureFrom) { add(mature); add(isWet ? matureWet : matureDry); if (isOnset) add(onset); if (isEnd) add(ending); }
      int b = d / 21; if (b < 8) add(byBlock[b]);
      if (p.st == CHANGED && h == 2) { days[d].chErr += fabs(ef); days[d].chN++; }
      if (p.st == TWIN && h == 2) { days[d].twErr += fabs(ef); days[d].twN++; }
    }
  }
  // Rappel « inhabituel » sur les incidents, précision « rupture prévue »
  int incTot = 0, incSeen = 0, incR = 0, incRS = 0, incD = 0, incDS = 0;
  for (auto & in : incs) {
    incTot++; incSeen += in.seen;
    const Day & D = days[(in.t0 - w.T0) / 86400];
    if (D.rainH >= 3) { incR++; incRS += in.seen; }
    if (D.rainH == 0) { incD++; incDS += in.seen; }
  }
  for (auto & so : soons) {
    int k0 = (int)((so.t - w.T0) / VN_RING_STEP_S), d = k0 / 288;
    bool hit = false;
    for (int q = 0; q <= 15 && k0 + q < steps; q++) {          // dans les 75 min
      int v = truth[(size_t)(k0 + q) * NST + so.st];
      if (v < 0) break;
      if (so.type == AL_SOON_EMPTY ? v == 0 : v == w.st[so.st].cap) { hit = true; break; }
    }
    int j = so.type == AL_SOON_EMPTY ? 0 : 1;
    days[d].soonN[j]++; days[d].soonOk[j] += hit;
  }

  // ── Rapport
  printf("# Banc de prévision — %d jours, graine %llu, effet météo « %s » (×%.1f), météo %s\n\n",
         cfg.days, (unsigned long long)cfg.seed, cfg.effName, cfg.effect, cfg.wxAvailable ? "disponible" : "ABSENTE");
  int rainyH = 0, totH = 0;
  for (auto & hh : w.hours) { totH++; rainyH += hh.rain >= 0.1f; }
  int nRainDays = 0, nDryDays = 0, nHol = 0;
  for (auto & D : days) { nRainDays += D.rainH >= 3; nDryDays += D.rainH == 0; nHol += D.hol; }
  printf("Calibrage : %.0f retraits / jour (parc), à 14 h 30 en semaine : %.1f stations vides, %.1f pleines (instantané réel du 13/10 : 15 vides, 7 pleines sur 148)\n",
         w.depTot / cfg.days, w.n1430 ? w.emptyAt1430 / w.n1430 : 0, w.n1430 ? w.fullAt1430 / w.n1430 : 0);
  printf("Météo simulée : %.1f %% des heures pluvieuses ; %d jours de pluie (≥ 3 h entre 7 h et 22 h), %d jours secs, %d fériés\n\n",
         100.0 * rainyH / totH, nRainDays, nDryDays, nHol);

  auto mae = [&](const char * title, const Cell & c) {
    printf("| %-28s |", title);
    for (int h = 0; h < NH; h++) printf(" %5.3f / %5.3f / %5.3f |", c.fc[h].mean(), c.per[h].mean(), c.orc[h].mean());
    printf(" %ld |\n", c.fc[0].n);
  };
  printf("## 1. MAE (vélos) du chiffre affiché (prévision arrondie) — modèle / persistance / oracle météo\n\n");
  printf("| %-28s |", "échantillon");
  for (int h = 0; h < NH; h++) printf(" %-23s |", HL[h]);
  printf(" n (+15) |\n|---|---|---|---|---|---|---|\n");
  mae("tout", all);
  mae("tout · sec", dry);
  mae("tout · pluie", wet);
  char tt[64];
  snprintf(tt, sizeof(tt), "mûr (j ≥ %d)", matureFrom); mae(tt, mature);
  snprintf(tt, sizeof(tt), "mûr · sec"); mae(tt, matureDry);
  snprintf(tt, sizeof(tt), "mûr · pluie"); mae(tt, matureWet);
  mae("jours fériés", hol);
  printf("\nPériode mûre — profil seul, sans tendance (MAE affichée) : ");
  for (int h = 0; h < NH; h++) printf("%s %.3f  ", HL[h], mature.prof[h].mean());
  printf("\nPériode mûre — RMSE de la prévision brute (non arrondie) / persistance : ");
  for (int h = 0; h < NH; h++) printf("%s %.3f / %.3f  ", HL[h], mature.raw[h].rmse(), mature.per[h].rmse());
  printf("\n");
  if (!cfg.brief) {
    printf("\nPar tranche de 3 semaines (maturation de la prévision) :\n\n| semaines | +15 | +30 | +60 | +120 | persistance +60 |\n|---|---|---|---|---|---|\n");
    for (int b = 0; b < 8 && b * 21 < cfg.days; b++)
      printf("| %d–%d | %.3f | %.3f | %.3f | %.3f | %.3f |\n", b * 3 + 1, b * 3 + 3, byBlock[b].fc[0].mean(), byBlock[b].fc[1].mean(),
             byBlock[b].fc[2].mean(), byBlock[b].fc[3].mean(), byBlock[b].per[2].mean());
  }

  printf("\n## 2. Alertes « inhabituel » : épisodes par jour, parc entier (une alerte qui revient dans l'heure ne compte pas)\n\n");
  auto unus = [&](const char * name, auto pred) {
    double f = 0, tr = 0; int nd = 0;
    for (int d = 0; d < cfg.days; d++) if (pred(d)) { f += days[d].falseUnus; tr += days[d].trueUnus; nd++; }
    printf("| %-22s | %3d | %6.2f | %6.2f |\n", name, nd, nd ? f / nd : NAN, nd ? tr / nd : NAN);
  };
  printf("| jours (j ≥ %d) | n | fausses / jour | vraies / jour |\n|---|---|---|---|\n", matureFrom);
  unus("jour de pluie (≥ 3 h)", [&](int d) { return d >= matureFrom && days[d].rainH >= 3 && !days[d].hol; });
  unus("jour sec", [&](int d) { return d >= matureFrom && days[d].rainH == 0 && !days[d].hol; });
  unus("jour mixte (1–2 h)", [&](int d) { return d >= matureFrom && days[d].rainH > 0 && days[d].rainH < 3 && !days[d].hol; });
  unus("jour férié", [&](int d) { return d >= matureFrom && days[d].hol; });
  printf("\nIncidents injectés : %d, détectés (« inhabituel » pendant l'incident) : %d → rappel %.1f %% "
         "(jours de pluie %.1f %% sur %d, jours secs %.1f %% sur %d)\n",
         incTot, incSeen, incTot ? 100.0 * incSeen / incTot : 0, incR ? 100.0 * incRS / incR : NAN, incR,
         incD ? 100.0 * incDS / incD : NAN, incD);

  if (!cfg.brief) {
    printf("\n## 3. Maturation du profil\n\n| jour | couverture learnPct | confiance +1 h (parc) | confiance du profil (écart) | stations évaluées « inhabituel » |\n|---|---|---|---|---|\n");
    for (int d : { 1, 2, 3, 5, 7, 10, 14, 21, 28, 42, 56, 84, 112, 140, 167 })
      if (d < cfg.days) printf("| %d | %.0f %% | %.0f %% | %.0f %% | %.0f %% |\n", d + 1, days[d].learn, days[d].conf, days[d].anomConf, days[d].evalFrac);

    printf("\n## 3b. Coefficient météo appris (α global, à midi)\n\n| jour | fenêtres pluvieuses | α brut ± erreur-type | α appliqué | origine |\n|---|---|---|---|---|\n");
    static const char * SRC[3] = { "défaut (trop peu de pluie)", "sans effet mesurable", "appris" };
    for (int d : { 7, 14, 21, 28, 42, 56, 84, 112, 140, 167 })
      if (d < cfg.days) printf("| %d | %.0f | %+.3f ± %.3f | %+.3f | %s |\n", d + 1, days[d].nRain, days[d].alphaRaw, days[d].alphaSE, days[d].alpha, SRC[days[d].src]);

    printf("\n## 4. Station agrandie 20 → 30 bornes au jour %d (jumelle de 30 bornes en témoin)\n\n", CHANGE_DAY + 1);
    printf("| jours après | écart moyen du profil à la jumelle (vélos) | MAE +60 agrandie | MAE +60 jumelle |\n|---|---|---|---|\n");
    for (int a : { -1, 0, 1, 3, 7, 14, 21, 28, 42, 56, 70, 84, 98, 111 }) {
      int d = CHANGE_DAY + a;
      if (d < 0 || d >= cfg.days) continue;
      double ce = 0, te = 0; int cn = 0, tn = 0;
      for (int dd = d - 3; dd <= d + 3; dd++) if (dd >= 0 && dd < cfg.days) { ce += days[dd].chErr; cn += days[dd].chN; te += days[dd].twErr; tn += days[dd].twN; }
      printf("| %+d | %.2f | %.2f | %.2f |\n", a, days[d].chBias, cn ? ce / cn : NAN, tn ? te / tn : NAN);
    }
    float b0 = days[CHANGE_DAY].chBias, floorB = NAN;
    if (CHANGE_DAY - 1 >= 0) floorB = NAN;
    int dHalf = -1, d90 = -1;
    for (int d = CHANGE_DAY; d < cfg.days; d++) {
      if (dHalf < 0 && days[d].chBias <= b0 * 0.5f) dHalf = d - CHANGE_DAY;
      if (d90 < 0 && days[d].chBias <= b0 * 0.1f + 0.3f) d90 = d - CHANGE_DAY;
    }
    (void)floorB;
    printf("\nÉcart initial %.2f vélos ; moitié absorbée en %d jours ; 90 %% absorbés en %s\n", b0, dHalf,
           d90 >= 0 ? (std::to_string(d90) + " jours").c_str() : "plus que la durée simulée");
  }

  printf("\n## 5. Bruit : apparitions d'alertes par jour (j ≥ %d)\n\n| type | sec | pluie | tous |\n|---|---|---|---|\n", matureFrom);
  static const char * AN[AL_TYPE_COUNT] = { "vide", "pleine", "rupture prévue", "saturation prévue", "maint. vélos", "maint. bornes", "aucun mouvement", "fermée", "inhabituel" };
  for (int ty = 0; ty < AL_TYPE_COUNT; ty++) {
    double s0 = 0, s1 = 0, s2 = 0; int n0 = 0, n1 = 0, n2 = 0;
    for (int d = matureFrom; d < cfg.days; d++) {
      if (days[d].hol) continue;
      s2 += days[d].onset[ty]; n2++;
      if (days[d].rainH == 0) { s0 += days[d].onset[ty]; n0++; }
      if (days[d].rainH >= 3) { s1 += days[d].onset[ty]; n1++; }
    }
    printf("| %s | %.1f | %.1f | %.1f |\n", AN[ty], n0 ? s0 / n0 : NAN, n1 ? s1 / n1 : NAN, n2 ? s2 / n2 : NAN);
  }
  double sn[2] = {}, so[2] = {}, wn[2] = {}, wo[2] = {};
  for (int d = matureFrom; d < cfg.days; d++) for (int j = 0; j < 2; j++) {
    if (days[d].rainH == 0) { sn[j] += days[d].soonN[j]; so[j] += days[d].soonOk[j]; }
    if (days[d].rainH >= 3) { wn[j] += days[d].soonN[j]; wo[j] += days[d].soonOk[j]; }
  }
  printf("\nRuptures réelles annoncées dans l'heure précédente (rappel) : vide %.1f %% (%ld), pleine %.1f %% (%ld)",
         rupt[0] ? 100.0 * ruptAnt[0] / rupt[0] : NAN, rupt[0], rupt[1] ? 100.0 * ruptAnt[1] / rupt[1] : NAN, rupt[1]);
  printf("\nPrécision des alertes prévues (la rupture arrive dans les 75 min) : rupture %.0f %% sec / %.0f %% pluie · saturation %.0f %% sec / %.0f %% pluie\n",
         sn[0] ? 100 * so[0] / sn[0] : NAN, wn[0] ? 100 * wo[0] / wn[0] : NAN, sn[1] ? 100 * so[1] / sn[1] : NAN, wn[1] ? 100 * wo[1] / wn[1] : NAN);

  if (!cfg.brief || true) {
    // Dispersion des α par station (2 termes, comme le modèle) : vraie sensibilité contre estimation
    double sr = 0, sr2 = 0, sse = 0; int k = 0; double cov = 0, st2 = 0, smean = 0, nrain = 0;
    std::vector<double> rv, tv;
    for (int i = 0; i < NST; i++) {
      const WxStats & ws = m.wxS[i];
      double det = (double)ws.sdd * ws.szz - (double)ws.sdz * ws.sdz;
      if (ws.sn < 1 || det <= 0) continue;
      double raw = -((double)ws.sdd * ws.szr - (double)ws.sdz * ws.sdr) / det;
      double se = sqrt((double)ws.sgg) / (det / ws.sdd);
      sr += raw; sr2 += raw * raw; sse += se * se; k++; nrain += ws.nRain;
      rv.push_back(raw); tv.push_back(w.st[i].sens * cfg.effect);
    }
    if (k > 1) {
      for (size_t j = 0; j < rv.size(); j++) smean += tv[j];
      smean /= tv.size(); double rm = sr / k;
      for (size_t j = 0; j < rv.size(); j++) { cov += (rv[j] - rm) * (tv[j] - smean); st2 += (tv[j] - smean) * (tv[j] - smean); }
      printf("\nα par station : %d stations, fenêtres pluvieuses %.0f en moyenne ; α̂ moyen %.3f, écart-type %.3f ; erreur-type moyenne %.3f ; "
             "τ² = %.4f ; corrélation avec la vraie sensibilité %.2f\n",
             k, nrain / k, rm, sqrt(fmax(0, sr2 / k - rm * rm)), sqrt(sse / k), fmax(0, sr2 / k - rm * rm - sse / k),
             st2 > 0 ? cov / sqrt(st2 * fmax(1e-12, sr2 - k * rm * rm)) : NAN);
    }
  }
  // Ligne de synthèse (pour comparer deux réglages d'un coup d'œil)
  double fRain = 0, fDry = 0; int nR = 0, nD = 0;
  for (int d = matureFrom; d < cfg.days; d++) {
    if (days[d].hol) continue;
    if (days[d].rainH >= 3) { fRain += days[d].falseUnus; nR++; }
    if (days[d].rainH == 0) { fDry += days[d].falseUnus; nD++; }
  }
  printf("\nSYNTHESE graine=%llu effet=%s mae15=%.4f mae30=%.4f mae60=%.4f mae120=%.4f mae60_sec=%.4f mae60_pluie=%.4f mae120_pluie=%.4f "
         "orc60_pluie=%.4f orc60=%.4f per60=%.4f fausses_pluie=%.2f fausses_sec=%.2f rappel=%.1f rappel_pluie=%.1f rappel_sec=%.1f ferie60=%.4f annonce_vide=%.1f annonce_pleine=%.1f annonces_pluie_j=%.2f precision_pluie=%.1f precision_sec=%.1f alpha=%.3f alpha_brut=%.3f alpha_se=%.3f jours_alpha=%.1f mae60_debut=%.4f mae60_fin=%.4f n60_debut=%ld mae240=%.4f mae240_pluie=%.4f mae240_debut=%.4f\n",
         (unsigned long long)cfg.seed, cfg.effName, mature.fc[0].mean(), mature.fc[1].mean(), mature.fc[2].mean(), mature.fc[3].mean(),
         matureDry.fc[2].mean(), matureWet.fc[2].mean(), matureWet.fc[3].mean(), matureWet.orc[2].mean(), mature.orc[2].mean(), mature.per[2].mean(),
         nR ? fRain / nR : NAN, nD ? fDry / nD : NAN, incTot ? 100.0 * incSeen / incTot : 0,
         incR ? 100.0 * incRS / incR : NAN, incD ? 100.0 * incDS / incD : NAN, hol.fc[2].mean(),
         rupt[0] ? 100.0 * ruptAnt[0] / rupt[0] : NAN, rupt[1] ? 100.0 * ruptAnt[1] / rupt[1] : NAN,
         nR ? (wn[0] + wn[1]) / nR : NAN, (wn[0] + wn[1]) ? 100 * (wo[0] + wo[1]) / (wn[0] + wn[1]) : NAN,
         (sn[0] + sn[1]) ? 100 * (so[0] + so[1]) / (sn[0] + sn[1]) : NAN, m.wxAlpha, m.wxAlphaRaw, m.wxAlphaSE, [&] { int a = 0, t = 0; for (int d = 28; d < cfg.days; d++) { t++; a += days[d].alpha != 0; } return t ? 100.0 * a / t : 0.0; }(), onset.fc[2].mean(), ending.fc[2].mean(), onset.fc[2].n, mature.fc[4].mean(), matureWet.fc[4].mean(), onset.fc[4].mean());
  return 0;
}
