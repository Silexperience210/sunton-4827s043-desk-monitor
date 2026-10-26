// ── Moteur VelohNav Ops en ligne : API C autour du cœur du firmware ──
// Le serveur (Node) et le jumeau d'écran (navigateur) chargent ce module WebAssembly.
// Il ne recalcule rien à sa façon : il appelle src/core tel quel, dans l'ordre exact de la
// tâche réseau de la carte (src/net/net_esp32.cpp, fetchCycle / fetchWeather / saveHistory).
//
//  - Ingestion : vn_ingest(flux, reçu, code, octets, durée) = un passage de fetchCycle pour ce flux
//    (analyse, application au modèle, compteurs d'ingestion), vn_cycle_end(ok) = la fin du cycle.
//  - Persistance : vn_save() écrit /vn_prof.bin, /vn_hist.bin, /vn_days.bin (Model::serialize, les
//    octets exacts de la carte) et /vn_live.bin (état « vivant » propre au moteur en ligne : stations,
//    tendance 1 min, journal des ruptures, météo, compteurs) ; vn_load() relit le tout.
//  - Réplication : un jumeau qui charge ces quatre fichiers puis rejoue les mêmes appels aux mêmes
//    instants calcule exactement ce que calcule le serveur (même code, même WebAssembly).
#include "../../src/config.h"
#include "../../src/core/model.h"
#include "../../src/core/settings.h"
#include "../../src/core/gbfs.h"
#include "../../src/core/weather.h"
#include "../../src/core/ops.h"
#include "../../src/core/days.h"
#include "../../src/core/service.h"
#include "../../src/core/dump.h"
#include "../../src/core/sync.h"
#include "../../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <map>
#include <vector>

#define EXPORT(name) extern "C" __attribute__((export_name(#name), used))

namespace eng {
extern int64_t g_now;
extern double g_ms;
extern std::map<std::string, std::vector<uint8_t>> g_fs;
extern std::string g_log;
// État de la tâche réseau que le firmware garde en variables statiques (net_esp32.cpp)
vn::FeedStat g_feed[vn::FEED_COUNT];
vn::ElecTypes g_elec;
int64_t g_typesAt = 0, g_infoAt = 0;
bool g_fetching = false;
}
using namespace vn;
using eng::g_feed;

static std::string s_out;                     // dernière sortie (JSON, fichier…)
static InfoRec * s_info = nullptr;
static StatusRec * s_stat = nullptr;

// ── Mémoire et sorties ───────────────────────────────────────────────
EXPORT(vn_alloc) void * vn_alloc(size_t n) { return malloc(n ? n : 1); }
EXPORT(vn_free) void vn_free(void * p) { free(p); }
EXPORT(vn_out_ptr) const char * vn_out_ptr() { return s_out.data(); }
EXPORT(vn_out_len) size_t vn_out_len() { return s_out.size(); }

EXPORT(vn_clock) void vn_clock(double epoch, double ms) { eng::g_now = (int64_t)epoch; eng::g_ms = ms; }
EXPORT(vn_version) const char * vn_version() { return VN_VERSION; }

EXPORT(vn_log_take) size_t vn_log_take() { s_out.swap(eng::g_log); eng::g_log.clear(); return s_out.size(); }

// ── Système de fichiers en mémoire ───────────────────────────────────
EXPORT(vn_fs_put) int vn_fs_put(const char * path, const uint8_t * d, size_t n) { return plat::fsWrite(path, d, n); }
EXPORT(vn_fs_get) int vn_fs_get(const char * path) {
  auto it = eng::g_fs.find(path);
  if (it == eng::g_fs.end()) { s_out.clear(); return -1; }
  s_out.assign((const char *)it->second.data(), it->second.size());
  return (int)s_out.size();
}
EXPORT(vn_fs_del) int vn_fs_del(const char * path) { return plat::fsRemove(path); }

// ── Initialisation ───────────────────────────────────────────────────
EXPORT(vn_init) int vn_init() {
  settingsDefaults(g_set);
  settingsLoad();                                   // /settings.bin déposé par l'hôte, sinon défauts
  syncCfgDefaults(g_sync);
  if (!g_model.init()) return 0;
  s_info = (InfoRec *)plat::psAlloc(sizeof(InfoRec) * VN_MAX_ST);
  s_stat = (StatusRec *)plat::psAlloc(sizeof(StatusRec) * VN_MAX_ST);
  memset(g_feed, 0, sizeof(g_feed));
  return s_info && s_stat ? 1 : 0;
}

