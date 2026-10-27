#include "regul.h"
#include "../platform/platform.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>

namespace vn {

RegCfg g_regCfg;
RegState g_reg;

static void cpy(char * d, size_t n, const char * s) { snprintf(d, n, "%s", s ? s : ""); }
static int16_t clampN(int v) { return (int16_t)(v < -999 ? -999 : v > 999 ? 999 : v); }
static const int64_t TIME_MIN = 1600000000;      // avant : heure pas encore réglée (NTP)

// ── Réglage ──────────────────────────────────────────────────────────
void regCfgDefaults(RegCfg & c) {
  memset(&c, 0, sizeof(c));
#ifdef VN_ROLE_REGUL
  c.role = ROLE_REGUL;
#else
  c.role = ROLE_POSTE;
#endif
}

bool regCfgLoad() {
  regCfgDefaults(g_regCfg);
  long sz = plat::fsSize(REG_CFG_PATH);
  if (sz <= 0 || sz > 512) return false;
  char b[513];
  if (!plat::fsRead(REG_CFG_PATH, b, (size_t)sz)) return false;
  b[sz] = 0;
  for (char * line = strtok(b, "\n"); line; line = strtok(nullptr, "\n")) {
    char * eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char * v = eq + 1;
    if (!strcmp(line, "role")) g_regCfg.role = atoi(v) == 1 ? ROLE_REGUL : ROLE_POSTE;
    else if (!strcmp(line, "reg")) cpy(g_regCfg.lastReg, sizeof(g_regCfg.lastReg), v);
    else if (!strcmp(line, "truck")) cpy(g_regCfg.lastTruck, sizeof(g_regCfg.lastTruck), v);
  }
  return true;
}

bool regCfgSave() {
  char b[160];
  int n = snprintf(b, sizeof(b), "role=%d\nreg=%s\ntruck=%s\n", g_regCfg.role, g_regCfg.lastReg, g_regCfg.lastTruck);
  return n > 0 && plat::fsWrite(REG_CFG_PATH, b, (size_t)n);
}

// ── Réponse du serveur ───────────────────────────────────────────────
static RegCount cnt(JsonVariantConst v) {
  RegCount c = { 0, 0 };
  if (v.is<JsonObjectConst>()) { c.ok = clampN(v["ok"] | 0); c.hs = clampN(v["hs"] | 0); }
  return c;
}
static void str(char * d, size_t n, JsonVariantConst v) { cpy(d, n, v.is<const char *>() ? v.as<const char *>() : ""); }
static void sidOf(char * d, size_t n, JsonVariantConst v) {
  if (v.is<const char *>()) cpy(d, n, v.as<const char *>());
  else if (v.is<long>()) snprintf(d, n, "%ld", v.as<long>());
  else d[0] = 0;
}

bool regParseTour(const char * json, size_t len, RegSnap & o) {
  memset(&o, 0, sizeof(o));
  if (!json || !len) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if (!doc["trucks"].is<JsonArrayConst>()) return false;       // une réponse d'erreur n'a pas les listes
  o.serverNow = doc["now"] | (int64_t)0;
  str(o.device, sizeof(o.device), doc["device"]);
  for (JsonObjectConst t : doc["trucks"].as<JsonArrayConst>()) {
    if (o.nTrucks >= REG_MAX_TRUCKS) break;
    RegTruck & r = o.trucks[o.nTrucks];
    str(r.id, sizeof(r.id), t["id"]);
    str(r.name, sizeof(r.name), t["name"]);
    int c = t["capacity"] | 0;
    if (!r.id[0] || c < 1 || c > 99) continue;
    r.cap = (uint8_t)c;
    o.nTrucks++;
  }
  for (JsonObjectConst t : doc["regulators"].as<JsonArrayConst>()) {
    if (o.nRegs >= REG_MAX_PEOPLE) break;
    RegPerson & r = o.regs[o.nRegs];
    str(r.id, sizeof(r.id), t["id"]);
    str(r.name, sizeof(r.name), t["name"]);
    if (r.id[0]) o.nRegs++;
  }
  JsonObjectConst at = doc["atelier"];
  str(o.atelierName, sizeof(o.atelierName), at["name"]);
  if (!o.atelierName[0]) cpy(o.atelierName, sizeof(o.atelierName), "Atelier");
  o.atelierSet = at["lat"].is<float>() && at["lon"].is<float>();
  JsonObjectConst stock = doc["atelierStock"];
  o.atelierStock = (stock["at"] | (int64_t)0) > 0 ? clampN(stock["ok"] | 0) : -1;
  for (JsonObjectConst t : doc["openTours"].as<JsonArrayConst>()) {
    if (o.nOpen >= REG_MAX_OPEN) break;
    RegOpen & r = o.open[o.nOpen];
    str(r.id, sizeof(r.id), t["id"]);
    str(r.truckName, sizeof(r.truckName), t["truckName"]);
    str(r.regName, sizeof(r.regName), t["regulatorName"]);
    str(r.device, sizeof(r.device), t["device"]);
    r.startedAt = t["startedAt"] | (int64_t)0;
    if (r.id[0]) o.nOpen++;
  }
  JsonObjectConst tour = doc["tour"];
  if (!tour.isNull()) {
    o.hasTour = true;
    str(o.tourId, sizeof(o.tourId), tour["id"]);
    str(o.tourDevice, sizeof(o.tourDevice), tour["device"]);
    str(o.truckName, sizeof(o.truckName), tour["truckName"]);
    str(o.regName, sizeof(o.regName), tour["regulatorName"]);
    int c = tour["capacity"] | 0;
    o.cap = (uint8_t)(c < 0 ? 0 : c > 99 ? 99 : c);
    o.startedAt = tour["startedAt"] | (int64_t)0;
    o.load = cnt(doc["load"]);
    if (!o.tourId[0]) o.hasTour = false;
  }
  JsonObjectConst plan = doc["plan"];
  if (o.hasTour && !plan.isNull()) {
    o.hasPlan = true;
    o.planBenefit = clampN(plan["benefitMin"] | 0);
    o.planDuration = clampN(plan["durationMin"] | 0);
    for (JsonObjectConst s : plan["stops"].as<JsonArrayConst>()) {
      if (o.nStops >= REG_MAX_STOPS) break;
      RegStop & r = o.stops[o.nStops];
      r.seq = (uint8_t)(s["seq"] | (o.nStops + 1));
      r.kind = !strcmp(s["kind"] | "", "atelier") ? RK_ATELIER : RK_STATION;
      sidOf(r.sid, sizeof(r.sid), s["sid"]);
      str(r.name, sizeof(r.name), s["name"]);
      if (r.kind == RK_STATION && !r.sid[0]) continue;
      if (!r.name[0]) cpy(r.name, sizeof(r.name), r.kind == RK_ATELIER ? o.atelierName : r.sid);
      r.drop = cnt(s["drop"]);
      r.take = cnt(s["take"]);
      r.etaMin = s["etaMin"].is<int>() ? clampN(s["etaMin"].as<int>()) : -1;
      r.benefitMin = clampN(s["benefitMin"] | 0);
      r.arriveAt = s["arriveAt"] | (int64_t)0;
      str(r.why, sizeof(r.why), s["why"]);
      JsonObjectConst nw = s["now"];
      if (!nw.isNull()) {
        r.hasNow = true;
        r.nowBikes = clampN(nw["bikes"] | 0); r.nowDocks = clampN(nw["docks"] | 0);
        r.nowDis = clampN(nw["dis"] | 0); r.nowCap = clampN(nw["cap"] | 0);
      }
      r.loadAfter = cnt(s["loadAfter"]);
      o.nStops++;
    }
  }
  str(o.method, sizeof(o.method), doc["method"]);
  for (JsonObjectConst e : doc["events"].as<JsonArrayConst>()) {
    if (o.nEvents >= REG_MAX_EVENTS) break;
    RegEvent & r = o.events[o.nEvents];
    str(r.id, sizeof(r.id), e["id"]);
    r.kind = !strcmp(e["kind"] | "", "atelier") ? RK_ATELIER : RK_STATION;
    sidOf(r.sid, sizeof(r.sid), e["sid"]);
    str(r.name, sizeof(r.name), e["name"]);
    r.at = e["at"] | (int64_t)0;
    r.took = cnt(e["took"]);
    r.dropped = cnt(e["dropped"]);
    str(r.impossible, sizeof(r.impossible), e["impossible"]);
    JsonObjectConst ck = e["check"];
    if (!ck.isNull()) {
      const char * v = ck["verdict"] | "";
      r.verdict = !strcmp(v, "concordant") ? RV_CONCORDANT : !strcmp(v, "ecart") ? RV_ECART
                : !strcmp(v, "non-visible") ? RV_NONVISIBLE : !strcmp(v, "hors-flux") ? RV_HORSFLUX : RV_NONE;
      r.declOk = clampN(ck["declared"]["ok"] | 0);
      r.obsOk = clampN(ck["observed"]["ok"] | 0);
    }
    if (r.id[0]) o.nEvents++;
  }
  o.valid = true;
  return true;
}

// Empreinte de ce qui compte à l'écran : l'état n'est réécrit en flash que s'il a changé
static uint32_t fnv(uint32_t h, const void * p, size_t n) {
  const uint8_t * b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}
static uint32_t fnvs(uint32_t h, const char * s) { return fnv(h, s, strlen(s) + 1); }
static uint32_t snapSig(const RegSnap & s) {
  uint32_t h = 2166136261u;
  h = fnv(h, &s.hasTour, 1); h = fnvs(h, s.tourId); h = fnv(h, &s.load, sizeof(s.load));
  h = fnv(h, &s.nTrucks, 1); h = fnv(h, &s.nRegs, 1); h = fnv(h, &s.nOpen, 1); h = fnv(h, &s.atelierStock, 2);
  for (int i = 0; i < s.nTrucks; i++) { h = fnvs(h, s.trucks[i].id); h = fnv(h, &s.trucks[i].cap, 1); }
  for (int i = 0; i < s.nRegs; i++) h = fnvs(h, s.regs[i].id);
  for (int i = 0; i < s.nOpen; i++) h = fnvs(h, s.open[i].id);
  h = fnv(h, &s.nStops, 1);
  for (int i = 0; i < s.nStops; i++) {
    const RegStop & r = s.stops[i];
    h = fnv(h, &r.kind, 1); h = fnvs(h, r.sid); h = fnv(h, &r.drop, sizeof(r.drop)); h = fnv(h, &r.take, sizeof(r.take));
  }
  h = fnv(h, &s.nEvents, 1);
  for (int i = 0; i < s.nEvents; i++) { h = fnvs(h, s.events[i].id); h = fnv(h, &s.events[i].verdict, 1); }
  return h;
}

// ── État, file et persistance ────────────────────────────────────────
void regInit(RegState & st) { memset(&st, 0, sizeof(st)); }

struct QHdr {
  uint32_t magic;
  uint16_t ver, itemSize, rejSize;
  uint8_t  nq, nSent, nRej, pad;
  uint32_t counter;
  char     joinTour[48];
};
static const uint32_t Q_MAGIC = 0x51524E56;          // « VNRQ »
static const uint16_t Q_VER = 1;

bool regQueueSave(const RegState & st) {
  size_t n = sizeof(QHdr) + (st.nq + st.nSent) * sizeof(RegItem) + st.nRej * sizeof(RegReject);
  uint8_t * b = (uint8_t *)plat::psAlloc(n);
  if (!b) return false;
  QHdr h;
  memset(&h, 0, sizeof(h));
  h.magic = Q_MAGIC; h.ver = Q_VER; h.itemSize = sizeof(RegItem); h.rejSize = sizeof(RegReject);
  h.nq = st.nq; h.nSent = st.nSent; h.nRej = st.nRej; h.counter = st.counter;
  cpy(h.joinTour, sizeof(h.joinTour), st.joinTour);
  size_t o = 0;
  memcpy(b, &h, sizeof(h)); o += sizeof(h);
  memcpy(b + o, st.q, st.nq * sizeof(RegItem)); o += st.nq * sizeof(RegItem);
  memcpy(b + o, st.sent, st.nSent * sizeof(RegItem)); o += st.nSent * sizeof(RegItem);
  memcpy(b + o, st.rej, st.nRej * sizeof(RegReject)); o += st.nRej * sizeof(RegReject);
  bool ok = plat::fsWrite(REG_QUEUE_PATH, b, o);
  plat::psFree(b);
  return ok;
}

bool regQueueLoad(RegState & st) {
  long sz = plat::fsSize(REG_QUEUE_PATH);
  if (sz < (long)sizeof(QHdr) || sz > 64 * 1024) return false;
  uint8_t * b = (uint8_t *)plat::psAlloc((size_t)sz);
  if (!b) return false;
  bool ok = false;
  if (plat::fsRead(REG_QUEUE_PATH, b, (size_t)sz)) {
    QHdr h;
    memcpy(&h, b, sizeof(h));
    size_t want = sizeof(QHdr) + (h.nq + h.nSent) * sizeof(RegItem) + h.nRej * sizeof(RegReject);
    if (h.magic == Q_MAGIC && h.ver == Q_VER && h.itemSize == sizeof(RegItem) && h.rejSize == sizeof(RegReject) &&
        h.nq <= REG_MAX_QUEUE && h.nSent <= REG_MAX_SENT && h.nRej <= REG_MAX_REJ && want == (size_t)sz) {
      size_t o = sizeof(QHdr);
      st.nq = h.nq; memcpy(st.q, b + o, h.nq * sizeof(RegItem)); o += h.nq * sizeof(RegItem);
      st.nSent = h.nSent; memcpy(st.sent, b + o, h.nSent * sizeof(RegItem)); o += h.nSent * sizeof(RegItem);
      st.nRej = h.nRej; memcpy(st.rej, b + o, h.nRej * sizeof(RegReject));
      st.counter = h.counter;
      cpy(st.joinTour, sizeof(st.joinTour), h.joinTour);
      st.version++;
      ok = true;
    } else plat::logf("[regul] file en flash illisible (version ou taille) : ignor\xC3\xA9" "e");
  }
  plat::psFree(b);
  return ok;
}

// Dernier état reçu : « VNRS <reçu à>\n » puis le JSON du serveur, tel quel
bool regSnapLoad(RegState & st) {
  long sz = plat::fsSize(REG_SNAP_PATH);
  if (sz < 8 || sz > 48 * 1024) return false;
  char * b = (char *)plat::psAlloc((size_t)sz + 1);
  if (!b) return false;
  bool ok = false;
  if (plat::fsRead(REG_SNAP_PATH, b, (size_t)sz) && !strncmp(b, "VNRS ", 5)) {
    b[sz] = 0;
    char * nl = strchr(b, '\n');
    int64_t at = strtoll(b + 5, nullptr, 10);
    if (nl && at > 0) {
      static RegSnap tmp;                              // gros : hors pile
      if (regParseTour(nl + 1, (size_t)(b + sz - nl - 1), tmp)) {
        st.snap = tmp;
        st.snapAt = at;
        st.snapFromFlash = true;
        st.snapHash = snapSig(tmp);
        st.version++;
        ok = true;
      }
    }
  }
  plat::psFree(b);
  return ok;
}

// ── Vue locale ───────────────────────────────────────────────────────
// Ordre des gestes : d'abord ceux que le serveur a acceptés depuis l'état relu, puis ceux de la file
static const RegItem * pendingAt(const RegState & st, int i) {
  return i < st.nSent ? &st.sent[i] : (i - st.nSent < st.nq ? &st.q[i - st.nSent] : nullptr);
}
static int pendingCount(const RegState & st) { return st.nSent + st.nq; }

void regTourNow(const RegState & st, RegTourView & v) {
  memset(&v, 0, sizeof(v));
  if (st.snap.valid && st.snap.hasTour) {
    v.has = true;
    cpy(v.id, sizeof(v.id), st.snap.tourId);
    cpy(v.truckName, sizeof(v.truckName), st.snap.truckName);
    cpy(v.regName, sizeof(v.regName), st.snap.regName);
    v.cap = st.snap.cap;
    v.startedAt = st.snap.startedAt;
  }
  for (int i = 0, n = pendingCount(st); i < n; i++) {
    const RegItem & it = *pendingAt(st, i);
    if (it.op == RO_START) {
      v.has = true; v.local = true;
      cpy(v.id, sizeof(v.id), it.id);
      cpy(v.truckName, sizeof(v.truckName), it.truckName);
      cpy(v.regName, sizeof(v.regName), it.regName);
      v.cap = it.cap;
      v.startedAt = it.at;
    } else if (it.op == RO_END && v.has && !strcmp(it.tour, v.id)) v.has = false;
  }
  if (!v.has) memset(&v, 0, sizeof(v));
}

RegCount regLoadNow(const RegState & st) {
  RegTourView v;
  regTourNow(st, v);
  RegCount l = { 0, 0 };
  if (!v.has) return l;
  bool fromSnap = st.snap.valid && st.snap.hasTour && !strcmp(st.snap.tourId, v.id);
  if (fromSnap) l = st.snap.load;
  for (int i = 0, n = pendingCount(st); i < n; i++) {
    const RegItem & it = *pendingAt(st, i);
    if (it.op == RO_START && !strcmp(it.id, v.id)) l = it.took;
    else if (it.op == RO_EVENT && !it.impossible[0] && !strcmp(it.tour, v.id)) {
      l.ok = (int16_t)(l.ok + it.took.ok - it.dropped.ok);
      l.hs = (int16_t)(l.hs + it.took.hs - it.dropped.hs);
    }
  }
  return l;
}

int regPendingEvents(const RegState & st) {
  int n = 0;
  for (int i = 0; i < st.nq; i++) if (st.q[i].op == RO_EVENT) n++;
  return n;
}

void regStopKey(uint8_t kind, const char * sid, char * out, size_t n) {
  if (kind == RK_ATELIER) cpy(out, n, "atelier:");
  else snprintf(out, n, "station:%s", sid ? sid : "");
}

bool regStopDone(const RegState & st, const RegStop & s) {
  RegTourView v;
  regTourNow(st, v);
  if (!v.has) return false;
  for (int i = 0, n = pendingCount(st); i < n; i++) {
    const RegItem & it = *pendingAt(st, i);
    if (it.op != RO_EVENT || strcmp(it.tour, v.id) || it.kind != s.kind) continue;
    if (s.kind == RK_ATELIER || !strcmp(it.sid, s.sid)) return true;
  }
  return false;
}

int regStopsNow(const RegState & st, int * idx, int max) {
  RegTourView v;
  regTourNow(st, v);
  if (!v.has || !st.snap.valid || !st.snap.hasTour || strcmp(st.snap.tourId, v.id)) return 0;
  int k = 0;
  for (int i = 0; i < st.snap.nStops && k < max; i++) if (!regStopDone(st, st.snap.stops[i])) idx[k++] = i;
  return k;
}

// ── Gestes ───────────────────────────────────────────────────────────
static bool fail(char * why, size_t wn, const char * fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(char * why, size_t wn, const char * fmt, ...) {
  if (why && wn) { va_list ap; va_start(ap, fmt); vsnprintf(why, wn, fmt, ap); va_end(ap); }
  return false;
}
static const char * TIME_KO = "Heure inconnue : le Sunton attend le r\xC3\xA9seau pour se mettre \xC3\xA0 l'heure. En attendant, d\xC3\xA9" "clarer sur le t\xC3\xA9l\xC3\xA9phone.";

static void pushItem(RegState & st, const RegItem & it) {
  st.q[st.nq++] = it;
  st.version++;
  if (!regQueueSave(st)) plat::logf("[regul] file : \xC3\xA9" "criture en flash impossible (gardée en m\xC3\xA9moire)");
}

bool regDeclare(RegState & st, const RegDecl & d, int64_t now, const char * dev, char * why, size_t wn) {
  RegTourView v;
  regTourNow(st, v);
  if (!v.has) return fail(why, wn, "Aucune tourn\xC3\xA9" "e en cours : commencer une tourn\xC3\xA9" "e d'abord.");
  if (now < TIME_MIN) return fail(why, wn, "%s", TIME_KO);
  if (st.nq >= REG_MAX_QUEUE) return fail(why, wn, "File pleine (%d gestes en attente) : r\xC3\xA9tablir la connexion pour les envoyer.", st.nq);
  if (d.kind == RK_STATION && !d.sid[0]) return fail(why, wn, "Station manquante.");
  const bool imp = d.impossible && *d.impossible;
  RegCount took = imp ? RegCount{ 0, 0 } : d.took, dropped = imp ? RegCount{ 0, 0 } : d.dropped;
  if (took.ok < 0 || took.hs < 0 || dropped.ok < 0 || dropped.hs < 0 || took.ok > REG_MAX_N || took.hs > REG_MAX_N ||
      dropped.ok > REG_MAX_N || dropped.hs > REG_MAX_N)
    return fail(why, wn, "Nombre hors limites (0 \xC3\xA0 %d).", REG_MAX_N);
  if (!imp) {
    if (d.kind == RK_STATION && dropped.hs) return fail(why, wn, "On ne d\xC3\xA9pose pas de v\xC3\xA9lo hors service en station.");
    if (d.kind == RK_ATELIER && took.hs) return fail(why, wn, "On ne reprend pas de v\xC3\xA9lo hors service \xC3\xA0 l'atelier.");
    if (!took.ok && !took.hs && !dropped.ok && !dropped.hs)
      return fail(why, wn, "Rien de d\xC3\xA9" "clar\xC3\xA9 : r\xC3\xA9gler les nombres, ou \xC2\xAB Impossible \xC2\xBB.");
    RegCount l = regLoadNow(st);
    int ok2 = l.ok + took.ok - dropped.ok, hs2 = l.hs + took.hs - dropped.hs;
    if (ok2 < 0 || hs2 < 0)
      return fail(why, wn, "Il n'y a que %d v\xC3\xA9lo%s en service et %d HS \xC3\xA0 bord.", l.ok, l.ok > 1 ? "s" : "", l.hs);
    if (v.cap && ok2 + hs2 > v.cap)
      return fail(why, wn, "Le camion ne prend que %d v\xC3\xA9los (%d apr\xC3\xA8s cet arr\xC3\xAAt).", v.cap, ok2 + hs2);
  }
  RegItem it;
  memset(&it, 0, sizeof(it));
  it.op = RO_EVENT;
  it.kind = d.kind;
  st.counter++;
  it.seq = (int16_t)(st.counter & 0x7FFF);
  snprintf(it.id, sizeof(it.id), "%s-%llx-%lx", dev && *dev ? dev : "sunton", (unsigned long long)now, (unsigned long)st.counter);
  cpy(it.tour, sizeof(it.tour), v.id);
  if (d.kind == RK_STATION) cpy(it.sid, sizeof(it.sid), d.sid);
  cpy(it.name, sizeof(it.name), d.name[0] ? d.name : (d.kind == RK_ATELIER ? (st.snap.atelierName[0] ? st.snap.atelierName : "Atelier") : d.sid));
  it.at = now;
  it.took = took;
  it.dropped = dropped;
  if (imp) cpy(it.impossible, sizeof(it.impossible), d.impossible);
  it.plSeq = d.plSeq;
  it.plDrop = d.plDrop;
  it.plTake = d.plTake;
  pushItem(st, it);
  return true;
}

bool regStart(RegState & st, int truckIdx, int regIdx, RegCount load, int64_t now, const char * dev, char * why, size_t wn) {
  if (!st.snap.valid || !st.snap.nTrucks || !st.snap.nRegs)
    return fail(why, wn, "Camions et r\xC3\xA9gulateurs pas encore re\xC3\xA7us du serveur (\xC3\xA0 d\xC3\xA9" "clarer dans le tableau de bord, onglet R\xC3\xA9gulation).");
  RegTourView v;
  regTourNow(st, v);
  if (v.has) return fail(why, wn, "Une tourn\xC3\xA9" "e est d\xC3\xA9j\xC3\xA0 en cours : la terminer d'abord.");
  if (truckIdx < 0 || truckIdx >= st.snap.nTrucks) return fail(why, wn, "Choisir le camion du jour.");
  if (regIdx < 0 || regIdx >= st.snap.nRegs) return fail(why, wn, "Choisir le r\xC3\xA9gulateur.");
  if (now < TIME_MIN) return fail(why, wn, "%s", TIME_KO);
  if (st.nq >= REG_MAX_QUEUE) return fail(why, wn, "File pleine : r\xC3\xA9tablir la connexion.");
  const RegTruck & t = st.snap.trucks[truckIdx];
  const RegPerson & p = st.snap.regs[regIdx];
  if (load.ok < 0 || load.hs < 0) return fail(why, wn, "Chargement n\xC3\xA9gatif.");
  if (load.ok + load.hs > t.cap)
    return fail(why, wn, "Chargement de d\xC3\xA9part (%d) sup\xC3\xA9rieur \xC3\xA0 la capacit\xC3\xA9 du camion (%d).", load.ok + load.hs, t.cap);
  RegItem it;
  memset(&it, 0, sizeof(it));
  it.op = RO_START;
  st.counter++;
  snprintf(it.id, sizeof(it.id), "t-%s-%llx", dev && *dev ? dev : "sunton", (unsigned long long)now);
  cpy(it.truck, sizeof(it.truck), t.id);
  cpy(it.reg, sizeof(it.reg), p.id);
  it.at = now;
  it.took = load;
  it.cap = t.cap;
  it.plSeq = -1;
  cpy(it.truckName, sizeof(it.truckName), t.name);
  cpy(it.regName, sizeof(it.regName), p.name);
  st.joinTour[0] = 0;
  pushItem(st, it);
  cpy(g_regCfg.lastTruck, sizeof(g_regCfg.lastTruck), t.id);
  cpy(g_regCfg.lastReg, sizeof(g_regCfg.lastReg), p.id);
  regCfgSave();
  return true;
}

bool regEnd(RegState & st, RegCount load, int64_t now, char * why, size_t wn) {
  RegTourView v;
  regTourNow(st, v);
  if (!v.has) return fail(why, wn, "Aucune tourn\xC3\xA9" "e en cours.");
  if (now < TIME_MIN) return fail(why, wn, "%s", TIME_KO);
  if (st.nq >= REG_MAX_QUEUE) return fail(why, wn, "File pleine : r\xC3\xA9tablir la connexion.");
  if (load.ok < 0 || load.hs < 0 || load.ok > REG_MAX_N || load.hs > REG_MAX_N) return fail(why, wn, "Nombre hors limites.");
  RegItem it;
  memset(&it, 0, sizeof(it));
  it.op = RO_END;
  snprintf(it.id, sizeof(it.id), "end-%.43s", v.id);
  cpy(it.tour, sizeof(it.tour), v.id);
  it.at = now;
  it.took = load;
  it.plSeq = -1;
  st.joinTour[0] = 0;
  pushItem(st, it);
  return true;
}

void regJoin(RegState & st, const char * tourId) {
  cpy(st.joinTour, sizeof(st.joinTour), tourId);
  st.version++;
  regQueueSave(st);
}

static const char * const WHY[REG_N_WHY] = {
  "Station inaccessible (travaux, v\xC3\xA9hicule gar\xC3\xA9)", "Impossible de se garer", "Camion plein", "Bornes en panne",
  "Plus besoin (d\xC3\xA9j\xC3\xA0 \xC3\xA9quilibr\xC3\xA9" "e)", "Autre" };
const char * regWhy(int i) { return i >= 0 && i < REG_N_WHY ? WHY[i] : ""; }

// ── Textes ───────────────────────────────────────────────────────────
static void add(char * out, size_t n, const char * sep, const char * part) {
  size_t l = strlen(out);
  if (l + 1 >= n) return;
  snprintf(out + l, n - l, "%s%s", l ? sep : "", part);
}

void regActText(const RegStop & s, char * out, size_t n) {
  if (!n) return;
  out[0] = 0;
  char p[40];
  if (s.drop.ok) { snprintf(p, sizeof(p), "d\xC3\xA9poser %d", s.drop.ok); add(out, n, " \xC2\xB7 ", p); }
  if (s.drop.hs) { snprintf(p, sizeof(p), "d\xC3\xA9" "charger %d HS", s.drop.hs); add(out, n, " \xC2\xB7 ", p); }
  if (s.take.ok) {
    if (s.kind == RK_ATELIER) snprintf(p, sizeof(p), "reprendre %d neuf%s", s.take.ok, s.take.ok > 1 ? "s" : "");
    else snprintf(p, sizeof(p), "prendre %d", s.take.ok);
    add(out, n, " \xC2\xB7 ", p);
  }
  if (s.take.hs) { snprintf(p, sizeof(p), "ramasser %d HS", s.take.hs); add(out, n, " \xC2\xB7 ", p); }
  if (!out[0]) cpy(out, n, "\xE2\x80\x94");
}

void regDeclText(uint8_t kind, RegCount took, RegCount dropped, char * out, size_t n) {
  if (!n) return;
  out[0] = 0;
  char p[32];
  if (dropped.ok) { snprintf(p, sizeof(p), "d\xC3\xA9pos\xC3\xA9 %d", dropped.ok); add(out, n, ", ", p); }
  if (dropped.hs) { snprintf(p, sizeof(p), "d\xC3\xA9" "charg\xC3\xA9 %d HS", dropped.hs); add(out, n, ", ", p); }
  if (took.ok) { snprintf(p, sizeof(p), "%s %d", kind == RK_ATELIER ? "repris" : "pris", took.ok); add(out, n, ", ", p); }
  if (took.hs) { snprintf(p, sizeof(p), "pris %d HS", took.hs); add(out, n, ", ", p); }
  if (!out[0]) cpy(out, n, "rien");
}

static void sgn(char * out, size_t n, int v) {
  if (v > 0) snprintf(out, n, "+%d", v);
  else if (v < 0) snprintf(out, n, "\xE2\x88\x92%d", -v);   // vrai signe moins
  else snprintf(out, n, "0");
}

void regVerdictText(const RegEvent & e, char * out, size_t n) {
  char a[12], b[12];
  sgn(a, sizeof(a), e.declOk);
  sgn(b, sizeof(b), e.obsOk);
  switch (e.verdict) {
    case RV_CONCORDANT: snprintf(out, n, "concordant : d\xC3\xA9" "clar\xC3\xA9 %s, observ\xC3\xA9 %s", a, b); break;
    case RV_ECART: snprintf(out, n, "\xC3\xA9" "cart : d\xC3\xA9" "clar\xC3\xA9 %s, observ\xC3\xA9 %s", a, b); break;
    case RV_NONVISIBLE: snprintf(out, n, "non visible dans le flux"); break;
    case RV_HORSFLUX: snprintf(out, n, "%s", e.impossible[0] ? "arr\xC3\xAAt non fait" : "hors flux (atelier)"); break;
    default: snprintf(out, n, "recoupement 10 min apr\xC3\xA8s l'arr\xC3\xAAt"); break;
  }
}

static void hm(char * out, size_t n, int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void regStatusLine(const RegState & st, bool hasUrl, int64_t now, char * out, size_t n) {
  char a[8], b[8];
  if (!hasUrl) { snprintf(out, n, "Serveur en ligne non r\xC3\xA9gl\xC3\xA9 : R\xC3\xA9glages \xE2\x86\x92 Synchro en ligne"); return; }
  if (st.snapAt) hm(a, sizeof(a), st.snapAt);
  int pend = st.nq;
  if (pend) {
    snprintf(out, n, "%d geste%s en attente d'envoi, gard\xC3\xA9%s dans le Sunton%s%s", pend, pend > 1 ? "s" : "", pend > 1 ? "s" : "",
             st.lastErr[0] ? " \xC2\xB7 " : "", st.lastErr);
    return;
  }
  if (st.lastOk && now - st.lastOk < 120) { snprintf(out, n, "En ligne \xC2\xB7 \xC3\xA9tat re\xC3\xA7u \xC3\xA0 %s", a); return; }
  if (st.lastOk) {
    hm(b, sizeof(b), st.lastOk);
    snprintf(out, n, "Hors ligne depuis %s%s%s", b, st.lastErr[0] ? " \xC2\xB7 " : "", st.lastErr);
    return;
  }
  if (st.snapFromFlash) { snprintf(out, n, "Pas encore joint \xC2\xB7 \xC3\xA9tat gard\xC3\xA9 de %s%s%s", a, st.lastErr[0] ? " \xC2\xB7 " : "", st.lastErr); return; }
  snprintf(out, n, "%s", st.lastErr[0] ? st.lastErr : "Connexion au serveur\xE2\x80\xA6");
}

// ── Échange ──────────────────────────────────────────────────────────
const char * regItemPath(const RegItem & it) {
  return it.op == RO_START ? "/reg/tour/start" : it.op == RO_END ? "/reg/tour/end" : "/reg/event";
}

static void putCount(JsonObject o, RegCount c) { o["ok"] = c.ok; o["hs"] = c.hs; }

size_t regItemJson(const RegItem & it, char * out, size_t n) {
  JsonDocument doc;
  if (it.op == RO_START) {
    doc["id"] = it.id;
    doc["truck"] = it.truck;
    doc["regulator"] = it.reg;
    putCount(doc["load"].to<JsonObject>(), it.took);
    doc["at"] = it.at;
  } else if (it.op == RO_END) {
    doc["tour"] = it.tour;
    putCount(doc["load"].to<JsonObject>(), it.took);
    doc["at"] = it.at;
  } else {
    doc["id"] = it.id;
    doc["tour"] = it.tour;
    doc["kind"] = it.kind == RK_ATELIER ? "atelier" : "station";
    if (it.kind == RK_STATION) doc["sid"] = it.sid; else doc["sid"] = nullptr;
    doc["name"] = it.name;
    doc["at"] = it.at;
    doc["seq"] = it.seq;
    putCount(doc["took"].to<JsonObject>(), it.took);
    putCount(doc["dropped"].to<JsonObject>(), it.dropped);
    if (it.impossible[0]) doc["impossible"] = it.impossible; else doc["impossible"] = nullptr;
    if (it.plSeq >= 0) {
      JsonObject pl = doc["planned"].to<JsonObject>();
      pl["seq"] = it.plSeq;
      putCount(pl["take"].to<JsonObject>(), it.plTake);
      putCount(pl["drop"].to<JsonObject>(), it.plDrop);
    } else doc["planned"] = nullptr;
  }
  size_t w = serializeJson(doc, out, n);
  return w < n ? w : 0;
}

static void errText(int code, char * out, size_t n) {
  if (code == 401 || code == 403) snprintf(out, n, "jeton refus\xC3\xA9 : le saisir dans R\xC3\xA9glages \xE2\x86\x92 Synchro en ligne");
  else if (code == 404) snprintf(out, n, "le serveur ne conna\xC3\xAEt pas /reg (version trop ancienne ?)");
  else if (code >= 500) snprintf(out, n, "erreur du serveur (HTTP %d), la file est gard\xC3\xA9" "e", code);
  else if (code > 0) snprintf(out, n, "r\xC3\xA9ponse inattendue (HTTP %d)", code);
  else snprintf(out, n, "serveur injoignable (code %d)", code);
}
// Les refus du serveur (400, 409) sont définitifs : le geste sort de la file, avec la raison donnée.
// Tout le reste (réseau, jeton, serveur en panne, route absente) laisse la file intacte.
static bool definitiveRefusal(int code) { return code == 400 || code == 409 || code == 410 || code == 422; }

static void serverError(const char * resp, size_t len, int code, char * out, size_t n) {
  JsonDocument doc;
  if (resp && len && !deserializeJson(doc, resp, len) && doc["error"].is<const char *>()) cpy(out, n, doc["error"].as<const char *>());
  else snprintf(out, n, "refus\xC3\xA9 par le serveur (HTTP %d)", code);
}

static void itemLabel(const RegItem & it, char * out, size_t n) {
  if (it.op == RO_START) snprintf(out, n, "D\xC3\xA9" "but de tourn\xC3\xA9" "e");
  else if (it.op == RO_END) snprintf(out, n, "Fin de tourn\xC3\xA9" "e");
  else cpy(out, n, it.name[0] ? it.name : it.sid);
}

static void addReject(RegState & st, int64_t now, const char * what, const char * why) {
  if (st.nRej >= REG_MAX_REJ) st.nRej = REG_MAX_REJ - 1;
  memmove(&st.rej[1], &st.rej[0], st.nRej * sizeof(RegReject));
  st.rej[0].at = now;
  cpy(st.rej[0].what, sizeof(st.rej[0].what), what);
  cpy(st.rej[0].why, sizeof(st.rej[0].why), why);
  st.nRej++;
}

static RegSnap s_tmp;                                  // relu du serveur (gros : hors pile, une seule tâche réseau)

static int flushQueue(RegIo & io, const char * base, RegState & st, int64_t now, char * buf, size_t cap, bool & failed) {
  int done = 0;
  failed = false;
  char url[224], body[1024];
  for (int guard = 0; guard < REG_MAX_QUEUE + 1; guard++) {
    RegItem it;
    {
      plat::LockGuard g;
      if (!st.nq || st.nSent >= REG_MAX_SENT) break;
      it = st.q[0];
    }
    size_t bn = regItemJson(it, body, sizeof(body));
    if (!bn) {                                          // ne peut pas arriver (champs bornés) : on ne bloque pas la file
      plat::LockGuard g;
      if (st.nq && !strcmp(st.q[0].id, it.id)) { memmove(&st.q[0], &st.q[1], (st.nq - 1) * sizeof(RegItem)); st.nq--; }
      addReject(st, now, it.name, "geste illisible (trop long)");
      regQueueSave(st); st.version++;
      continue;
    }
    snprintf(url, sizeof(url), "%s%s", base, regItemPath(it));
    size_t rl = 0;
    int code = io.post(io.ctx, url, body, bn, buf, cap - 1, rl);
    if (rl >= cap) rl = cap - 1;
    buf[rl] = 0;
    plat::LockGuard g;
    st.lastTry = now;
    st.lastCode = (int16_t)code;
    if (code != 200 && !definitiveRefusal(code)) {
      errText(code, st.lastErr, sizeof(st.lastErr));
      st.version++;
      failed = true;
      break;
    }
    if (!st.nq || strcmp(st.q[0].id, it.id) || st.q[0].op != it.op) { failed = true; break; }   // file changée : impossible ici
    memmove(&st.q[0], &st.q[1], (st.nq - 1) * sizeof(RegItem));
    st.nq--;
    if (code == 200) {
      st.sent[st.nSent++] = it;
      st.lastOk = now;
      st.lastErr[0] = 0;
    } else {
      char why[120], what[48];
      serverError(buf, rl, code, why, sizeof(why));
      itemLabel(it, what, sizeof(what));
      if (it.op == RO_START) {
        // Début refusé : les gestes de cette tournée seraient refusés un à un (« tournée inconnue ») ; on le dit
        // une fois, avec leur nombre. Ils ne sont PAS enregistrés : à refaire sur le téléphone ou le tableau de bord.
        int k = 0, dropped = 0;
        for (int i = 0; i < st.nq; i++) {
          if (!strcmp(st.q[i].tour, it.id)) { dropped++; continue; }
          st.q[k++] = st.q[i];
        }
        st.nq = (uint8_t)k;
        if (dropped) {
          size_t l = strlen(why);
          snprintf(why + l, sizeof(why) - l, " \xE2\x80\x94 %d d\xC3\xA9" "claration%s non enregistr\xC3\xA9" "e%s", dropped, dropped > 1 ? "s" : "", dropped > 1 ? "s" : "");
        }
      }
      addReject(st, now, what, why);
      plat::logf("[regul] refus\xC3\xA9 par le serveur : %s \xE2\x80\x94 %s", what, why);
    }
    regQueueSave(st);
    st.version++;
    done++;
  }
  return done;
}

int regPoll(RegIo & io, const char * base, RegState & st, int64_t now, char * buf, size_t cap) {
  if (!base || !*base || !buf || cap < 4096) return -1;
  int total = 0;
  char url[224];
  for (int round = 0; round < 4; round++) {
    bool failed = false;
    total += flushQueue(io, base, st, now, buf, cap, failed);
    if (failed) return -1;
    char join[48];
    {
      plat::LockGuard g;
      cpy(join, sizeof(join), st.joinTour);
    }
    snprintf(url, sizeof(url), "%s/reg/tour%s%s", base, join[0] ? "?tour=" : "", join);
    size_t len = 0;
    const size_t PREFIX = 32;                           // place gardée pour l'en-tête du fichier
    int code = io.get(io.ctx, url, buf + PREFIX, cap - PREFIX - 1, len);
    if (code != 200 || !regParseTour(buf + PREFIX, len, s_tmp)) {
      plat::LockGuard g;
      st.lastTry = now;
      st.lastCode = (int16_t)(code == 200 ? -100 : code);
      if (code == 200) snprintf(st.lastErr, sizeof(st.lastErr), "r\xC3\xA9ponse du serveur illisible");
      else errText(code, st.lastErr, sizeof(st.lastErr));
      st.version++;
      return -1;
    }
    bool more;
    {
      plat::LockGuard g;
      st.snap = s_tmp;
      st.snapAt = now;
      st.snapFromFlash = false;
      bool hadSent = st.nSent > 0;
      st.nSent = 0;                                     // l'état relu les contient désormais
      st.lastTry = st.lastOk = now;
      st.lastCode = 200;
      st.lastErr[0] = 0;
      bool joinGone = st.joinTour[0] && !s_tmp.hasTour;
      if (joinGone) st.joinTour[0] = 0;
      if (hadSent || joinGone) regQueueSave(st);
      uint32_t h = snapSig(s_tmp);
      if (h != st.snapHash) {
        st.snapHash = h;
        char pre[PREFIX + 1];
        int pl = snprintf(pre, sizeof(pre), "VNRS %lld\n", (long long)now);
        if (pl > 0 && (size_t)pl <= PREFIX) {
          memcpy(buf + PREFIX - pl, pre, (size_t)pl);
          if (!plat::fsWrite(REG_SNAP_PATH, buf + PREFIX - pl, len + (size_t)pl)) plat::logf("[regul] \xC3\xA9tat : \xC3\xA9" "criture en flash impossible");
        }
      }
      st.version++;
      more = st.nq > 0;
    }
    if (!more) break;                                   // sinon : la liste des gestes acceptés était pleine, on continue
  }
  return total;
}

} // namespace vn
