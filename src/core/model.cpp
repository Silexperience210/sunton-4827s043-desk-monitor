#include "model.h"
#include "settings.h"
#include "../platform/platform.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

namespace vn {

Model g_model;

static const int DAYQ = 96;                         // quarts d'heure par jour
static const int PROF_STRIDE = 7 * DAYQ;            // cellules par station
static const float PROF_FULL_N = 6.f;               // n à partir duquel la cellule « propre » suffit
static const float CONF_N = 8.f;                    // n effectif pour une confiance de 100 %

// ── Utilitaires ──────────────────────────────────────────────────────
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float haversineM(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.f, D = 0.01745329252f;
  float dla = (lat2 - lat1) * D, dlo = (lon2 - lon1) * D;
  float a = sinf(dla / 2) * sinf(dla / 2) + cosf(lat1 * D) * cosf(lat2 * D) * sinf(dlo / 2) * sinf(dlo / 2);
  return 2 * R * atan2f(sqrtf(a), sqrtf(1 - a));
}

const char * statusShort(uint8_t s) {
  static const char * L[] = { "VIDE", "FAIBLE", "\xC3\x89QUILIBR\xC3\x89" "E", "CHARG\xC3\x89" "E", "PLEINE", "FERM\xC3\x89" "E" };
  return s < ST_COUNT ? L[s] : "?";
}
// Heure locale → (jour de semaine, minute du jour)
static void localDM(int64_t t, int & dow, int & mod) {
  struct tm lt;
  plat::localTime(t, &lt);
  dow = lt.tm_wday;
  mod = lt.tm_hour * 60 + lt.tm_min;
}

// ── Initialisation ───────────────────────────────────────────────────
bool Model::init() {
  st     = (Station *)plat::psAlloc(sizeof(Station) * VN_MAX_ST);
  ringT  = (int64_t *)plat::psAlloc(sizeof(int64_t) * VN_RING_SLOTS);
  ring   = (uint8_t *)plat::psAlloc((size_t)VN_RING_SLOTS * VN_MAX_ST);
  net    = (NetSample *)plat::psAlloc(sizeof(NetSample) * VN_RING_SLOTS);
  recT   = (int64_t *)plat::psAlloc(sizeof(int64_t) * VN_RECENT_SLOTS);
  rec    = (uint8_t *)plat::psAlloc((size_t)VN_RECENT_SLOTS * VN_MAX_ST);
  prof   = (ProfCell *)plat::psAlloc(sizeof(ProfCell) * (size_t)VN_MAX_ST * PROF_STRIDE);
  cellN  = (uint8_t *)plat::psAlloc(PROF_STRIDE);
  alerts = (Alert *)plat::psAlloc(sizeof(Alert) * VN_MAX_ALERTS);
  if (!st || !ringT || !ring || !net || !recT || !rec || !prof || !cellN || !alerts) return false;
  memset(ring, 255, (size_t)VN_RING_SLOTS * VN_MAX_ST);
  memset(rec, 255, (size_t)VN_RECENT_SLOTS * VN_MAX_ST);
  return true;
}

int Model::find(const char * sid) const {
  for (int i = 0; i < n; i++) if (strcmp(st[i].sid, sid) == 0) return i;
  return -1;
}

static void initStation(Station & s, const char * sid) {
  memset(&s, 0, sizeof(s));
  strncpy(s.sid, sid, sizeof(s.sid) - 1);
  s.status = ST_CLOSED;
  s.etaEmpty = s.etaFull = -1;
}

int Model::ringIndex(int back) const {
  if (head < 0 || back < 0 || back >= count) return -1;
  return (head - back + VN_RING_SLOTS) % VN_RING_SLOTS;
}

// ── station_information ──────────────────────────────────────────────
void Model::applyInfo(const InfoRec * r, int cnt, int64_t now) {
  for (int i = 0; i < n; i++) st[i].active = false;
  for (int k = 0; k < cnt; k++) {
    int i = find(r[k].sid);
    if (i < 0) {
      if (n >= VN_MAX_ST) continue;
      i = n++;
      initStation(st[i], r[k].sid);
      st[i].lastChange = now;
    }
    Station & s = st[i];
    memcpy(s.name, r[k].name, sizeof(s.name));
    memcpy(s.addr, r[k].addr, sizeof(s.addr));
    s.lat = r[k].lat; s.lon = r[k].lon; s.cap = r[k].cap;
    s.active = true;
  }
  infoAt = now;
  version++;
}

// Classement sur l'axe de remplissage (valeurs « utilisables » comme dans l'app)
static uint8_t classify(const Station & s) {
  if (!s.live || !s.installed || (!s.renting && !s.returning)) return ST_CLOSED;
  int bikes = s.renting ? s.bikes : 0;
  int docks = s.returning ? s.docks : 0;
  if (bikes == 0) return ST_EMPTY;
  if (docks == 0) return ST_FULL;
  if (bikes <= g_set.lowBikes) return ST_LOW;
  if (docks <= g_set.lowDocks) return ST_HIGH;
  return ST_OK;
}

// Début d'une rupture en cours d'après l'historique 5 min (0 = inconnu : pas d'historique récent)
int64_t Model::ruptureStart(int i, bool empty, int64_t now) const {
  int64_t since = 0;
  const uint16_t cap = st[i].capEff();
  bool first = true;
  for (int back = 0; back < count; back++) {
    int idx = ringIndex(back);
    if (idx < 0) break;
    uint8_t v = ring[(size_t)idx * VN_MAX_ST + i];
    if (v == 255) continue;                                  // trou dans l'historique
    if (first && now - ringT[idx] > 1800) return 0;           // historique trop ancien
    first = false;
    bool same = empty ? v == 0 : (cap > 0 && v >= cap);
    if (!same) break;
    since = ringT[idx];
  }
  return since;
}

// ── station_status ───────────────────────────────────────────────────
void Model::applyStatus(const StatusRec * r, int cnt, int64_t now) {
  static bool seen[VN_MAX_ST];
  memset(seen, 0, sizeof(seen));
  int dep = 0, ret = 0;
  const bool firstFill = (lastUpdate == 0);
  for (int k = 0; k < cnt; k++) {
    int i = find(r[k].sid);
    if (i < 0 || !st[i].active) continue;
    Station & s = st[i];
    seen[i] = true;
    if (s.live) {
      int d = (int)r[k].bikes - (int)s.bikes;
      if (d > 0) ret += d; else dep -= d;
      if (r[k].bikes != s.bikes || r[k].docks != s.docks) s.lastChange = now;
    } else if (!s.lastChange) {
      s.lastChange = now;
    }
    s.bikes = r[k].bikes; s.elec = r[k].elec; s.docks = r[k].docks;
    s.bikesDis = r[k].bikesDis; s.docksDis = r[k].docksDis;
    s.installed = r[k].installed; s.renting = r[k].renting; s.returning = r[k].returning;
    s.lastReported = r[k].lastReported;
    s.live = true;
  }
  // Station publiée dans info mais absente du statut : non installée (cf. gbfs.js)
  for (int i = 0; i < n; i++)
    if (st[i].active && !seen[i]) { st[i].installed = st[i].renting = st[i].returning = false; }

  // Ruptures nouvelles → évènements UI
  for (int i = 0; i < n; i++) {
    Station & s = st[i];
    if (!s.active) continue;
    uint8_t prev = s.status, cur = classify(s);
    // Au premier relevé, une rupture déjà en cours n'a pas commencé « maintenant » :
    // sa durée est reconstituée depuis l'historique restauré, sinon elle reste inconnue (0).
    if (cur == ST_EMPTY) { if (prev != ST_EMPTY) { s.emptySince = firstFill ? ruptureStart(i, true, now) : now; if (!firstFill && nEv < 8) ev[nEv++] = { (int16_t)i, AL_EMPTY }; } }
    else s.emptySince = 0;
    if (cur == ST_FULL) { if (prev != ST_FULL) { s.fullSince = firstFill ? ruptureStart(i, false, now) : now; if (!firstFill && nEv < 8) ev[nEv++] = { (int16_t)i, AL_FULL }; } }
    else s.fullSince = 0;
    s.status = cur;
  }

  // Échantillon récent (~1 min) pour la tendance
  if (rHead < 0 || now - recT[rHead] >= 50) {
    rHead = (rHead + 1) % VN_RECENT_SLOTS;
    if (rCount < VN_RECENT_SLOTS) rCount++;
    recT[rHead] = now;
    uint8_t * row = rec + (size_t)rHead * VN_MAX_ST;
    for (int i = 0; i < VN_MAX_ST; i++)
      row[i] = (i < n && st[i].active && st[i].live) ? (uint8_t)(st[i].bikes > 254 ? 254 : st[i].bikes) : 255;
  }

  // Tranche de 5 min : historique 24 h + profils
  int64_t slotT = now - (now % VN_RING_STEP_S);
  if (head < 0 || net[head].t != slotT) {
    int gaps = 0;
    if (head >= 0 && slotT > net[head].t) gaps = (int)((slotT - net[head].t) / VN_RING_STEP_S) - 1;
    if (gaps > VN_RING_SLOTS) gaps = VN_RING_SLOTS;
    for (int g = 0; g <= gaps; g++) {
      head = (head + 1) % VN_RING_SLOTS;
      if (count < VN_RING_SLOTS) count++;
      bool real = (g == gaps);
      int64_t t = slotT - (int64_t)(gaps - g) * VN_RING_STEP_S;
      ringT[head] = t;
      uint8_t * row = ring + (size_t)head * VN_MAX_ST;
      NetSample & ns = net[head];
      memset(&ns, 0, sizeof(ns));
      ns.t = t;
      if (!real) {
        memset(row, 255, VN_MAX_ST);
        ns.avail = ns.docks = ns.dis = ns.bikesRaw = 0xFFFF;
        continue;
      }
      int av = 0, dk = 0, ds = 0, raw = 0, em = 0, fu = 0;
      for (int i = 0; i < VN_MAX_ST; i++) {
        if (i >= n || !st[i].active || !st[i].live) { row[i] = 255; continue; }
        const Station & s = st[i];
        row[i] = (uint8_t)(s.bikes > 254 ? 254 : s.bikes);
        raw += s.bikes; ds += s.bikesDis;
        if (s.status == ST_CLOSED) continue;
        if (s.renting) av += s.bikes;
        if (s.returning) dk += s.docks;
        em += s.status == ST_EMPTY; fu += s.status == ST_FULL;
      }
      ns.avail = (uint16_t)av; ns.docks = (uint16_t)dk; ns.dis = (uint16_t)ds; ns.bikesRaw = (uint16_t)raw;
      ns.empty = (uint8_t)(em > 255 ? 255 : em); ns.full = (uint8_t)(fu > 255 ? 255 : fu);

      // Profils : moyenne mobile plafonnée (≈ fenêtre exponentielle, cf. useAvailability.js)
      int dow, mod;
      localDM(t, dow, mod);
      int cell = dow * DAYQ + mod / 15;
      for (int i = 0; i < n; i++) {
        const Station & s = st[i];
        if (!s.active || !s.live || s.status == ST_CLOSED) continue;
        ProfCell & c = prof[(size_t)i * PROF_STRIDE + cell];
        float x = s.bikes * 256.f;
        if (c.n == 0) { c.a = (uint16_t)clampf(x, 0, 65535); c.n = 1; }
        else {
          int nn = c.n < VN_PROFILE_CAP_N ? c.n + 1 : VN_PROFILE_CAP_N;
          float a = c.a + (x - c.a) / nn;
          c.a = (uint16_t)clampf(a + 0.5f, 0, 65535);
          c.n = (uint8_t)nn;
        }
      }
      if (cellN[cell] < 255) cellN[cell]++;
      learnSamples++;
      if (!learnSince) learnSince = now;
    }
  }
  net[head].dep = (uint16_t)(net[head].dep + dep);
  net[head].ret = (uint16_t)(net[head].ret + ret);

  lastUpdate = now;
  dirty = true;
  recompute(now);
}

// ── Profil appris ────────────────────────────────────────────────────
// Valeur d'une cellule (jour, quart d'heure) : cellule propre complétée par
// le « pool » des jours de même type (lun–ven / sam–dim) tant qu'elle est jeune.
static bool cellValue(const ProfCell * p, int dow, int q, float & v, float & nEff) {
  const ProfCell & own = p[dow * DAYQ + q];
  if (own.n >= PROF_FULL_N) { v = own.a / 256.f; nEff = own.n; return true; }
  bool weekend = (dow == 0 || dow == 6);
  float sum = 0, pn = 0;
  for (int d = 0; d < 7; d++) {
    if (d == dow || ((d == 0 || d == 6) != weekend)) continue;
    const ProfCell & c = p[d * DAYQ + q];
    if (!c.n) continue;
    sum += (c.a / 256.f) * c.n;
    pn += c.n;
  }
  if (!own.n && pn <= 0) return false;
  if (pn <= 0) { v = own.a / 256.f; nEff = own.n; return true; }
  float w = own.n / PROF_FULL_N;
  v = w * (own.a / 256.f) + (1 - w) * (sum / pn);
  nEff = own.n + 0.5f * (pn > 12 ? 12 : pn);
  return true;
}

// Lissage ±15 min (0,25 / 0,5 / 0,25) + interpolation entre quarts d'heure
static bool smoothAt(const ProfCell * p, int dow, int q, float & v, float & nEff) {
  float sv = 0, sw = 0, sn = 1e9f;
  static const float W[3] = { 0.25f, 0.5f, 0.25f };
  for (int o = -1; o <= 1; o++) {
    int qq = q + o, d = dow;
    if (qq < 0) { qq += DAYQ; d = (d + 6) % 7; }
    if (qq >= DAYQ) { qq -= DAYQ; d = (d + 1) % 7; }
    float cv, cn;
    if (!cellValue(p, d, qq, cv, cn)) continue;
    sv += W[o + 1] * cv; sw += W[o + 1];
    if (cn < sn) sn = cn;
  }
  if (sw < 0.5f) return false;       // la cellule centrale au moins
  v = sv / sw;
  nEff = sn;
  return true;
}

static bool profileLocal(const ProfCell * p, int dow, int mod, float & avg, float & nEff) {
  while (mod < 0) { mod += 1440; dow = (dow + 6) % 7; }
  while (mod >= 1440) { mod -= 1440; dow = (dow + 1) % 7; }
  int q = mod / 15;
  float f = (mod % 15) / 15.f;
  float v0, n0, v1, n1;
  bool a = smoothAt(p, dow, q, v0, n0);
  int q1 = q + 1, d1 = dow;
  if (q1 >= DAYQ) { q1 = 0; d1 = (dow + 1) % 7; }
  bool b = smoothAt(p, d1, q1, v1, n1);
  if (a && b) { avg = v0 + (v1 - v0) * f; nEff = n0 < n1 ? n0 : n1; return true; }
  if (a) { avg = v0; nEff = n0; return true; }
  if (b) { avg = v1; nEff = n1; return true; }
  return false;
}

bool Model::profileAt(int s, int64_t t, float & avg, float & nEff) const {
  if (s < 0 || s >= n) return false;
  int dow, mod;
  localDM(t, dow, mod);
  return profileLocal(prof + (size_t)s * PROF_STRIDE, dow, mod, avg, nEff);
}

float Model::learnPct() const {
  int c = 0;
  for (int i = 0; i < PROF_STRIDE; i++) c += cellN[i] >= 3;
  return 100.f * c / PROF_STRIDE;
}

// ── Prévision d'une station ──────────────────────────────────────────
// fc(h) = actuel + c·[profil(t+h) − profil(t)] + (1 − 0,6c)·tendance amortie(h)
//  - profil : comportement appris à cette heure / ce jour (c = confiance 0…1)
//  - tendance : régression linéaire des 30 dernières minutes, amortie (τ = 25 min)
// Tendance court terme : pente (vélos/min) des 30 dernières minutes ; oldest = valeur la plus ancienne (‑1 si aucune)
static float trendSlope(const Model & m, int i, int64_t now, float & oldest) {
  float sx = 0, sy = 0, sxx = 0, sxy = 0; int cnt = 0;
  float tmin = 0;
  oldest = -1;
  for (int b = 0; b < m.rCount; b++) {
    int idx = (m.rHead - b + VN_RECENT_SLOTS) % VN_RECENT_SLOTS;
    float dtm = (m.recT[idx] - now) / 60.f;
    if (dtm < -31) break;
    uint8_t v = m.rec[(size_t)idx * VN_MAX_ST + i];
    if (v == 255) continue;
    sx += dtm; sy += v; sxx += dtm * dtm; sxy += dtm * v; cnt++;
    oldest = v; tmin = dtm;
  }
  float slope = 0;
  if (cnt >= 3 && tmin <= -8) {
    float den = cnt * sxx - sx * sx;
    if (fabsf(den) > 1e-3f) slope = clampf((cnt * sxy - sx * sy) / den, -0.6f, 0.6f);
  }
  return slope;
}

// La règle de projection elle-même, partagée par la prévision 2 h (pas 5 min)
// et la courbe 24 h (Model::project) : un seul endroit à lire et à tester.
static void projectSteps(const ProfCell * p, int dow0, int mod0, float cur, float capE, float slope,
                         int stepMin, int nSteps, float * out, float * conf) {
  float p0, n0;
  bool has0 = profileLocal(p, dow0, mod0, p0, n0);
  const float tau = 25.f;
  for (int k = 0; k < nSteps; k++) {
    int h = k * stepMin;
    float dS = 0, cS = 0, ph, nh;
    if (has0 && profileLocal(p, dow0, mod0 + h, ph, nh)) {
      dS = ph - p0;
      cS = clampf((n0 < nh ? n0 : nh) / CONF_N, 0, 1);
    }
    float dT = slope * tau * (1 - expf(-h / tau));
    float v = cur + cS * dS + (1 - 0.6f * cS) * dT;
    out[k] = clampf(v, 0, capE);
    if (conf) conf[k] = cS;
  }
}

static void forecastStation(Model & m, int i, int dow0, int mod0, int64_t now) {
  Station & s = m.st[i];
  float cur = s.bikes, capE = s.capEff();
  s.etaEmpty = s.etaFull = -1;
  s.conf = 0;
  s.delta30 = 0;
  if (s.status == ST_CLOSED || !s.live) {
    for (int k = 0; k < VN_FC_STEPS; k++) s.fc[k] = cur;
    return;
  }
  float oldest;
  float slope = trendSlope(m, i, now, oldest);
  if (oldest >= 0) s.delta30 = (int16_t)lroundf(cur - oldest);

  float conf[VN_FC_STEPS];
  projectSteps(m.prof + (size_t)i * PROF_STRIDE, dow0, mod0, cur, capE, slope, 5, VN_FC_STEPS, s.fc, conf);
  float conf60 = conf[12];
  s.conf = (uint8_t)lroundf(conf60 * 100);

  // Échéances (interpolées entre deux pas)
  if (s.renting && s.bikes == 0) s.etaEmpty = 0;
  else for (int k = 1; k < VN_FC_STEPS; k++) {
    if (s.fc[k] <= 0.5f) {
      float a = s.fc[k - 1], b = s.fc[k];
      float f = a - b > 1e-3f ? (a - 0.5f) / (a - b) : 1;
      s.etaEmpty = (int16_t)lroundf(5 * (k - 1) + 5 * f);
      break;
    }
  }
  if (s.returning && s.docks == 0) s.etaFull = 0;
  else if (capE > 0) for (int k = 1; k < VN_FC_STEPS; k++) {
    if (s.fc[k] >= capE - 0.5f) {
      float a = s.fc[k - 1], b = s.fc[k];
      float f = b - a > 1e-3f ? (capE - 0.5f - a) / (b - a) : 1;
      s.etaFull = (int16_t)lroundf(5 * (k - 1) + 5 * f);
      break;
    }
  }
}

bool Model::project(int i, int64_t now, int stepMin, int nSteps, float * out, float * conf) const {
  if (i < 0 || i >= n || nSteps <= 0 || stepMin <= 0) return false;
  const Station & s = st[i];
  if (s.status == ST_CLOSED || !s.live) {             // même convention que forecastStation
    for (int k = 0; k < nSteps; k++) { out[k] = s.bikes; if (conf) conf[k] = 0; }
    return true;
  }
  int dow0, mod0;
  localDM(now, dow0, mod0);
  float oldest;
  float slope = trendSlope(*this, i, now, oldest);
  projectSteps(prof + (size_t)i * PROF_STRIDE, dow0, mod0, s.bikes, s.capEff(), slope, stepMin, nSteps, out, conf);
  return true;
}

// ── Écart au profil (« comportement inhabituel ») ────────────────────
// On compare N (vélos en station maintenant) à P, le profil appris lissé pour ce
// (jour de semaine, quart d'heure) — la même valeur que la courbe « habituel » de
// la fiche — et à la confiance c de ce profil (n effectif ÷ CONF_N, plafonnée à 1).
//
//  • Exclusions : aucune alerte, le motif est gardé pour la fiche station.
//    – station fermée ou sans relevé ;
//    – hors heures d'activité (avant 7 h, à partir de 22 h, comme « aucun mouvement ») :
//      la nuit, les flux sont faibles et les régulations planifiées font des écarts sans
//      signification ;
//    – profil absent, ou trop jeune : c < ANOM_MIN_CONF = 0,5 (moins de 4 relevés
//      effectifs pour ce créneau) — sans recul, « inhabituel » ne veut rien dire.
//  • Tolérance T (vélos) = max(ANOM_MIN_BIKES, ANOM_REL_CAP × capacité utile) × (2 − c) :
//      à confiance pleine, T = max(4 vélos, 20 % de la capacité) ; à c = 0,5, T est 1,5 ×
//      plus large (moins le profil est nourri, plus il faut d'écart pour conclure).
//  • Écart minimal : ANOM_MIN_BIKES = 4 vélos, plancher de T — un retrait groupé de
//      2–3 vélos est du bruit ordinaire, même sur une petite station.
//  • Alerte si |N − P| > T (strictement).
// Limite connue : le relevé courant vient d'alimenter la cellule du profil (moyenne
// plafonnée à VN_PROFILE_CAP_N), ce qui rapproche P de N d'au plus 1/n : l'écart est
// légèrement sous-estimé, jamais gonflé.
static const float ANOM_MIN_CONF = 0.5f;
static const float ANOM_MIN_BIKES = 4.f;
static const float ANOM_REL_CAP = 0.2f;

static void anomalyStation(const Model & m, Station & s, int i, int64_t now, bool activeHours) {
  s.anomExp = -1; s.anomTol = 0; s.anomConf = 0;
  if (!s.live || s.status == ST_CLOSED) { s.anom = AN_CLOSED; return; }
  float p, nEff;
  bool has = m.profileAt(i, now, p, nEff);
  float c = has ? clampf(nEff / CONF_N, 0, 1) : 0;
  if (has) { s.anomExp = (int16_t)lroundf(p); s.anomConf = (uint8_t)lroundf(c * 100); }
  if (!activeHours) { s.anom = AN_OFF_HOURS; return; }
  if (!has) { s.anom = AN_NO_PROFILE; return; }
  if (c < ANOM_MIN_CONF) { s.anom = AN_YOUNG; return; }
  float tol = fmaxf(ANOM_MIN_BIKES, ANOM_REL_CAP * s.capEff()) * (2.f - c);
  s.anomTol = (int16_t)lroundf(tol);
  float dev = s.bikes - p;
  s.anom = fabsf(dev) > tol ? (dev > 0 ? AN_HIGH : AN_LOW) : AN_NORMAL;
}

// ── Alertes ──────────────────────────────────────────────────────────
static const Model * s_sortModel = nullptr;
static int alertCmp(const void * a, const void * b) {
  const Alert * x = (const Alert *)a; const Alert * y = (const Alert *)b;
  if (x->sev != y->sev) return y->sev - x->sev;
  int rx = s_sortModel->st[x->st].risk, ry = s_sortModel->st[y->st].risk;
  if (rx != ry) return ry - rx;
  return x->type - y->type;
}

void Model::recompute(int64_t now) {
  int dow0, mod0;
  localDM(now, dow0, mod0);
  int hour = mod0 / 60;
  bool activeHours = hour >= 7 && hour < 22;

  Totals t;
  memset(&t, 0, sizeof(t));
  for (int k = 0; k < VN_FC_STEPS; k++) netFc[k] = 0;
  float confSum = 0; int confN = 0;

  for (int i = 0; i < n; i++) {
    Station & s = st[i];
    if (!s.active) continue;
    s.status = classify(s);
    s.frozen = activeHours && s.live && s.status != ST_CLOSED && s.lastChange
               && (now - s.lastChange) >= (int64_t)g_set.frozenHours * 3600;
    forecastStation(*this, i, dow0, mod0, now);
    anomalyStation(*this, s, i, now, activeHours);

    // Besoin de régulation (viser 50 % de la capacité utile à +1 h)
    float capE = s.capEff(), fc60 = s.fc[12];
    bool tension = s.status == ST_EMPTY || s.status == ST_LOW || s.status == ST_FULL || s.status == ST_HIGH
                   || (s.etaEmpty >= 0 && s.etaEmpty <= 60) || (s.etaFull >= 0 && s.etaFull <= 60);
    s.need = 0;
    if (tension && capE >= 4 && s.status != ST_CLOSED) {
      int nd = (int)lroundf(capE * 0.5f - fc60);
      if (nd > 60) nd = 60;
      if (nd < -60) nd = -60;
      if (abs(nd) >= 2) s.need = (int8_t)nd;
    }
    // Priorité d'intervention
    float r = 0;
    if (s.status == ST_CLOSED) r = 5;
    else if (s.status == ST_EMPTY) r = 90 + fminf(10, s.emptySince ? (now - s.emptySince) / 600.f : 0);
    else if (s.status == ST_FULL) r = 85 + fminf(10, s.fullSince ? (now - s.fullSince) / 600.f : 0);
    else {
      int eta = -1;
      if (s.etaEmpty >= 0) eta = s.etaEmpty;
      if (s.etaFull >= 0 && (eta < 0 || s.etaFull < eta)) eta = s.etaFull;
      if (eta >= 0) r = 10 + 75 * (1 - eta / 120.f);
      if (s.status == ST_LOW || s.status == ST_HIGH) r = fmaxf(r, 40);
      r = fmaxf(r, fabsf(s.fill() - 0.5f) * 30);
      if (s.frozen) r = fmaxf(r, 30);
      if (s.bikesDis >= g_set.maintBikes || s.docksDis >= g_set.maintDocks) r = fmaxf(r, 25);
    }
    s.risk = (uint8_t)clampf(r, 0, 100);

    // Totaux
    t.stations++;
    t.cap += s.cap;
    t.bikesRaw += s.bikes;
    t.bikesDis += s.bikesDis; t.docksDis += s.docksDis;
    t.byStatus[s.status]++;
    if (s.status == ST_CLOSED) { t.closed++; continue; }
    t.open++;
    if (s.renting) { t.bikes += s.bikes; t.elec += s.elec; t.nRenting++; }
    if (s.returning) { t.docks += s.docks; t.nReturning++; }
    if (s.frozen) t.frozen++;
    if (s.bikesDis >= g_set.maintBikes || s.docksDis >= g_set.maintDocks) t.maint++;
    if (s.need > 0) t.need += s.need; else t.surplus -= s.need;
    if (s.renting) for (int k = 0; k < VN_FC_STEPS; k++) netFc[k] += s.fc[k];
    confSum += s.conf; confN++;
  }
  netConf = confN ? (int)lroundf(confSum / confN) : 0;
  t.fillPct = (t.bikes + t.docks) ? 100.f * t.bikes / (t.bikes + t.docks) : 0;
  t.balancePct = t.open ? 100.f * t.byStatus[ST_OK] / t.open : 0;

  // Parc estimé + vélos en circulation.
  // Le parc ne peut se lire que sur le creux de la nuit : c'est le seul moment
  // où la quasi-totalité des vélos est en station. Un maximum pris en pleine
  // journée ne mesure que le trafic du moment, d'où une validité qui dépend de
  // l'HEURE du pic retenu, pas seulement de la profondeur d'historique.
  int fleet = t.bikesRaw + t.bikesDis, span = 0;
  int64_t fleetAt = now;                         // le relevé courant peut être lui-même le maximum (en pleine nuit)
  for (int b = 0; b < count; b++) {
    const NetSample & ns = net[ringIndex(b)];
    if (ns.avail == 0xFFFF) continue;
    int v = ns.bikesRaw + ns.dis;
    if (v > fleet) { fleet = v; fleetAt = ns.t; }
    span = b;
  }
  t.fleet = fleet;
  t.histSlots = count;
  t.inUse = fleet - (t.bikesRaw + t.bikesDis);
  if (t.inUse < 0) t.inUse = 0;
  struct tm ltf;
  plat::localTime(fleetAt, &ltf);
  bool nightPeak = fleetAt > 0 && ltf.tm_hour >= 1 && ltf.tm_hour < 5;
  t.fleetAt = fleetAt;
  t.inUseValid = (span >= 72) && nightPeak;      // ≥ 6 h d'historique ET un creux de nuit observé

  // Activité sur 1 h glissante + variation
  for (int b = 0; b < 12 && b < count; b++) {
    const NetSample & ns = net[ringIndex(b)];
    t.dep1h += ns.dep; t.ret1h += ns.ret;
  }
  int i12 = ringIndex(12);
  bool h1 = i12 >= 0 && count >= 13 && net[i12].avail != 0xFFFF;
  t.actValid = count >= 13;                      // une heure pleine de relevés
  t.deltaValid = h1;
  t.delta1h = h1 ? t.bikes - net[i12].avail : 0;
  tot = t;

  // Liste d'alertes
  nAlerts = 0;
  memset(alertCount, 0, sizeof(alertCount));
  auto add = [&](uint8_t type, uint8_t sev, int i, int minutes, int value) {
    if (nAlerts >= VN_MAX_ALERTS) return;
    alerts[nAlerts++] = { type, sev, (int16_t)i, (int16_t)minutes, (int16_t)value };
    alertCount[type]++;
  };
  for (int i = 0; i < n; i++) {
    const Station & s = st[i];
    if (!s.active || !s.live) continue;
    if (s.status == ST_CLOSED) { add(AL_CLOSED, 0, i, -1, 0); continue; }
    if (s.status == ST_EMPTY) add(AL_EMPTY, 3, i, s.emptySince ? (int)((now - s.emptySince) / 60) : -1, 0);
    else if (s.status == ST_FULL) add(AL_FULL, 3, i, s.fullSince ? (int)((now - s.fullSince) / 60) : -1, 0);
    else if (s.etaEmpty >= 0 && s.etaEmpty <= 60) add(AL_SOON_EMPTY, s.etaEmpty <= 30 ? 2 : 1, i, s.etaEmpty, s.bikes);
    else if (s.etaFull >= 0 && s.etaFull <= 60) add(AL_SOON_FULL, s.etaFull <= 30 ? 2 : 1, i, s.etaFull, s.docks);
    if (!s.renting) add(AL_CLOSED, 1, i, -1, 1);
    else if (!s.returning) add(AL_CLOSED, 1, i, -1, 2);
    if (s.bikesDis >= g_set.maintBikes) add(AL_MAINT_BIKES, 1, i, -1, s.bikesDis);
    if (s.docksDis >= g_set.maintDocks) add(AL_MAINT_DOCKS, 1, i, -1, s.docksDis);
    if (s.frozen) add(AL_FROZEN, 1, i, (int)((now - s.lastChange) / 60), 0);
    // Sévérité « info » : un écart au profil est à examiner, pas une intervention — il ne
    // doit pas évincer les ruptures et la maintenance des priorités du tableau de bord.
    if (s.anom == AN_HIGH || s.anom == AN_LOW) add(AL_UNUSUAL, 0, i, -1, s.bikes - s.anomExp);
  }
  s_sortModel = this;
  qsort(alerts, nAlerts, sizeof(Alert), alertCmp);

  // Suggestions de transferts (glouton : besoin le plus urgent ← donneur le plus proche)
  nTr = 0;
  static int16_t donorLeft[VN_MAX_ST];
  for (int i = 0; i < n; i++) {
    const Station & s = st[i];
    donorLeft[i] = 0;
    if (!s.active || s.status == ST_CLOSED || !s.renting) continue;
    float capE = s.capEff();
    int give = s.need < 0 ? -s.need : (int)lroundf(fminf(s.fc[12], (float)s.bikes) - capE * 0.6f);
    if (give > s.bikes) give = s.bikes;
    if (give >= 2) donorLeft[i] = (int16_t)give;
  }
  static int16_t order[VN_MAX_ST];
  int no = 0;
  for (int i = 0; i < n; i++) if (st[i].active && st[i].need > 0 && st[i].risk >= 40) order[no++] = (int16_t)i;
  for (int a = 1; a < no; a++) {                     // tri par risque décroissant
    int16_t v = order[a]; int b = a - 1;
    while (b >= 0 && st[order[b]].risk < st[v].risk) { order[b + 1] = order[b]; b--; }
    order[b + 1] = v;
  }
  for (int k = 0; k < no && nTr < VN_MAX_TRANSFERS; k++) {
    const Station & d = st[order[k]];
    int best = -1; float bestD = 1e12f;
    for (int i = 0; i < n; i++) {
      if (donorLeft[i] < 2 || i == order[k]) continue;
      float dm = haversineM(d.lat, d.lon, st[i].lat, st[i].lon);
      if (dm < bestD) { bestD = dm; best = i; }
    }
    if (best < 0) break;
    int q = d.need;
    if (q > donorLeft[best]) q = donorLeft[best];
    if (q > 15) q = 15;
    if (q < 2) continue;
    donorLeft[best] -= q;
    tr[nTr++] = { (int16_t)best, order[k], (uint8_t)q, (uint16_t)fminf(bestD, 65000) };
  }
  version++;
}

// ── Persistance ──────────────────────────────────────────────────────
struct FileHdr {
  uint32_t magic;
  uint16_t ver, nSt;
  int32_t  head, count;
  int64_t  learnSince;
  uint32_t learnSamples;
  int64_t  savedAt;
};
static const uint32_t HIST_MAGIC = 0x31484E56;   // "VNH1"
static const uint32_t PROF_MAGIC = 0x31504E56;   // "VNP1"
static const char * HIST_PATH = "/vn_hist.bin";
static const char * PROF_PATH = "/vn_prof.bin";

bool Model::serialize(Blob & out) const {
  out = Blob();
  if (n <= 0) return false;
  FileHdr h{};
  h.nSt = (uint16_t)n; h.head = head; h.count = count;
  h.learnSince = learnSince; h.learnSamples = learnSamples; h.savedAt = lastUpdate;
  // Profils
  size_t sz = sizeof(h) + (size_t)n * 8 + (size_t)n * PROF_STRIDE * sizeof(ProfCell) + PROF_STRIDE;
  uint8_t * b = (uint8_t *)plat::psAlloc(sz);
  if (!b) return false;
  h.magic = PROF_MAGIC; h.ver = 1;
  size_t o = 0;
  memcpy(b + o, &h, sizeof(h)); o += sizeof(h);
  for (int i = 0; i < n; i++) { memcpy(b + o, st[i].sid, 8); o += 8; }
  memcpy(b + o, prof, (size_t)n * PROF_STRIDE * sizeof(ProfCell)); o += (size_t)n * PROF_STRIDE * sizeof(ProfCell);
  memcpy(b + o, cellN, PROF_STRIDE); o += PROF_STRIDE;
  out.prof = b; out.profLen = o;
  // Historique 24 h
  sz = sizeof(h) + (size_t)n * 8 + VN_RING_SLOTS * (8 + sizeof(NetSample) + (size_t)n);
  b = (uint8_t *)plat::psAlloc(sz);
  if (!b) { plat::psFree(out.prof); out = Blob(); return false; }
  h.magic = HIST_MAGIC; h.ver = 1;
  o = 0;
  memcpy(b + o, &h, sizeof(h)); o += sizeof(h);
  for (int i = 0; i < n; i++) { memcpy(b + o, st[i].sid, 8); o += 8; }
  memcpy(b + o, ringT, VN_RING_SLOTS * 8); o += VN_RING_SLOTS * 8;
  memcpy(b + o, net, VN_RING_SLOTS * sizeof(NetSample)); o += VN_RING_SLOTS * sizeof(NetSample);
  for (int r = 0; r < VN_RING_SLOTS; r++) { memcpy(b + o, ring + (size_t)r * VN_MAX_ST, n); o += n; }
  out.hist = b; out.histLen = o;
  return true;
}

bool Model::writeSaved(Blob & b) {
  bool ok = true;
  if (b.prof) { ok &= plat::fsWrite(PROF_PATH, b.prof, b.profLen); plat::psFree(b.prof); }
  if (b.hist) { ok &= plat::fsWrite(HIST_PATH, b.hist, b.histLen); plat::psFree(b.hist); }
  b = Blob();
  return ok;
}

// Associe les sid d'un fichier aux emplacements courants (crée les manquants)
static void mapSids(Model & m, const uint8_t * sids, int nSt, int16_t * map) {
  for (int j = 0; j < nSt; j++) {
    char sid[9];
    memcpy(sid, sids + j * 8, 8); sid[8] = 0;
    int i = m.find(sid);
    if (i < 0 && m.n < VN_MAX_ST && sid[0]) { i = m.n++; initStation(m.st[i], sid); }
    map[j] = (int16_t)i;
  }
}

bool Model::load(int64_t now) {
  bool any = false;
  static int16_t map[VN_MAX_ST];
  long psz = plat::fsSize(PROF_PATH);
  if (psz > (long)sizeof(FileHdr)) {
    uint8_t * b = (uint8_t *)plat::psAlloc(psz);
    if (b && plat::fsRead(PROF_PATH, b, psz)) {
      FileHdr h; memcpy(&h, b, sizeof(h));
      size_t need = sizeof(h) + (size_t)h.nSt * 8 + (size_t)h.nSt * PROF_STRIDE * sizeof(ProfCell) + PROF_STRIDE;
      if (h.magic == PROF_MAGIC && h.ver == 1 && h.nSt <= VN_MAX_ST && (size_t)psz == need) {
        mapSids(*this, b + sizeof(h), h.nSt, map);
        const uint8_t * pp = b + sizeof(h) + (size_t)h.nSt * 8;
        for (int j = 0; j < h.nSt; j++)
          if (map[j] >= 0) memcpy(prof + (size_t)map[j] * PROF_STRIDE, pp + (size_t)j * PROF_STRIDE * sizeof(ProfCell), PROF_STRIDE * sizeof(ProfCell));
        memcpy(cellN, pp + (size_t)h.nSt * PROF_STRIDE * sizeof(ProfCell), PROF_STRIDE);
        learnSince = h.learnSince; learnSamples = h.learnSamples;
        any = true;
      }
    }
    if (b) plat::psFree(b);
  }
  long hsz = plat::fsSize(HIST_PATH);
  if (hsz > (long)sizeof(FileHdr)) {
    uint8_t * b = (uint8_t *)plat::psAlloc(hsz);
    if (b && plat::fsRead(HIST_PATH, b, hsz)) {
      FileHdr h; memcpy(&h, b, sizeof(h));
      size_t need = sizeof(h) + (size_t)h.nSt * 8 + VN_RING_SLOTS * (8 + sizeof(NetSample) + (size_t)h.nSt);
      bool fresh = now <= 0 || (h.savedAt > 0 && now - h.savedAt < 20 * 3600);
      if (h.magic == HIST_MAGIC && h.ver == 1 && h.nSt <= VN_MAX_ST && (size_t)hsz == need && fresh
          && h.head >= 0 && h.head < VN_RING_SLOTS && h.count >= 0 && h.count <= VN_RING_SLOTS) {
        mapSids(*this, b + sizeof(h), h.nSt, map);
        size_t o = sizeof(h) + (size_t)h.nSt * 8;
        memcpy(ringT, b + o, VN_RING_SLOTS * 8); o += VN_RING_SLOTS * 8;
        memcpy(net, b + o, VN_RING_SLOTS * sizeof(NetSample)); o += VN_RING_SLOTS * sizeof(NetSample);
        for (int r = 0; r < VN_RING_SLOTS; r++) {
          uint8_t * row = ring + (size_t)r * VN_MAX_ST;
          for (int j = 0; j < h.nSt; j++) if (map[j] >= 0) row[map[j]] = b[o + j];
          o += h.nSt;
        }
        head = h.head; count = h.count;
        any = true;
      }
    }
    if (b) plat::psFree(b);
  }
  return any;
}

void Model::resetHistory() {
  memset(ring, 255, (size_t)VN_RING_SLOTS * VN_MAX_ST);
  memset(rec, 255, (size_t)VN_RECENT_SLOTS * VN_MAX_ST);
  memset(prof, 0, sizeof(ProfCell) * (size_t)VN_MAX_ST * PROF_STRIDE);
  memset(cellN, 0, PROF_STRIDE);
  memset(net, 0, sizeof(NetSample) * VN_RING_SLOTS);
  head = -1; count = 0; rHead = -1; rCount = 0;
  learnSince = 0; learnSamples = 0;
  plat::fsRemove(HIST_PATH);
  plat::fsRemove(PROF_PATH);
  version++;
}

} // namespace vn