// Seuils d'exploitation (Réglages de la carte) : même structure, même fichier /settings.bin
EXPORT(vn_settings_set) void vn_settings_set(int refreshSec, int lowBikes, int lowDocks, int maintBikes,
                                             int maintDocks, int frozenHours, int staleMin) {
  if (refreshSec >= 30) g_set.refreshSec = (uint16_t)refreshSec;
  if (lowBikes >= 0) g_set.lowBikes = (uint8_t)lowBikes;
  if (lowDocks >= 0) g_set.lowDocks = (uint8_t)lowDocks;
  if (maintBikes > 0) g_set.maintBikes = (uint8_t)maintBikes;
  if (maintDocks > 0) g_set.maintDocks = (uint8_t)maintDocks;
  if (frozenHours > 0) g_set.frozenHours = (uint8_t)frozenHours;
  if (staleMin >= 0) g_set.staleMin = (uint8_t)staleMin;
  if (g_model.lastUpdate) g_model.recompute(eng::g_now);
}

// ── Ingestion (fetchCycle / fetchWeather de net_esp32.cpp) ────────────
static void feedDone(int f, bool ok, int code, size_t bytes, uint32_t ms) {
  if (ok) ingestOk(g_feed[f], plat::now(), (uint32_t)bytes, ms);
  else ingestErr(g_feed[f], plat::now(), code, (uint32_t)bytes, ms);
  if (!ok) plat::logf("[net] %s : \xC3\xA9" "chec %d (%s), %u octets, %u ms", feedName(f), code,
                      ingestErrText(code), (unsigned)bytes, (unsigned)ms);
}

// got : la requête HTTP a rendu un corps (200 et au moins un octet), sinon code = dernier statut
// (code HTTP, HTTPC_ERROR_*, ou ING_E_*). Renvoie le nombre d'enregistrements appliqués (‑1 = échec).
EXPORT(vn_ingest) int vn_ingest(int feed, int got, int code, const char * buf, size_t len, uint32_t ms, uint32_t cycleMs) {
  const int64_t now = plat::now();
  switch (feed) {
    case FEED_TYPES: {
      bool parsed = got && gbfsParseTypes(buf, len, eng::g_elec);
      if (parsed) eng::g_typesAt = now;
      feedDone(FEED_TYPES, parsed, got ? ING_E_PARSE : code, len, ms);
      return parsed ? eng::g_elec.n : -1;
    }
    case FEED_INFO: {
      int n = got ? gbfsParseInfo(buf, len, s_info, VN_MAX_ST) : -1;
      if (n > 0) { g_model.applyInfo(s_info, n, now); eng::g_infoAt = now; }
      feedDone(FEED_INFO, n > 0, !got ? code : n == 0 ? ING_E_EMPTY : ING_E_PARSE, len, ms);
      return n > 0 ? n : -1;
    }
    case FEED_STATUS: {
      int64_t lastUpd = 0;
      int n = -1, bad = 0;
      if (got) n = gbfsParseStatus(buf, len, s_stat, VN_MAX_ST, eng::g_elec, &lastUpd, &bad);
      feedDone(FEED_STATUS, n > 0, !got ? code : n == 0 ? ING_E_EMPTY : ING_E_PARSE, len, ms);
      if (n > 0) {
        g_model.elec = eng::g_elec;
        g_model.applyStatus(s_stat, n, plat::now());
        g_model.feedUpdated = lastUpd;
        g_model.fetchErrors = 0;
        g_model.fetchMs = cycleMs;
        plat::logf("[net] relev\xC3\xA9 ok : %d stations, %d v\xC3\xA9los dispo, %u octets, %u ms", n, g_model.tot.bikes,
                   (unsigned)len, (unsigned)ms);
      }
      return n > 0 ? n : -1;
    }
    case FEED_WEATHER: {
      if (!got) { feedDone(FEED_WEATHER, false, code, len, ms); return -1; }
      Weather w;
      bool parsed = wxParse(buf, len, w);
      feedDone(FEED_WEATHER, parsed, ING_E_PARSE, len, ms);
      if (!parsed) return -1;
      w.fetchedAt = plat::now();
      g_model.wx = w;
      g_model.recompute(plat::now());
      return w.nfc;
    }
  }
  return -1;
}
// Fin d'un cycle station_status : « if (!ok) { fetchErrors++; version++; } »
EXPORT(vn_cycle_end) void vn_cycle_end(int ok) {
  if (!ok) { g_model.fetchErrors++; g_model.version++; }
}
EXPORT(vn_set_fetching) void vn_set_fetching(int on) { eng::g_fetching = on != 0; }
// Le flux d'information doit être lu avant le premier statut, comme sur la carte (s_infoAt)
EXPORT(vn_info_age) double vn_info_age() { return eng::g_infoAt ? (double)(plat::now() - eng::g_infoAt) : -1; }
EXPORT(vn_types_age) double vn_types_age() { return eng::g_typesAt ? (double)(plat::now() - eng::g_typesAt) : -1; }
EXPORT(vn_recompute) void vn_recompute() { if (g_model.lastUpdate) g_model.recompute(plat::now()); }

