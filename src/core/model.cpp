#include "model.h"
#include "settings.h"
#include "calendar.h"
#include "../platform/platform.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

namespace vn {

Model g_model;
PredictOpts g_pred;
AlertOpts g_alertOpt;

static const int DAYQ = 96;                         // quarts d'heure par jour
static const int PROF_STRIDE = 7 * DAYQ;            // cellules par station
static const float PROF_FULL_N = 6.f;               // n à partir duquel la cellule « propre » suffit
static const float CONF_N = 8.f;                    // n effectif pour une confiance de 100 %
// Écart au profil (voir anomalyStation) — ici parce que l'apprentissage de la dispersion s'en sert
static const float ANOM_MIN_CONF = 0.5f;
static const float ANOM_MIN_BIKES = 4.f;
static const float ANOM_REL_CAP = 0.2f;
// Dispersion apprise (levier 1a) : d = écart absolu moyen du relevé au profil, ×16 dans la cellule
static const float DISP_SCALE = 16.f;
static const int   DISP_DN_MAX = 7;                 // compte dn sur 3 bits
static const float DISP_FULL_N = 6.f;               // dn à partir duquel la cellule « propre » suffit
static const float DISP_MIN_N = 4.f;                // relevés effectifs pour juger avec la dispersion
static const float DISP_CLIP_K = 1.5f, DISP_CLIP_B = 0.5f;   // écart versé écrêté à 1,5 D + ½ vélo

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
static bool holidayAt(int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  return isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}
const char * holidayNameAt(int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  return publicHolidayName(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}
// Jour « de profil » : le jour de semaine, ou dimanche pour un jour férié (levier 5a-ii)
static void profDM(int64_t t, int & dow, int & mod) {
  struct tm lt;
  plat::localTime(t, &lt);
  dow = lt.tm_wday;
  mod = lt.tm_hour * 60 + lt.tm_min;
  if (g_pred.holidaySunday && isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)) dow = 0;
}

// Case de profil qu'un relevé à l'instant t nourrit (levier « fériés » (a)) ; false = jour non appris
static bool learnDM(int64_t t, int & dow, int & mod) {
  struct tm lt;
  plat::localTime(t, &lt);
  dow = lt.tm_wday;
  mod = lt.tm_hour * 60 + lt.tm_min;
  if (g_pred.holidayLearn == HL_DAY || !isPublicHoliday(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)) return true;
  if (g_pred.holidayLearn == HL_SKIP) return false;
  dow = 0;
  return true;
}

// ── Mémoire du profil (levier « récence ») ───────────────────────────
// Montée : moyenne simple des n premiers relevés (poids 1/(n + 1)) ; ensuite poids fixe 1/N, soit
// a_k = λ·a_(k−1) + (1 − λ)·x_k avec λ = 1 − 1/N : le relevé d'il y a j relevés pèse λ^j, la
// constante de temps est N relevés (N/3 semaines), la demi-vie N·ln 2 relevés. n tient sur 5 bits :
// au-delà de 31, une cellule saturée prend directement 1/N.
static const int PROF_N_BITS_MAX = 31;
int profCountMax(int N) { return N < 2 ? 2 : N > PROF_N_BITS_MAX ? PROF_N_BITS_MAX : N; }
float profWeight(int n, int N) {
  if (N < 2) N = 2;
  if (n < 0) n = 0;
  if (n >= PROF_N_BITS_MAX) return 1.f / N;
  return 1.f / (n + 1 < N ? n + 1 : N);
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
  wxS    = (WxStats *)plat::psAlloc(sizeof(WxStats) * VN_MAX_ST);
  dispSince = (int64_t *)plat::psAlloc(sizeof(int64_t) * VN_MAX_ST);
  dispPendDev = (float *)plat::psAlloc(sizeof(float) * VN_MAX_ST);
  dispPendCell = (int16_t *)plat::psAlloc(sizeof(int16_t) * VN_MAX_ST);
  if (!dispPendDev || !dispPendCell) return false;
  if (!st || !ringT || !ring || !net || !recT || !rec || !prof || !cellN || !alerts || !wxS || !dispSince) return false;
  if (!days.init()) return false;
  memset(ring, 255, (size_t)VN_RING_SLOTS * VN_MAX_ST);
  memset(rec, 255, (size_t)VN_RECENT_SLOTS * VN_MAX_ST);
  return true;
}

WxUse Model::wxUseAt(int64_t now) const {
  if (!wx.valid) return WXU_NONE;
  if (now > 0 && wx.fetchedAt > 0 && now - wx.fetchedAt > 3 * VN_WX_REFRESH_S) return WXU_STALE;
  return WXU_OK;
}

// Météo du moment dans un échantillon 5 min (inconnue si absente ou périmée)
static void stampWeather(const Model & m, NetSample & ns, int64_t now) {
  netWxUnknown(ns);
  if (m.wxUseAt(now) != WXU_OK) return;
  const Weather & w = m.wx;
  ns.wxTemp = (int8_t)lroundf(clampf(w.temp, -127, 127));
  ns.wxRain = (uint8_t)lroundf(clampf(w.rain * 10, 0, 254));
  ns.wxWind = (uint8_t)lroundf(clampf(w.wind, 0, 254));
  ns.wxCode = (uint8_t)(w.code >= 0 && w.code < 255 ? w.code : WX_NA);
  ns.wxProb = (w.nfc > 0 && w.fc[0].prob >= 0) ? (uint8_t)(w.fc[0].prob > 100 ? 100 : w.fc[0].prob) : WX_NA;
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
    int dc = (int)r[k].cap - (int)s.cap;
    if (g_pred.rejuvN > 0 && s.cap > 0 && abs(dc) >= 2 && abs(dc) * 10 >= s.cap) {
      ProfCell * p = prof + (size_t)i * PROF_STRIDE;
      for (int q = 0; q < PROF_STRIDE; q++) if (p[q].n > g_pred.rejuvN) p[q].n = (uint8_t)g_pred.rejuvN;
    }
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
  bool newSlot = false;
  if (head < 0 || net[head].t != slotT) {
    newSlot = true;
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
      netWxUnknown(ns);
      if (!real) {
        memset(row, 255, VN_MAX_ST);
        ns.avail = ns.docks = ns.dis = ns.bikesRaw = 0xFFFF;
        continue;
      }
      stampWeather(*this, ns, now);
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

      // Profils : moyenne simple puis décroissance exponentielle (profWeight)
      int dow, mod;
      const bool learnDay = learnDM(t, dow, mod);
      int cell = dow * DAYQ + mod / 15;
      const int decN = g_pred.profDecayN, nMax = profCountMax(decN);
      for (int i = 0; i < n && learnDay; i++) {
        const Station & s = st[i];
        if (!s.active || !s.live || s.status == ST_CLOSED) continue;
        ProfCell & c = prof[(size_t)i * PROF_STRIDE + cell];
        learnDispersion(i, cell, t);
        float x = s.bikes * 256.f;
        if (c.n == 0) { c.a = (uint16_t)clampf(x, 0, 65535); c.n = 1; }
        else {
          float a = c.a + (x - c.a) * profWeight(c.n, decN);
          c.a = (uint16_t)clampf(a + 0.5f, 0, 65535);
          if (c.n < nMax) c.n++;
          else if (c.n > nMax) c.n = (uint8_t)nMax;     // N raccourci depuis l'écriture du fichier
        }
      }
      if (learnDay && cellN[cell] < 255) cellN[cell]++;
      learnSamples++;
      if (!learnSince) learnSince = now;
      learnWeather(t);
    }
  }
  net[head].dep = (uint16_t)(net[head].dep + dep);
  net[head].ret = (uint16_t)(net[head].ret + ret);