// ── État « vivant » (/vn_live.bin) ───────────────────────────────────
// Ce que les trois fichiers de la carte ne contiennent pas et qu'un jumeau doit reprendre pour
// calculer à l'identique : stations (relevé, suivi, dérivés), anneau 1 min de la tendance, journal
// des ruptures, météo, types électriques, dispersion en attente, compteurs d'ingestion. Format
// propre au moteur en ligne (le firmware ne le lit pas) : il n'est échangé qu'entre deux copies du
// MÊME module, d'où les tailles de structure dans l'en-tête — un module différent le refuse.
static const uint32_t LIVE_MAGIC = 0x314C4E56;   // "VNL1"
static const uint16_t LIVE_VER = 1;
struct LiveHdr {
  uint32_t magic;
  uint16_t ver, nSt;
  uint16_t szStation, szWeather, szElec, szFeed, szLog;
  uint16_t pad;
  int64_t  savedAt;
  int64_t  lastUpdate, feedUpdated, typesAt, infoAt;
  int32_t  fetchErrors, rHead, rCount, nLog, logHead;
  uint32_t fetchMs, version;
};

// Copies sans octets de remplissage : une structure copiée depuis la pile emporte ce qui traînait
// entre ses champs (des adresses, qui diffèrent d'un module à l'autre). Le fichier ne doit contenir
// que les champs, pour que deux copies du calcul donnent les mêmes octets.
static Weather cleanWx(const Weather & w) {
  Weather c;
  memset((void *)&c, 0, sizeof(c));
  c.valid = w.valid; c.fetchedAt = w.fetchedAt; c.temp = w.temp; c.rain = w.rain; c.wind = w.wind;
  c.windDir = w.windDir; c.code = w.code; c.nfc = w.nfc; c.score = w.score;
  for (int k = 0; k < 6; k++) {
    c.fc[k].h = w.fc[k].h; c.fc[k].temp = w.fc[k].temp; c.fc[k].rain = w.fc[k].rain; c.fc[k].wind = w.fc[k].wind;
    c.fc[k].prob = w.fc[k].prob; c.fc[k].code = w.fc[k].code;
  }
  strncpy(c.reason, w.reason, sizeof(c.reason) - 1);   // complète par des zéros
  return c;
}
static AlertLog cleanLog(const AlertLog & e) {
  AlertLog c;
  memset((void *)&c, 0, sizeof(c));
  c.t = e.t; c.dur = e.dur; c.st = e.st; c.type = e.type;
  return c;
}

static void liveSerialize(std::string & o) {
  LiveHdr h{};
  h.magic = LIVE_MAGIC; h.ver = LIVE_VER; h.nSt = (uint16_t)g_model.n;
  h.szStation = sizeof(Station); h.szWeather = sizeof(Weather); h.szElec = sizeof(ElecTypes);
  h.szFeed = sizeof(FeedStat); h.szLog = sizeof(AlertLog);
  h.savedAt = plat::now();
  h.lastUpdate = g_model.lastUpdate; h.feedUpdated = g_model.feedUpdated;
  h.typesAt = eng::g_typesAt; h.infoAt = eng::g_infoAt;
  h.fetchErrors = g_model.fetchErrors; h.rHead = g_model.rHead; h.rCount = g_model.rCount;
  h.nLog = g_model.nLog; h.logHead = g_model.logHead;
  h.fetchMs = g_model.fetchMs; h.version = g_model.version;
  const int n = g_model.n;
  o.clear();
  o.append((const char *)&h, sizeof(h));
  o.append((const char *)g_model.st, sizeof(Station) * n);
  o.append((const char *)g_model.recT, sizeof(int64_t) * VN_RECENT_SLOTS);
  for (int r = 0; r < VN_RECENT_SLOTS; r++) o.append((const char *)g_model.rec + (size_t)r * VN_MAX_ST, n);
  for (int k = 0; k < VN_ALERT_LOG; k++) { AlertLog c = cleanLog(g_model.log[k]); o.append((const char *)&c, sizeof(c)); }
  { Weather c = cleanWx(g_model.wx); o.append((const char *)&c, sizeof(c)); }
  o.append((const char *)&eng::g_elec, sizeof(ElecTypes));
  o.append((const char *)g_model.dispPendDev, sizeof(float) * n);
  o.append((const char *)g_model.dispPendCell, sizeof(int16_t) * n);
  o.append((const char *)g_feed, sizeof(g_feed));
}

static bool liveLoad(const std::vector<uint8_t> & b) {
  if (b.size() < sizeof(LiveHdr)) return false;
  LiveHdr h; memcpy(&h, b.data(), sizeof(h));
  if (h.magic != LIVE_MAGIC || h.ver != LIVE_VER || h.szStation != sizeof(Station) || h.szWeather != sizeof(Weather)
      || h.szElec != sizeof(ElecTypes) || h.szFeed != sizeof(FeedStat) || h.szLog != sizeof(AlertLog) || h.nSt > VN_MAX_ST)
    return false;
  const int n = h.nSt;
  size_t need = sizeof(h) + sizeof(Station) * n + sizeof(int64_t) * VN_RECENT_SLOTS + (size_t)VN_RECENT_SLOTS * n
                + sizeof(AlertLog) * VN_ALERT_LOG + sizeof(Weather) + sizeof(ElecTypes) + sizeof(float) * n
                + sizeof(int16_t) * n + sizeof(g_feed);
  if (b.size() != need) return false;
  const uint8_t * p = b.data() + sizeof(h);
  // Les stations du fichier, retrouvées par sid (créées si absentes) ; map[j] = index dans le modèle
  std::vector<int16_t> map(n, -1);
  const uint8_t * stp = p;
  for (int j = 0; j < n; j++) {
    Station s; memcpy(&s, stp + sizeof(Station) * j, sizeof(Station));
    s.sid[sizeof(s.sid) - 1] = 0;
    int i = g_model.find(s.sid);
    if (i < 0 && g_model.n < VN_MAX_ST && s.sid[0]) i = g_model.n++;
    if (i < 0) continue;
    g_model.st[i] = s;
    map[j] = (int16_t)i;
  }
  p += sizeof(Station) * n;
  memcpy(g_model.recT, p, sizeof(int64_t) * VN_RECENT_SLOTS); p += sizeof(int64_t) * VN_RECENT_SLOTS;
  memset(g_model.rec, 255, (size_t)VN_RECENT_SLOTS * VN_MAX_ST);
  for (int r = 0; r < VN_RECENT_SLOTS; r++) {
    for (int j = 0; j < n; j++) if (map[j] >= 0) g_model.rec[(size_t)r * VN_MAX_ST + map[j]] = p[j];
    p += n;
  }
  memcpy(g_model.log, p, sizeof(AlertLog) * VN_ALERT_LOG); p += sizeof(AlertLog) * VN_ALERT_LOG;
  for (int k = 0; k < VN_ALERT_LOG; k++) {
    int16_t s = g_model.log[k].st;
    if (s >= 0 && s < n) g_model.log[k].st = map[s];
  }
  memcpy((void *)&g_model.wx, p, sizeof(Weather)); p += sizeof(Weather);
  memcpy(&eng::g_elec, p, sizeof(ElecTypes)); p += sizeof(ElecTypes);
  g_model.elec = eng::g_elec;
  for (int j = 0; j < n; j++) if (map[j] >= 0) memcpy(&g_model.dispPendDev[map[j]], p + sizeof(float) * j, sizeof(float));
  p += sizeof(float) * n;
  for (int j = 0; j < n; j++) if (map[j] >= 0) memcpy(&g_model.dispPendCell[map[j]], p + sizeof(int16_t) * j, sizeof(int16_t));
  p += sizeof(int16_t) * n;
  memcpy(g_feed, p, sizeof(g_feed));
  g_model.lastUpdate = h.lastUpdate; g_model.feedUpdated = h.feedUpdated;
  eng::g_typesAt = h.typesAt; eng::g_infoAt = h.infoAt;
  g_model.fetchErrors = h.fetchErrors; g_model.rHead = h.rHead; g_model.rCount = h.rCount;
  g_model.nLog = h.nLog; g_model.logHead = h.logHead;
  g_model.fetchMs = h.fetchMs; g_model.version = h.version;
  return true;
}

// ── Sauvegarde / relecture ───────────────────────────────────────────
// Les trois fichiers de la carte passent par le code de la carte : Model::serialize puis writeSaved
// (ici, en mémoire). Renvoie 0 si le modèle est vide (aucune station : rien à écrire, comme la carte).
EXPORT(vn_save) int vn_save() {
  Model::Blob b;
  if (!g_model.serialize(b)) return 0;
  g_model.dirty = false;
  bool ok = Model::writeSaved(b);
  std::string live;
  liveSerialize(live);
  plat::fsWrite("/vn_live.bin", live.data(), live.size());
  return ok ? 1 : 0;
}

// Relit ce qui a été déposé (vn_fs_put) : Model::load (mêmes contrôles et migrations que la carte),
// puis l'état vivant s'il vient du même module. 1 = quelque chose a été relu.
EXPORT(vn_load) int vn_load(int withLive) {
  bool any = g_model.load(plat::now());
  bool live = false;
  if (withLive) {
    auto it = eng::g_fs.find("/vn_live.bin");
    if (it != eng::g_fs.end()) live = liveLoad(it->second);
  }
  if (live && g_model.lastUpdate) {
    // Les dérivés (totaux, prévisions réseau, alertes, transferts) ne sont pas dans le fichier : on les
    // recalcule à l'instant du DERNIER recalcul du serveur (dernier relevé ou dernière météo), pas à
    // l'heure du chargement, pour retrouver exactement son état ; le compteur de version est rendu tel quel.
    const uint32_t ver = g_model.version;
    int64_t t = g_model.lastUpdate;
    if (g_model.wx.valid && g_model.wx.fetchedAt > t) t = g_model.wx.fetchedAt;
    g_model.recompute(t);
    g_model.version = ver;
  } else if (g_model.lastUpdate) g_model.recompute(plat::now());
  return (any ? 1 : 0) | (live ? 2 : 0);
}