  lastUpdate = now;
  dirty = true;
  freshReading = true;
  recompute(now);
  freshReading = false;
  // Résumé du jour : la tranche avec les états qu'elle vient de produire (statut, écart au profil)
  if (newSlot) days.addSlot(*this, slotT);
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

bool Model::holidayProfile(int64_t t) const { return g_pred.holidaySunday && holidayAt(t); }

bool Model::profileAt(int s, int64_t t, float & avg, float & nEff) const {
  if (s < 0 || s >= n) return false;
  int dow, mod;
  profDM(t, dow, mod);
  return profileLocal(prof + (size_t)s * PROF_STRIDE, dow, mod, avg, nEff);
}

// ── Dispersion apprise (levier 1a) ───────────────────────────────────
// À chaque tranche de 5 min, avant que le relevé ne nourrisse la moyenne : |relevé − profil| (le
// profil tel que l'alerte le lit, hors échantillon), mis en attente et versé à la cellule à la tranche
// SUIVANTE — le relevé est donc toujours jugé contre une dispersion qui ne le contient pas (sinon un
// incident élargit sa propre tolérance : mesuré, ± 4 → ± 11 en deux relevés sur un profil de 3 semaines).
// Moyenne mobile : poids 1/(dn + 1) pour les 7 premiers relevés, puis le même 1/n que la moyenne
// (n ≥ 8). Écart écrêté à 1,5 D + ½ vélo, D = la dispersion lissée du créneau au moment du relevé
// dès qu'elle est connue (à la Huber : un relevé aberrant — un incident — ne fait monter D que de
// quelques %, au lieu d'élargir sa propre tolérance et celle des semaines suivantes ; sur un bruit
// gaussien, D en sort sous-estimé de ~3 %, ce que le réglage de k absorbe). Seulement
// quand le profil est assez mûr pour être évalué (confiance ≥ 50 %) : avant, l'écart mesure surtout
// la jeunesse du profil. Arrondi au 1/16 de vélo par un tramage déterministe (sans lui, un pas de mise
// à jour < ½ unité serait perdu et la dispersion resterait bloquée loin de sa valeur).
static inline float dither01(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
  return (h >> 8) * (1.f / 16777216.f);
}

void Model::learnDispersion(int i, int cell, int64_t t) {
  // 1. l'écart de la tranche précédente, mis en attente : versé maintenant
  if (dispPendCell[i] > 0) {
    int pc = dispPendCell[i] - 1;
    ProfCell & c = prof[(size_t)i * PROF_STRIDE + pc];
    float dev = dispPendDev[i];
    float x = clampf(dev * DISP_SCALE, 0, 255);
    int nw = c.n >= PROF_N_BITS_MAX && g_pred.profDecayN > PROF_N_BITS_MAX ? g_pred.profDecayN : c.n;
    int w = c.dn < DISP_DN_MAX ? c.dn + 1 : (nw > 8 ? nw : 8);
    float v = c.d + (x - c.d) / w;
    int iv = (int)floorf(v);
    if (v - iv > dither01((uint32_t)i * PROF_STRIDE + (uint32_t)pc, learnSamples)) iv++;
    c.d = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
    if (c.dn < DISP_DN_MAX) c.dn++;
    dispPendCell[i] = 0;
  }
  // 2. l'écart du relevé courant, contre le profil d'avant ce relevé
  float p, nEff;
  if (!profileAt(i, t, p, nEff) || nEff < ANOM_MIN_CONF * CONF_N) return;
  float dev = fabsf(st[i].bikes - p), dref, dcnt;
  if (dispersionAt(i, t, dref, dcnt) && dcnt >= DISP_MIN_N) dev = fminf(dev, DISP_CLIP_K * dref + DISP_CLIP_B);
  dispPendDev[i] = dev;
  dispPendCell[i] = (int16_t)(cell + 1);
  if (!dispSince[i]) dispSince[i] = t;
}

// Dispersion d'une cellule : cellule propre complétée par les jours de même type tant qu'elle est jeune
// (la même règle que cellValue pour la moyenne)
static bool dispCell(const ProfCell * p, int dow, int q, float & v, float & cnt) {
  const ProfCell & own = p[dow * DAYQ + q];
  if (own.dn >= DISP_FULL_N) { v = own.d / DISP_SCALE; cnt = own.dn; return true; }
  bool weekend = (dow == 0 || dow == 6);
  float sum = 0, pn = 0;
  for (int d = 0; d < 7; d++) {
    if (d == dow || ((d == 0 || d == 6) != weekend)) continue;
    const ProfCell & c = p[d * DAYQ + q];
    if (!c.dn) continue;
    sum += (c.d / DISP_SCALE) * c.dn;
    pn += c.dn;
  }
  if (!own.dn && pn <= 0) return false;
  if (pn <= 0) { v = own.d / DISP_SCALE; cnt = own.dn; return true; }
  float w = own.dn / DISP_FULL_N;
  v = w * (own.d / DISP_SCALE) + (1 - w) * (sum / pn);
  cnt = own.dn + 0.5f * (pn > 12 ? 12 : pn);
  return true;
}

bool Model::dispersionAt(int s, int64_t t, float & d, float & cnt) const {
  if (s < 0 || s >= n) return false;
  int dow, mod;
  profDM(t, dow, mod);
  const ProfCell * p = prof + (size_t)s * PROF_STRIDE;
  // lissage ±15 min (0,25 / 0,5 / 0,25), comme le profil ; compte = le plus faible des trois
  static const float W[3] = { 0.25f, 0.5f, 0.25f };
  float sv = 0, sw = 0, sn = 1e9f;
  int q = mod / 15;
  for (int o = -1; o <= 1; o++) {
    int qq = q + o, dd = dow;
    if (qq < 0) { qq += DAYQ; dd = (dd + 6) % 7; }
    if (qq >= DAYQ) { qq -= DAYQ; dd = (dd + 1) % 7; }
    float cv, cn;
    if (!dispCell(p, dd, qq, cv, cn)) { sn = 0; continue; }
    sv += W[o + 1] * cv; sw += W[o + 1];
    if (cn < sn) sn = cn;
  }
  if (sw < 0.5f) return false;
  d = sv / sw;
  cnt = sn;
  return true;
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
//   fc(h) = actuel + c·[ΔP(h) + M(h)] + (1 − 0,6c)·tendance amortie(h)
//   M(h)  = Σ sur les pas jusqu'à h de (mouvement du profil sur le pas) × f(pas)
//   f     = −α·(x − x̄) (wxF, nullptr = aucune correction météo), |c·M| ≤ 40 % de la capacité utile
// La part météo c·M est rendue dans wxPart (0 sans correction). Elle ne touche que la
// trajectoire : le point 0 reste le relevé réel.
static const float WX_MAX_SHARE = 0.4f;
// (jour de profil, minute) de chaque pas : sans le levier 5a-ii, le jour de départ et la minute qui
// déborde (profileLocal passe minuit tout seul) — exactement la règle d'avant ; avec, chaque pas est
// daté pour qu'un jour férié — et lui seul — prenne le profil du dimanche.
struct StepDM { int16_t dow[128]; int16_t mod[128]; };
static void stepDays(int64_t now, int stepMin, int nSteps, StepDM & d) {
  int dow0, mod0;
  localDM(now, dow0, mod0);
  for (int k = 0; k < nSteps && k < 128; k++) {
    if (!g_pred.holidaySunday) { d.dow[k] = (int16_t)dow0; d.mod[k] = (int16_t)(mod0 + k * stepMin); continue; }
    int dw, md;
    profDM(now + (int64_t)k * stepMin * 60, dw, md);
    d.dow[k] = (int16_t)dw; d.mod[k] = (int16_t)md;
  }
}

static void projectSteps(const ProfCell * p, const StepDM & dm, float cur, float capE, float slope,
                         int stepMin, int nSteps, float * out, float * conf, const float * wxF, float * wxPart) {
  float p0, n0;
  bool has0 = profileLocal(p, dm.dow[0], dm.mod[0], p0, n0);
  const float tau = 25.f;
  float prev = p0, M = 0;
  for (int k = 0; k < nSteps; k++) {
    int h = k * stepMin;
    float dS = 0, cS = 0, ph, nh;
    if (has0 && profileLocal(p, dm.dow[k], dm.mod[k], ph, nh)) {
      dS = ph - p0;
      cS = clampf((n0 < nh ? n0 : nh) / CONF_N, 0, 1);
      if (wxF && k > 0) M += (ph - prev) * wxF[k];
      prev = ph;
    }
    float wxC = clampf(cS * M, -WX_MAX_SHARE * capE, WX_MAX_SHARE * capE);
    float dT = slope * tau * (1 - expf(-h / tau));
    float v = cur + cS * dS + wxC + (1 - 0.6f * cS) * dT;
    out[k] = clampf(v, 0, capE);
    if (conf) conf[k] = cS;
    if (wxPart) wxPart[k] = wxC;
  }
}

// Facteurs météo f(pas) = −α·(x − x̄) d'une station ; false = aucune correction (comportement d'avant)
static bool wxFactors(const Model & m, int i, int64_t now, int stepMin, int nSteps, float * f) {
  if (!g_pred.wxFlow || m.wxUse != WXU_OK) return false;
  float a = m.wxAlpha;
  if (a == 0) return false;
  for (int k = 0; k < nSteps; k++) {
    f[k] = 0;
    if (k == 0) continue;
    float x;                                  // pénalité attendue au milieu du pas
    int64_t tau = now + (int64_t)(k * stepMin - stepMin / 2) * 60;
    if (m.wxPenaltyAt(now, tau, x)) {
      // seulement un temps PIRE que la normale de l'heure : l'amplification par beau temps
      // (dx < 0) coûtait par temps sec sans rien gagner (docs/mesures-prevision.md, étape 3a)
      float dx = x - m.wxClimAt(tau);
      f[k] = dx > 0 ? -a * dx : 0;
    }
  }
  return true;
}

static void forecastStation(Model & m, int i, const StepDM & dm, int64_t now) {
  Station & s = m.st[i];
  float cur = s.bikes, capE = s.capEff();
  s.etaEmpty = s.etaFull = -1;
  s.conf = 0;
  s.delta30 = 0;
  if (s.status == ST_CLOSED || !s.live) {
    for (int k = 0; k < VN_FC_STEPS; k++) { s.fc[k] = cur; s.fcWx[k] = 0; }
    return;
  }
  float oldest;
  float slope = trendSlope(m, i, now, oldest);
  if (oldest >= 0) s.delta30 = (int16_t)lroundf(cur - oldest);

  float conf[VN_FC_STEPS], wxF[VN_FC_STEPS];
  bool wxOn = wxFactors(m, i, now, 5, VN_FC_STEPS, wxF);
  projectSteps(m.prof + (size_t)i * PROF_STRIDE, dm, cur, capE, slope, 5, VN_FC_STEPS, s.fc, conf,
               wxOn ? wxF : nullptr, s.fcWx);
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

bool Model::project(int i, int64_t now, int stepMin, int nSteps, float * out, float * conf, float * wxPart) const {
  if (i < 0 || i >= n || nSteps <= 0 || stepMin <= 0) return false;
  const Station & s = st[i];
  if (s.status == ST_CLOSED || !s.live) {             // même convention que forecastStation
    for (int k = 0; k < nSteps; k++) { out[k] = s.bikes; if (conf) conf[k] = 0; if (wxPart) wxPart[k] = 0; }
    return true;
  }
  if (nSteps > 128) nSteps = 128;                     // la courbe 24 h en a 97
  static StepDM dm;
  stepDays(now, stepMin, nSteps, dm);
  float oldest;
  float slope = trendSlope(*this, i, now, oldest);
  float wxF[128];
  bool wxOn = wxFactors(*this, i, now, stepMin, nSteps, wxF);
  projectSteps(prof + (size_t)i * PROF_STRIDE, dm, s.bikes, s.capEff(), slope, stepMin, nSteps, out, conf,
               wxOn ? wxF : nullptr, wxPart);
  return true;
}

// ── Météo apprise (étape 3) ──────────────────────────────────────────
// Pénalité x(τ) attendue : météo actuelle sur les 2 premières heures (étape 3) ; au-delà, avec
// g_pred.wxForecast (étape 4), la météo prévue de l'heure qui contient τ (pluie comptée pour son
// espérance). Au-delà de ce qu'on sait, aucune correction (x inconnue → facteur 0).
bool Model::wxPenaltyAt(int64_t now, int64_t tau, float & x) const {
  if (wxUse != WXU_OK) return false;
  // Sur 2 h, la météo actuelle qui persiste prévoit mieux que la prévision horaire (mesuré :
  // la prévision gagne quand la pluie commence, perd autant quand elle s'arrête, même parfaite) ;
  // au-delà, la météo prévue prend le relais (étape 4) — sinon, plus de correction.
  if (tau - now <= 2 * 3600) { x = wxNow; return true; }
  if (!g_pred.wxForecast) return false;
  int64_t h0 = wx.fetchedAt - wx.fetchedAt % 3600;          // fc[0] = heure en cours au relevé
  int idx = (int)((tau - h0) / 3600);
  if (idx < 0 || idx >= wx.nfc) return false;
  x = idx == 0 ? wxNow : wxPenaltyHour(wx.fc[idx]);
  return true;
}

static const float WX_DECAY = 1.f - 1.f / 1800.f;      // ~60 jours de fenêtres de 30 min (7 h–22 h)
static const float WX_RES_CLIP = 6.f;                  // résidu écrêté (camions, incidents)
static const float WX_MIN_RAIN = 12.f;                 // fenêtres pluvieuses avant de juger
static const float WX_ALPHA_MIN = -0.5f, WX_ALPHA_MAX = 1.f;
static const float WX_SIGNIF = 3.f;                    // seuil de significativité (erreurs-types)

static void wxDecay(WxStats & w) {
  float * f = (float *)&w;
  for (size_t k = 0; k < sizeof(WxStats) / sizeof(float); k++) f[k] *= WX_DECAY;
}

float Model::wxClimAt(int64_t t) const {
  struct tm lt;
  plat::localTime(t, &lt);
  int h = lt.tm_hour;
  return wxClim.sw[h] > 0 ? wxClim.sx[h] / wxClim.sw[h] : 0;
}

// Une fenêtre de 30 min se termine à slotT (tranche :00 ou :30, 7 h 30 → 22 h) : pour chaque
// station ouverte, mouvement observé contre mouvement du profil, sous la météo OBSERVÉE pendant
// la fenêtre (échantillons 5 min, étape 2). Ne se nourrit que du flux GBFS (anneau 5 min) et
// d'Open-Meteo ; une station fermée, inactive ou sans relevé aux deux bouts est exclue.
void Model::learnWeather(int64_t slotT) {
  if (slotT % 1800 || count < 7) return;
  int dow, mod;
  localDM(slotT, dow, mod);
  if (mod < 7 * 60 + 30 || mod > 22 * 60) return;
  int i0 = ringIndex(0), i6 = ringIndex(6);
  if (i0 < 0 || i6 < 0 || ringT[i0] != slotT || ringT[i6] != slotT - 1800) return;
  float x = 0;
  for (int b = 1; b <= 6; b++) {
    const NetSample & ns = net[ringIndex(b)];
    if (ns.avail == 0xFFFF || !netWxKnown(ns)) return;      // fenêtre incomplète : rien appris
    x += wxPenalty(ns.wxTemp, ns.wxRain / 10.f, ns.wxWind == WX_NA ? 0.f : ns.wxWind, ns.wxCode == WX_NA ? 0 : ns.wxCode);
  }
  x /= 6;
  // x̄ₕ : climatologie de la pénalité à l'heure du milieu de la fenêtre
  struct tm lm;
  plat::localTime(slotT - 900, &lm);
  int hh = lm.tm_hour;
  for (int k = 0; k < 24; k++) { wxClim.sx[k] *= WX_DECAY; wxClim.sw[k] *= WX_DECAY; }
  wxClim.sx[hh] += x; wxClim.sw[hh] += 1;
  float xbar = wxClim.sx[hh] / wxClim.sw[hh];
  wxDecay(wxG);
  for (int i = 0; i < n; i++) wxDecay(wxS[i]);
  float g = 0;
  int used = 0;
  const uint8_t * r0 = ring + (size_t)i0 * VN_MAX_ST, * r6 = ring + (size_t)i6 * VN_MAX_ST;
  for (int i = 0; i < n; i++) {
    const Station & s = st[i];
    if (!s.active || !s.live || s.status == ST_CLOSED || r0[i] == 255 || r6[i] == 255) continue;
    float pa, na, pb, nb;
    if (!profileAt(i, slotT - 1800, pa, na) || !profileAt(i, slotT, pb, nb)) continue;
    if (na < CONF_N || nb < CONF_N) continue;                 // profil pas encore mûr : référence trop bruitée
    float d = pb - pa;
    float r = clampf((float)r0[i] - (float)r6[i] - d, -WX_RES_CLIP, WX_RES_CLIP);
    float z = (x - xbar) * d;
    WxStats * ws[2] = { &wxG, &wxS[i] };
    for (WxStats * w : ws) { w->sdd += d * d; w->sdz += d * z; w->szz += z * z; w->sdr += d * r; w->szr += z * r; w->sn += 1; }
    wxS[i].sgg += (z * r) * (z * r);                        // grappe = l'observation elle-même
    if (x > 0.05f) wxS[i].nRain += 1;
    g += z * r;
    used++;
  }
  wxG.sgg += g * g;                                         // grappe = la fenêtre
  if (used && x > 0.05f) wxG.nRain += 1;                    // fenêtre pluvieuse réellement apprise
}

// α̂ (régression à deux termes β·d − α·z) et son erreur-type robuste ; renvoie α appliqué :
// 0 tant que |α̂| ne dépasse pas 3 erreurs-types, puis α̂ tel quel, borné à [−0,5 ; 1].
// Mesuré (docs/mesures-prevision.md, étape 3a) : un rétrécissement de 2 erreurs-types divisait
// l'effet par deux ; un seuil de 2 erreurs-types, vérifié en continu, inventait une correction
// 4 % des jours dans le monde où la pluie est sans effet ; 3 : jamais, pour le même gain.
static float alphaOf(const WxStats & w, float & raw, float & se) {
  raw = se = 0;
  double det = (double)w.sdd * w.szz - (double)w.sdz * w.sdz;
  if (w.sn < 1 || w.sdd <= 1e-6f || det <= 1e-9 * (double)w.sdd * w.szz || det <= 0) return 0;
  raw = (float)(-((double)w.sdd * w.szr - (double)w.sdz * w.sdr) / det);
  double szzEff = det / w.sdd;                               // Σz² une fois d retiré
  se = (float)(sqrt((double)w.sgg) / szzEff);
  return fabsf(raw) > WX_SIGNIF * se ? clampf(raw, WX_ALPHA_MIN, WX_ALPHA_MAX) : 0;
}

void Model::updateWxCoef() {
  float a = alphaOf(wxG, wxAlphaRaw, wxAlphaSE);
  if (wxG.nRain < WX_MIN_RAIN) { wxSrc = WXC_DEFAULT; wxAlpha = 0; }
  else { wxAlpha = a; wxSrc = a != 0 ? WXC_LEARNED : WXC_NULL; }
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
//  • Tolérance T (vélos), deux règles (g_alertOpt.disp, docs/mesures-prevision.md) :
//    – formule fixe : max(ANOM_MIN_BIKES, ANOM_REL_CAP × capacité utile) × (2 − c) — à
//      confiance pleine, max(4 vélos, 20 % de la capacité) ; à c = 0,5, 1,5 × plus large ;
//    – dispersion apprise (levier 1a) : T = max(ANOM_MIN_BIKES, k × D), D = écart absolu moyen
//      du relevé au profil, mesuré pour CE créneau et CETTE station ; tant que D repose sur
//      moins de DISP_MIN_N relevés, retour à la formule fixe (dit à l'écran). Décision sur les
//      nombres affichés (écart = N − P arrondi, T arrondie) : l'écran ne peut pas montrer
//      « écart 6, tolérance ± 6 » à côté d'une alerte.
//  • Écart minimal : ANOM_MIN_BIKES = 4 vélos, plancher de T — un retrait groupé de
//      2–3 vélos est du bruit ordinaire, même sur une petite station.
//  • Hors tolérance si |N − P| > T (strictement).
//  • Inertie (levier 1b) : l'alerte ne sonne que si l'écart, de même sens, tient sur
//      g_alertOpt.persist relevés de 5 min consécutifs (le relevé courant et les tranches
//      précédentes de l'historique, chacune jugée contre le profil de SON instant) ; avant,
//      l'état est AN_PENDING (affiché, sans alerte). Une tranche manquante interrompt la série.
// Limite connue : le relevé courant vient d'alimenter la cellule du profil (avec le poids
// profWeight), ce qui rapproche P de N d'au plus ce poids : l'écart est
// légèrement sous-estimé, jamais gonflé.
struct AnomEval { float p, c, tol, disp, dcnt; uint8_t src; };

static bool anomEval(const Model & m, const Station & s, int i, int64_t t, AnomEval & e) {
  float nEff;
  if (!m.profileAt(i, t, e.p, nEff)) return false;
  e.c = clampf(nEff / CONF_N, 0, 1);
  e.disp = e.dcnt = 0;
  bool dk = m.dispersionAt(i, t, e.disp, e.dcnt) && e.dcnt >= DISP_MIN_N;
  if (!dk) e.disp = e.dcnt = 0;
  // levier « fériés » (c) : tolérance élargie un jour férié (holidayScale = 1 : inchangée)
  float hs = g_alertOpt.holidayScale != 1.f && holidayAt(t) ? g_alertOpt.holidayScale : 1.f;
  float formula = fmaxf(ANOM_MIN_BIKES, ANOM_REL_CAP * s.capEff()) * (2.f - e.c) * g_alertOpt.scale * hs;
  if (g_alertOpt.disp && dk) { e.tol = (float)lroundf(fmaxf(ANOM_MIN_BIKES, g_alertOpt.k * e.disp) * hs); e.src = ATS_DISP; }
  else { e.tol = formula; e.src = g_alertOpt.disp ? ATS_FORMULA_YOUNG : ATS_FORMULA; }
  return true;
}
// Écart signé hors tolérance (+1 au-dessus, −1 en dessous, 0 dans la tolérance)
static int anomSide(const AnomEval & e, float bikes) {
  float dev = e.src == ATS_DISP ? bikes - (float)lroundf(e.p) : bikes - e.p;
  return fabsf(dev) > e.tol ? (dev > 0 ? 1 : -1) : 0;
}

static void anomalyStation(const Model & m, Station & s, int i, int64_t now, bool activeHours) {
  s.anomExp = -1; s.anomTol = 0; s.anomConf = 0; s.anomSrc = ATS_FORMULA;
  s.anomDispOk = false; s.anomDisp10 = 0; s.anomDispDays = 0;
  int side = 0;
  AnomEval e;
  bool has = s.live && s.status != ST_CLOSED && anomEval(m, s, i, now, e);
  if (!s.live || s.status == ST_CLOSED) s.anom = AN_CLOSED;
  else {
    if (has) {
      s.anomExp = (int16_t)lroundf(e.p); s.anomConf = (uint8_t)lroundf(e.c * 100);
      if (e.dcnt > 0) {
        s.anomDispOk = true;
        s.anomDisp10 = (uint8_t)clampf(lroundf(e.disp * 10), 0, 255);
        int64_t since = m.dispSince[i];
        s.anomDispDays = since > 0 && now > since ? (uint16_t)clampf((float)((now - since) / 86400), 0, 65535) : 0;
      }
    }
    if (!activeHours) s.anom = AN_OFF_HOURS;
    else if (!has) s.anom = AN_NO_PROFILE;
    else if (e.c < ANOM_MIN_CONF) s.anom = AN_YOUNG;
    else {
      s.anomTol = (int16_t)lroundf(e.tol);
      s.anomSrc = e.src;
      side = anomSide(e, s.bikes);
      s.anom = side ? AN_PENDING : AN_NORMAL;
    }
  }
  // Inertie : la série ne compte que des relevés NEUFS (un recalcul après la météo ou les réglages
  // ne fait pas avancer le temps), chacun jugé une fois, à son instant ; une tranche de 5 min sans
  // relevé, un retour dans la tolérance ou un changement de côté la remettent à zéro.
  if (m.freshReading) {
    int64_t slot = now - now % VN_RING_STEP_S;
    if (!side) { s.anomRun = 0; s.anomRunSlot = 0; s.anomRunSide = 0; }
    else if (s.anomRun && s.anomRunSide == side && s.anomRunSlot == slot) {}          // même tranche : déjà compté
    else if (s.anomRun && s.anomRunSide == side && s.anomRunSlot == slot - VN_RING_STEP_S) {
      if (s.anomRun < 255) s.anomRun++;
      s.anomRunSlot = slot;
    } else { s.anomRun = 1; s.anomRunSide = (int8_t)side; s.anomRunSlot = slot; }
  }
  if (!side) return;
  int need = g_alertOpt.persist < 1 ? 1 : g_alertOpt.persist;
  int run = s.anomRunSide == side ? s.anomRun : 0;
  if (run >= need || need == 1) s.anom = side > 0 ? AN_HIGH : AN_LOW;
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
  static StepDM fcDays;                                 // pas de la prévision 2 h, communs au parc
  stepDays(now, 5, VN_FC_STEPS, fcDays);
  // Météo : utilisée seulement si reçue et fraîche (3 périodes de VN_WX_REFRESH_S, comme le bandeau)
  wxUse = wxUseAt(now);
  wxNow = wxUse == WXU_OK ? wxPenalty(wx.temp, wx.rain, wx.wind, wx.code) : 0;
  wxXbar = wxClimAt(now);
  updateWxCoef();

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
    forecastStation(*this, i, fcDays, now);
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
// /vn_hist.bin : ver 1 (≤ v1.0.8) = échantillons sans météo ; ver 2 = NetSample avec météo.
// Une version 1 est relue (météo « inconnue »), une version inconnue ou une taille fausse
// est ignorée : jamais de lecture hors du fichier, jamais de plantage au démarrage.
static const uint32_t HIST_MAGIC = 0x31484E56;   // "VNH1" (le chiffre du magic ne suit pas la version)
static const uint16_t HIST_VER = 2;
struct NetSampleV1 {                              // format des versions ≤ 1.0.8
  int64_t  t;
  uint16_t avail, docks, dis, bikesRaw;
  uint16_t dep, ret;
  uint8_t  empty, full;
};
static_assert(sizeof(NetSampleV1) == 24, "format v1 de /vn_hist.bin");
static_assert(sizeof(NetSample) == 32, "format v2 de /vn_hist.bin");
// /vn_prof.bin : ver 1 (≤ v1.0.8) = profils (cellule de 3 octets) ; ver 2 = + statistiques météo
// (global, puis une par station dans l'ordre des sid) ; ver 3 = cellule de 4 octets (+ dispersion,
// levier 1a) et, en fin de fichier, le premier relevé de dispersion de chaque station.
// Une version 1 ou 2 est relue cellule par cellule (moyenne et n intacts, dispersion inconnue :
// la tolérance retombe sur la formule fixe le temps de la réapprendre) ; une version 1 sans
// apprentissage météo. Version inconnue, taille fausse : fichier ignoré (profil à zéro), jamais
// de lecture hors du fichier.
static const uint32_t PROF_MAGIC = 0x31504E56;   // "VNP1"
static const uint16_t PROF_VER = 3;
static const size_t PROF_CELL_V12 = 3;            // { uint16 a ; uint8 n } des versions 1 et 2
static_assert(sizeof(WxStats) == 32 && sizeof(WxClimate) == 192, "format v2 de /vn_prof.bin");
static_assert(sizeof(ProfCell) == 4, "format v3 de /vn_prof.bin");
static const char * HIST_PATH = "/vn_hist.bin";
static const char * PROF_PATH = "/vn_prof.bin";
// /vn_days.bin : résumés par jour, format et migration décrits dans core/days.cpp

bool Model::serialize(Blob & out) const {
  out = Blob();
  if (n <= 0) return false;
  FileHdr h{};
  h.nSt = (uint16_t)n; h.head = head; h.count = count;
  h.learnSince = learnSince; h.learnSamples = learnSamples; h.savedAt = lastUpdate;
  // Profils
  size_t sz = sizeof(h) + (size_t)n * 8 + (size_t)n * PROF_STRIDE * sizeof(ProfCell) + PROF_STRIDE
              + sizeof(WxClimate) + sizeof(WxStats) * (1 + (size_t)n) + sizeof(int64_t) * (size_t)n;
  uint8_t * b = (uint8_t *)plat::psAlloc(sz);
  if (!b) return false;
  h.magic = PROF_MAGIC; h.ver = PROF_VER;
  size_t o = 0;
  memcpy(b + o, &h, sizeof(h)); o += sizeof(h);
  for (int i = 0; i < n; i++) { memcpy(b + o, st[i].sid, 8); o += 8; }
  memcpy(b + o, prof, (size_t)n * PROF_STRIDE * sizeof(ProfCell)); o += (size_t)n * PROF_STRIDE * sizeof(ProfCell);
  memcpy(b + o, cellN, PROF_STRIDE); o += PROF_STRIDE;
  memcpy(b + o, &wxClim, sizeof(WxClimate)); o += sizeof(WxClimate);
  memcpy(b + o, &wxG, sizeof(WxStats)); o += sizeof(WxStats);
  memcpy(b + o, wxS, sizeof(WxStats) * (size_t)n); o += sizeof(WxStats) * (size_t)n;
  memcpy(b + o, dispSince, sizeof(int64_t) * (size_t)n); o += sizeof(int64_t) * (size_t)n;
  out.prof = b; out.profLen = o;
  // Historique 24 h
  sz = sizeof(h) + (size_t)n * 8 + VN_RING_SLOTS * (8 + sizeof(NetSample) + (size_t)n);
  b = (uint8_t *)plat::psAlloc(sz);
  if (!b) { plat::psFree(out.prof); out = Blob(); return false; }
  h.magic = HIST_MAGIC; h.ver = HIST_VER;
  o = 0;
  memcpy(b + o, &h, sizeof(h)); o += sizeof(h);
  for (int i = 0; i < n; i++) { memcpy(b + o, st[i].sid, 8); o += 8; }
  memcpy(b + o, ringT, VN_RING_SLOTS * 8); o += VN_RING_SLOTS * 8;
  memcpy(b + o, net, VN_RING_SLOTS * sizeof(NetSample)); o += VN_RING_SLOTS * sizeof(NetSample);
  for (int r = 0; r < VN_RING_SLOTS; r++) { memcpy(b + o, ring + (size_t)r * VN_MAX_ST, n); o += n; }
  out.hist = b; out.histLen = o;
  // Résumés par jour (facultatifs : leur échec n'empêche pas de sauver le reste)
  days.serialize(*this, out.days, out.daysLen);
  return true;
}

bool Model::writeSaved(Blob & b) {
  bool ok = true;
  if (b.prof) { ok &= plat::fsWrite(PROF_PATH, b.prof, b.profLen); plat::psFree(b.prof); }
  if (b.hist) { ok &= plat::fsWrite(HIST_PATH, b.hist, b.histLen); plat::psFree(b.hist); }
  if (b.days) { ok &= plat::fsWrite(DAYS_PATH, b.days, b.daysLen); plat::psFree(b.days); }
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
      size_t cell = h.ver >= 3 ? sizeof(ProfCell) : PROF_CELL_V12;
      size_t need = sizeof(h) + (size_t)h.nSt * 8 + (size_t)h.nSt * PROF_STRIDE * cell + PROF_STRIDE;
      if (h.ver >= 2) need += sizeof(WxClimate) + sizeof(WxStats) * (1 + (size_t)h.nSt);
      if (h.ver >= 3) need += sizeof(int64_t) * (size_t)h.nSt;
      if (h.magic == PROF_MAGIC && h.ver >= 1 && h.ver <= PROF_VER && h.nSt <= VN_MAX_ST && (size_t)psz == need) {
        mapSids(*this, b + sizeof(h), h.nSt, map);
        const uint8_t * pp = b + sizeof(h) + (size_t)h.nSt * 8;
        for (int j = 0; j < h.nSt; j++) {
          if (map[j] < 0) continue;
          ProfCell * dst = prof + (size_t)map[j] * PROF_STRIDE;
          const uint8_t * src = pp + (size_t)j * PROF_STRIDE * cell;
          if (h.ver >= 3) { memcpy(dst, src, PROF_STRIDE * sizeof(ProfCell)); continue; }
          for (int q = 0; q < PROF_STRIDE; q++) {     // migration 3 → 4 octets : dispersion inconnue
            const uint8_t * c3 = src + (size_t)q * PROF_CELL_V12;
            uint16_t a; memcpy(&a, c3, 2);
            dst[q].a = a;
            int nMax = profCountMax(g_pred.profDecayN);
            dst[q].n = c3[2] > nMax ? nMax : c3[2];
            dst[q].dn = 0; dst[q].d = 0;
          }
        }
        const uint8_t * wp = pp + (size_t)h.nSt * PROF_STRIDE * cell;
        memcpy(cellN, wp, PROF_STRIDE); wp += PROF_STRIDE;
        if (h.ver >= 2) {
          // Statistiques relues seulement si toutes finies et de signe plausible ; sinon on repart de zéro
          WxClimate c; memcpy(&c, wp, sizeof(c));
          static WxStats tmp[VN_MAX_ST + 1];
          memcpy(tmp, wp + sizeof(WxClimate), sizeof(WxStats) * (1 + (size_t)h.nSt));
          wp += sizeof(WxClimate) + sizeof(WxStats) * (1 + (size_t)h.nSt);
          bool sane = true;
          for (int k = 0; k < 24; k++) sane &= isfinite(c.sx[k]) && isfinite(c.sw[k]) && c.sw[k] >= 0;
          for (int j = 0; j <= h.nSt; j++) {
            const float * f = (const float *)&tmp[j];
            for (size_t k = 0; k < sizeof(WxStats) / sizeof(float); k++) sane &= isfinite(f[k]);
            sane &= tmp[j].sdd >= 0 && tmp[j].szz >= 0 && tmp[j].sgg >= 0 && tmp[j].sn >= 0;
          }
          if (sane) {
            wxClim = c;
            wxG = tmp[0];
            for (int j = 0; j < h.nSt; j++) if (map[j] >= 0) wxS[map[j]] = tmp[1 + j];
          }
          updateWxCoef();
        }
        if (h.ver >= 3) {
          // date du premier relevé de dispersion : une valeur impossible (future, négative) est oubliée
          for (int j = 0; j < h.nSt; j++) {
            if (map[j] < 0) continue;
            int64_t v; memcpy(&v, wp + (size_t)j * sizeof(int64_t), sizeof(v));
            dispSince[map[j]] = (v > 0 && (h.savedAt <= 0 || v <= h.savedAt)) ? v : 0;
          }
        }
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
      size_t nsz = h.ver == 1 ? sizeof(NetSampleV1) : sizeof(NetSample);
      size_t need = sizeof(h) + (size_t)h.nSt * 8 + VN_RING_SLOTS * (8 + nsz + (size_t)h.nSt);
      bool fresh = now <= 0 || (h.savedAt > 0 && now - h.savedAt < 20 * 3600);
      if (h.magic == HIST_MAGIC && (h.ver == 1 || h.ver == HIST_VER) && h.nSt <= VN_MAX_ST && (size_t)hsz == need && fresh
          && h.head >= 0 && h.head < VN_RING_SLOTS && h.count >= 0 && h.count <= VN_RING_SLOTS) {
        mapSids(*this, b + sizeof(h), h.nSt, map);
        size_t o = sizeof(h) + (size_t)h.nSt * 8;
        memcpy(ringT, b + o, VN_RING_SLOTS * 8); o += VN_RING_SLOTS * 8;
        if (h.ver == 1) {                           // migration : mêmes champs, météo inconnue
          for (int r = 0; r < VN_RING_SLOTS; r++) {
            NetSampleV1 v; memcpy(&v, b + o + (size_t)r * sizeof(v), sizeof(v));
            NetSample & ns = net[r];
            memset(&ns, 0, sizeof(ns));
            ns.t = v.t; ns.avail = v.avail; ns.docks = v.docks; ns.dis = v.dis; ns.bikesRaw = v.bikesRaw;
            ns.dep = v.dep; ns.ret = v.ret; ns.empty = v.empty; ns.full = v.full;
            netWxUnknown(ns);
          }
        } else {
          memcpy(net, b + o, VN_RING_SLOTS * sizeof(NetSample));
        }
        o += VN_RING_SLOTS * nsz;
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
  // Résumés par jour : relus indépendamment (un fichier illisible les laisse vides, rien d'autre)
  long dsz = plat::fsSize(DAYS_PATH);
  if (dsz > (long)sizeof(DaysFileHdr)) {
    uint8_t * b = (uint8_t *)plat::psAlloc(dsz);
    if (b && plat::fsRead(DAYS_PATH, b, dsz)) {
      DaysFileHdr h; memcpy(&h, b, sizeof(h));
      if (DayBook::check(b, (size_t)dsz)) {           // tout vérifié avant de créer la moindre station
        mapSids(*this, b + sizeof(h), h.nSt, map);
        if (days.load(b, (size_t)dsz, map, h.nSt, now)) any = true;
        else plat::logf("[model] %s illisible : r\xC3\xA9sum\xC3\xA9s par jour ignor\xC3\xA9s", DAYS_PATH);
      } else plat::logf("[model] %s : version ou en-t\xC3\xAAte inconnus, ignor\xC3\xA9", DAYS_PATH);
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
  memset(&wxG, 0, sizeof(wxG));
  memset(&wxClim, 0, sizeof(wxClim));
  memset(wxS, 0, sizeof(WxStats) * VN_MAX_ST);
  memset(dispSince, 0, sizeof(int64_t) * VN_MAX_ST);
  memset(dispPendCell, 0, sizeof(int16_t) * VN_MAX_ST);
  updateWxCoef();
  head = -1; count = 0; rHead = -1; rCount = 0;
  learnSince = 0; learnSamples = 0;
  days.clear();
  plat::fsRemove(HIST_PATH);
  plat::fsRemove(PROF_PATH);
  plat::fsRemove(DAYS_PATH);
  version++;
}

} // namespace vn