// Liste des stations (même texte que le vidage série : /vn_stations.csv)
EXPORT(vn_stations_csv) size_t vn_stations_csv() {
  size_t cap = 64 + (size_t)g_model.n * 160;
  s_out.assign(cap, 0);
  size_t n = stationsCsv(g_model, g_model.lastUpdate, &s_out[0], cap);
  s_out.resize(n);
  return n;
}

EXPORT(vn_crc32) uint32_t vn_crc32(const uint8_t * d, size_t n) { return crc32(d, n); }

// ── Résumé JSON (API du serveur) ─────────────────────────────────────
struct J {
  std::string & s;
  bool first = true;
  explicit J(std::string & o) : s(o) {}
  void sep() { if (!first) s += ','; first = false; }
  void key(const char * k) { sep(); s += '"'; s += k; s += "\":"; }
  void raw(const char * k, const char * v) { key(k); s += v; }
  void num(const char * k, double v) {
    key(k);
    char b[40];
    if (!isfinite(v)) { s += "null"; return; }
    if (v == (double)(long long)v && fabs(v) < 9e15) snprintf(b, sizeof(b), "%lld", (long long)v);
    else snprintf(b, sizeof(b), "%.4g", v);
    s += b;
  }
  void boolean(const char * k, bool v) { key(k); s += v ? "true" : "false"; }
  static void esc(std::string & s, const char * v) {
    s += '"';
    for (const unsigned char * p = (const unsigned char *)v; *p; p++) {
      if (*p == '"' || *p == '\\') { s += '\\'; s += (char)*p; }
      else if (*p < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", *p); s += b; }
      else s += (char)*p;
    }
    s += '"';
  }
  void str(const char * k, const char * v) { key(k); esc(s, v ? v : ""); }
  void open(const char * k, char c) { if (k) key(k); else sep(); s += c; first = true; }
  void close(char c) { s += c; first = false; }
};

EXPORT(vn_summary_json) size_t vn_summary_json() {
  s_out.clear();
  J j(s_out);
  const Totals & t = g_model.tot;
  const int64_t now = plat::now();
  j.open(nullptr, '{');
  j.str("fw", VN_VERSION);
  j.num("now", (double)now);
  j.num("lastUpdate", (double)g_model.lastUpdate);
  j.num("feedUpdated", (double)g_model.feedUpdated);
  j.num("fetchErrors", g_model.fetchErrors);
  j.boolean("feedFrozen", g_model.alertCount[AL_FEED_FROZEN] > 0);
  j.num("version", g_model.version);
  j.num("stations", g_model.n);
  j.num("learnSince", (double)g_model.learnSince);
  j.num("learnSamples", g_model.learnSamples);
  j.num("learnPct", g_model.learnPct());
  j.num("netConf", g_model.netConf);
  j.num("histMinutes", t.histMinutes);
  j.num("daysKept", g_model.days.count);
  j.num("wxAlpha", g_model.wxAlpha);
  j.num("wxSrc", g_model.wxSrc);
  char b[160];
  wxOriginText(g_model, b, sizeof(b));
  j.str("wxOrigin", b);
  j.open("totals", '{');
  j.num("open", t.open); j.num("closed", t.closed);
  j.num("bikes", t.bikes); j.num("elec", t.elec); j.num("docks", t.docks);
  j.num("bikesDis", t.bikesDis); j.num("docksDis", t.docksDis);
  j.num("fillPct", t.fillPct); j.num("balancePct", t.balancePct);
  j.boolean("inUseValid", t.inUseValid); j.num("inUse", t.inUse); j.num("fleet", t.fleet);
  j.num("need", t.need); j.num("surplus", t.surplus);
  j.num("empty", t.byStatus[ST_EMPTY]); j.num("full", t.byStatus[ST_FULL]);
  j.num("fc60", g_model.netFc[12]);
  j.close('}');
  j.num("alerts", g_model.nAlertsTotal);
  j.num("ruptures", g_model.alertCount[AL_EMPTY] + g_model.alertCount[AL_FULL]);
  j.open("weather", '{');
  j.boolean("valid", g_model.wx.valid);
  j.num("fetchedAt", (double)g_model.wx.fetchedAt);
  j.num("temp", g_model.wx.temp); j.num("code", g_model.wx.code); j.num("score", g_model.wx.score);
  j.str("label", wxLabel(g_model.wx.code)); j.str("reason", g_model.wx.reason);
  j.close('}');
  j.open("feeds", '[');
  for (int f = 0; f < FEED_COUNT; f++) {
    const FeedStat & fs = g_feed[f];
    j.open(nullptr, '{');
    j.str("name", feedName(f));
    j.num("okAt", (double)fs.okAt); j.num("errAt", (double)fs.errAt); j.num("errCode", fs.errCode);
    j.str("errText", fs.errAt ? ingestErrText(fs.errCode) : "");
    j.num("fails", fs.fails); j.num("bytes", fs.bytes); j.num("ms", fs.ms);
    j.num("okCount", fs.okCount); j.num("errCount", fs.errCount);
    j.close('}');
  }
  j.close(']');
  j.num("staleS", staleThresholdS(g_set));
  j.close('}');
  return s_out.size();
}

// Tranches 5 min de l'anneau (pour l'API : courbe réseau 24 h)
EXPORT(vn_ring_json) size_t vn_ring_json() {
  // [[t, vélos dispo, bornes libres, vélos HS, retraits, retours], …] du plus ancien au plus récent
  s_out = "[";
  bool first = true;
  for (int b = g_model.count - 1; b >= 0; b--) {
    int idx = g_model.ringIndex(b);
    if (idx < 0) continue;
    const NetSample & ns = g_model.net[idx];
    char b2[160];
    if (ns.avail == 0xFFFF) snprintf(b2, sizeof(b2), "%s[%lld,null,null,null,null,null]", first ? "" : ",", (long long)ns.t);
    else snprintf(b2, sizeof(b2), "%s[%lld,%u,%u,%u,%u,%u]", first ? "" : ",", (long long)ns.t, ns.avail, ns.docks, ns.dis, ns.dep, ns.ret);
    s_out += b2;
    first = false;
  }
  s_out += "]";
  return s_out.size();
}

// ════════════════════════════════════════════════════════════════════
// Exports pour l'historique long, la mesure des prévisions, les notifications
// ════════════════════════════════════════════════════════════════════
static void numOrNull(std::string & s, double v, const char * fmt = "%.4g") {
  if (!isfinite(v)) { s += "null"; return; }
  char b[32];
  if (v == (double)(long long)v && fabs(v) < 9e15) snprintf(b, sizeof(b), "%lld", (long long)v);
  else snprintf(b, sizeof(b), fmt, v);
  s += b;
}

// Instant de la tranche 5 min la plus récente de l'anneau (0 = aucune) : l'hôte détecte ainsi qu'un
// relevé vient d'ouvrir une tranche (la précédente est alors définitive : retraits et retours compris).
EXPORT(vn_ring_head) double vn_ring_head() {
  int idx = g_model.ringIndex(0);
  return idx < 0 ? 0 : (double)g_model.ringT[idx];
}

// Une tranche de l'anneau : réseau (NetSample), météo de la tranche, vélos de chaque station dans
// l'ordre du modèle (la valeur de l'anneau : celle que Model::load relira et que le banc rejoue).
EXPORT(vn_slot_json) size_t vn_slot_json(int back) {
  s_out.clear();
  int idx = g_model.ringIndex(back);
  if (idx < 0) { s_out = "null"; return s_out.size(); }
  const NetSample & ns = g_model.net[idx];
  char b[96];
  snprintf(b, sizeof(b), "{\"t\":%lld,\"n\":", (long long)g_model.ringT[idx]);
  s_out += b;
  if (ns.avail == 0xFFFF) s_out += "null";
  else {
    snprintf(b, sizeof(b), "[%u,%u,%u,%u,%u,%u,%u,%u]", ns.avail, ns.docks, ns.dis, ns.bikesRaw, ns.dep, ns.ret, ns.empty, ns.full);
    s_out += b;
  }
  s_out += ",\"w\":";
  if (!netWxKnown(ns)) s_out += "null";
  else {
    s_out += "[";
    numOrNull(s_out, ns.wxTemp); s_out += ",";
    numOrNull(s_out, ns.wxRain / 10.0, "%.1f"); s_out += ",";
    numOrNull(s_out, ns.wxProb == WX_NA ? NAN : ns.wxProb); s_out += ",";
    numOrNull(s_out, ns.wxWind == WX_NA ? NAN : ns.wxWind); s_out += ",";
    numOrNull(s_out, ns.wxCode == WX_NA ? NAN : ns.wxCode);
    s_out += "]";
  }
  s_out += ",\"b\":[";
  const uint8_t * row = g_model.ring + (size_t)idx * VN_MAX_ST;
  for (int i = 0; i < g_model.n; i++) {
    if (i) s_out += ",";
    if (row[i] == 255) s_out += "null"; else { snprintf(b, sizeof(b), "%u", row[i]); s_out += b; }
  }
  s_out += "]}";
  return s_out.size();
}

// Stations dans l'ordre du modèle : identité, relevé courant, dérivés (statut, échéances, besoin, risque)
EXPORT(vn_stations_json) size_t vn_stations_json() {
  s_out.clear();
  J j(s_out);
  j.open(nullptr, '[');
  for (int i = 0; i < g_model.n; i++) {
    const Station & s = g_model.st[i];
    j.open(nullptr, '{');
    j.str("sid", s.sid); j.str("name", s.name); j.num("cap", s.cap);
    j.num("lat", s.lat); j.num("lon", s.lon);
    j.boolean("active", s.active); j.boolean("live", s.live);
    j.num("bikes", s.bikes); j.num("elec", s.elec); j.num("docks", s.docks);
    j.num("bikesDis", s.bikesDis); j.num("docksDis", s.docksDis);
    j.boolean("renting", s.renting); j.boolean("returning", s.returning);
    j.num("status", s.status); j.str("statusText", statusShort(s.status));
    j.num("etaEmpty", s.etaEmpty); j.num("etaFull", s.etaFull);
    j.num("need", s.need); j.num("risk", s.risk); j.num("conf", s.conf);
    j.num("fc60", s.fc[12]);
    j.num("emptySince", (double)s.emptySince); j.num("fullSince", (double)s.fullSince);
    j.close('}');
  }
  j.close(']');
  return s_out.size();
}

// Prévisions de chaque station, comme sim/bench_reel.cpp les note : modèle à +15/+30/+60/+120/+240 min
// (st.fc, prolongée par Model::project au-delà de 2 h), persistance (le relevé), profil seul (relevé +
// confiance × mouvement du profil, sans tendance). Valeurs ARRONDIES comme le banc avant de les comparer
// (roundf) : [relevé, modèle ×5, profil seul ×5, échéance vide, échéance pleine, capacité].
// null = station que le banc n'évalue pas (inconnue, fermée, sans valeur dans la tranche courante).
static const int FC_HK[5] = { 3, 6, 12, 24, 48 };
EXPORT(vn_forecast_json) size_t vn_forecast_json() {
  s_out.clear();
  const int64_t t = plat::now();
  const int head = g_model.ringIndex(0);
  char b[64];
  snprintf(b, sizeof(b), "{\"t\":%lld,\"slot\":%lld,\"net60\":", (long long)t, head < 0 ? 0LL : (long long)g_model.ringT[head]);
  s_out += b;
  numOrNull(s_out, roundf(g_model.netFc[12]));
  s_out += ",\"bikes\":";
  numOrNull(s_out, g_model.tot.bikes);
  s_out += ",\"st\":[";
  static float tmp[49], cf[49];
  const uint8_t * row = head < 0 ? nullptr : g_model.ring + (size_t)head * VN_MAX_ST;
  for (int i = 0; i < g_model.n; i++) {
    if (i) s_out += ",";
    const Station & st = g_model.st[i];
    if (!row || row[i] == 255 || !st.live || st.status == ST_CLOSED) { s_out += "null"; continue; }
    g_model.project(i, t, 5, 49, tmp, cf);
    float p0, n0, ph, nh;
    const bool has0 = g_model.profileAt(i, t, p0, n0);
    int v[16], k = 0;
    v[k++] = st.bikes;
    for (int h = 0; h < 5; h++) v[k++] = (int)roundf(FC_HK[h] < VN_FC_STEPS ? st.fc[FC_HK[h]] : tmp[FC_HK[h]]);
    for (int h = 0; h < 5; h++) {
      float pr = st.bikes;
      if (has0 && g_model.profileAt(i, t + (int64_t)FC_HK[h] * 300, ph, nh))
        pr = fminf((float)st.capEff(), fmaxf(0.f, st.bikes + cf[FC_HK[h]] * (ph - p0)));
      v[k++] = (int)roundf(pr);
    }
    v[k++] = st.etaEmpty; v[k++] = st.etaFull; v[k++] = st.capEff();
    s_out += "[";
    for (int q = 0; q < k; q++) { if (q) s_out += ","; snprintf(b, sizeof(b), "%d", v[q]); s_out += b; }
    s_out += "]";
  }
  s_out += "]}";
  return s_out.size();
}

// Alertes en cours (liste de la carte, déjà triée par gravité puis risque) avec la station
EXPORT(vn_alerts_json) size_t vn_alerts_json() {
  s_out.clear();
  J j(s_out);
  j.open(nullptr, '[');
  for (int k = 0; k < g_model.nAlerts; k++) {
    const Alert & a = g_model.alerts[k];
    j.open(nullptr, '{');
    j.num("type", a.type); j.num("sev", a.sev); j.num("st", a.st);
    j.num("minutes", a.minutes); j.num("value", a.value);
    if (a.st >= 0 && a.st < g_model.n) {
      const Station & s = g_model.st[a.st];
      j.str("sid", s.sid); j.str("name", s.name);
      j.num("bikes", s.bikes); j.num("docks", s.docks); j.num("cap", s.capEff()); j.num("need", s.need);
      j.num("fc60", s.fc[12]);
      if (a.type == AL_UNUSUAL) { char t[128]; anomAlertText(s, t, sizeof(t)); j.str("text", t); }
    }
    j.close('}');
  }
  j.close(']');
  return s_out.size();
}

// Résumés par jour (vue Service) : un objet par jour stocké, du plus ancien au plus récent, sommés sur les
// heures de service comme svcSum (jour « relevé » : au moins une heure pleinement relevée).
EXPORT(vn_days_json) size_t vn_days_json() {
  s_out.clear();
  J j(s_out);
  const DayBook & b = g_model.days;
  j.open(nullptr, '[');
  for (int k = b.count - 1; k >= 0; k--) {
    const DayNet * d = b.dayAt(k);
    if (!d || !d->day) continue;
    SvcCount c;
    svcSum(b, d->day, d->day, DAY_NH, c);
    int y, m, dd;
    dayCivil(d->day, y, m, dd);
    char date[16], lab[24], pct[24];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", y, m, dd);
    svcDayLabel(d->day, lab, sizeof(lab));
    svcPct(pct, sizeof(pct), c.avail(), c.obs);
    j.open(nullptr, '{');
    j.num("day", d->day); j.str("date", date); j.str("label", lab); j.num("dow", d->dow);
    j.boolean("holiday", (d->flags & DF_HOLIDAY) != 0); j.boolean("recorded", c.days > 0);
    j.num("slots", c.slots); j.num("slotsMax", c.slotsMax);
    j.num("obs", c.obs); j.num("avail", c.avail()); j.num("closed", c.closed); j.num("empty", c.empty); j.num("full", c.full);
    j.num("evEmpty", c.evEmpty); j.num("evFull", c.evFull); j.str("availText", pct);
    j.close('}');
  }
  j.close(']');
  return s_out.size();
}

// Une station, jour par jour (DaySt) : tranches observées / hors service / vides / pleines
EXPORT(vn_days_station_json) size_t vn_days_station_json(int i) {
  s_out.clear();
  J j(s_out);
  const DayBook & b = g_model.days;
  j.open(nullptr, '[');
  if (i >= 0 && i < g_model.n) {
    for (int k = b.count - 1; k >= 0; k--) {
      const DayNet * d = b.dayAt(k);
      const DaySt * r = b.stAt(k);
      if (!d || !d->day || !r) continue;
      const DaySt & x = r[i];
      j.open(nullptr, '{');
      j.num("day", d->day); j.num("obs", x.obs); j.num("closed", x.closed); j.num("empty", x.empty); j.num("full", x.full);
      j.close('}');
    }
  }
  j.close(']');
  return s_out.size();
}

// Résumé d'un jour pour le message du matin : disponibilité, ruptures, pires stations, problèmes
// récurrents (règles et phrases de core/service, celles de l'écran Service)
EXPORT(vn_digest_json) size_t vn_digest_json(int day) {
  s_out.clear();
  J j(s_out);
  const DayBook & b = g_model.days;
  SvcCount c;
  svcSum(b, day, day, DAY_NH, c);
  char pct[24], lab[24], hrs[24];
  svcPct(pct, sizeof(pct), c.avail(), c.obs);
  svcDayLabel(day, lab, sizeof(lab));
  j.open(nullptr, '{');
  j.num("day", day); j.str("label", lab); j.boolean("recorded", c.days > 0);
  j.str("availText", pct); j.num("evEmpty", c.evEmpty); j.num("evFull", c.evFull);
  svcPct(pct, sizeof(pct), c.empty, c.obs); j.str("emptyText", pct);
  svcPct(pct, sizeof(pct), c.full, c.obs); j.str("fullText", pct);
  svcPct(pct, sizeof(pct), c.closed, c.obs); j.str("closedText", pct);
  svcHours(hrs, sizeof(hrs), c.obs); j.str("obsHours", hrs);
  static StRank r[VN_MAX_ST];
  int skipped = 0;
  int n = svcRank(b, g_model, day, day, r, VN_MAX_ST, &skipped);
  svcSortWorst(r, n);
  j.open("worst", '[');
  for (int k = 0; k < n && k < 3; k++) {
    if (!(r[k].bad() > 0)) break;
    svcPct(pct, sizeof(pct), r[k].closed + r[k].empty + r[k].full, r[k].obs);
    j.open(nullptr, '{');
    j.str("name", g_model.st[r[k].st].name); j.str("sid", g_model.st[r[k].st].sid); j.str("badText", pct);
    j.close('}');
  }
  j.close(']');
  static Recur rec[24];
  int nr = svcRecurrent(b, g_model, day + 1, rec, 24);
  j.num("recurrent", nr);
  j.open("recur", '[');
  for (int k = 0; k < nr && k < 5; k++) {
    char t[160];
    svcRecurText(rec[k], t, sizeof(t));
    j.open(nullptr, '{');
    j.str("name", rec[k].st >= 0 && rec[k].st < g_model.n ? g_model.st[rec[k].st].name : "?");
    j.str("text", t);
    j.close('}');
  }
  j.close(']');
  j.close('}');
  return s_out.size();
}

EXPORT(vn_local_day) int vn_local_day(double t) { return localDay((int64_t)t); }
