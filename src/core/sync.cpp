#include "sync.h"
#include "model.h"
#include "days.h"
#include "dump.h"
#include "../config.h"
#include "../platform/platform.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#if __has_include("secrets.h")
#include "secrets.h"      // facultatif : VN_SYNC_URL par défaut
#endif

namespace vn {

SyncCfg g_sync;

static const char * const NAMES[SK_COUNT] = { "vn_prof.bin", "vn_hist.bin", "vn_days.bin" };
static const char * const PATHS[SK_COUNT] = { "/vn_prof.bin", "/vn_hist.bin", "/vn_days.bin" };
static const char * const LABELS[SK_COUNT] = { "profils", "historique 24 h", "r\xC3\xA9sum\xC3\xA9s par jour" };
const char * syncFileName(int k) { return k >= 0 && k < SK_COUNT ? NAMES[k] : "?"; }
const char * syncFilePath(int k) { return k >= 0 && k < SK_COUNT ? PATHS[k] : "?"; }
const char * syncKindLabel(int k) { return k >= 0 && k < SK_COUNT ? LABELS[k] : "?"; }

// ── Réglage ──────────────────────────────────────────────────────────
void syncCfgDefaults(SyncCfg & c) {
  memset(&c, 0, sizeof(c));
#ifdef VN_SYNC_URL
  syncNormalizeUrl(VN_SYNC_URL, c.url, sizeof(c.url));
#endif
#ifdef VN_SYNC_TOKEN
  snprintf(c.token, sizeof(c.token), "%s", VN_SYNC_TOKEN);
#endif
  c.atBoot = true;
  c.push = true;
}

bool syncNormalizeUrl(const char * in, char * out, size_t n) {
  if (!in || !out || n < 16) return false;
  char b[160];
  size_t o = 0;
  for (const char * p = in; *p && o + 1 < sizeof(b); p++) if (!isspace((unsigned char)*p)) b[o++] = *p;
  b[o] = 0;
  if (!o) { out[0] = 0; return false; }
  char t[192];
  if (strncmp(b, "http://", 7) && strncmp(b, "https://", 8)) {
    if (strstr(b, "://")) { out[0] = 0; return false; }          // autre protocole
    snprintf(t, sizeof(t), "http://%s", b);
  } else snprintf(t, sizeof(t), "%s", b);
  const size_t scheme = strncmp(t, "https://", 8) ? 7 : 8;   // « http:// » ou « https:// »
  size_t L = strlen(t);
  static const char * SUFFIX = "/sync/manifest";
  size_t sl = strlen(SUFFIX);
  if (L > sl && !strcmp(t + L - sl, SUFFIX)) { L -= sl; t[L] = 0; }
  while (L > scheme && t[L - 1] == '/') t[--L] = 0;
  if (L <= scheme || t[scheme] == '/' || L >= n) { out[0] = 0; return false; }   // pas d'hôte
  memcpy(out, t, L + 1);
  return true;
}

bool syncCfgLoad() {
  syncCfgDefaults(g_sync);
  long sz = plat::fsSize(SYNC_CFG_PATH);
  if (sz <= 0 || sz > 1024) return false;
  char b[1025];
  if (!plat::fsRead(SYNC_CFG_PATH, b, (size_t)sz)) return false;
  b[sz] = 0;
  for (char * line = strtok(b, "\n"); line; line = strtok(nullptr, "\n")) {
    char * eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char * v = eq + 1;
    if (!strcmp(line, "url")) { if (!syncNormalizeUrl(v, g_sync.url, sizeof(g_sync.url))) g_sync.url[0] = 0; }
    else if (!strcmp(line, "token")) snprintf(g_sync.token, sizeof(g_sync.token), "%s", v);
    else if (!strcmp(line, "boot")) g_sync.atBoot = atoi(v) != 0;
    else if (!strcmp(line, "push")) g_sync.push = atoi(v) != 0;
  }
  return true;
}

bool syncCfgSave() {
  char b[320];
  int n = snprintf(b, sizeof(b), "url=%s\ntoken=%s\nboot=%d\npush=%d\n", g_sync.url, g_sync.token, g_sync.atBoot ? 1 : 0,
                   g_sync.push ? 1 : 0);
  return n > 0 && plat::fsWrite(SYNC_CFG_PATH, b, (size_t)n);
}

// ── En-têtes ─────────────────────────────────────────────────────────
// FileHdr (model.cpp) : magic u32 · ver u16 · nSt u16 · head i32 · count i32 · learnSince i64 (16) ·
// learnSamples u32 (24) · savedAt i64 (32) ; DaysFileHdr (days.h) : nDays u16 (8) · savedAt i64 (32).
static int64_t rd64(const uint8_t * b, size_t o) { int64_t v; memcpy(&v, b + o, 8); return v; }
static uint32_t rd32(const uint8_t * b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static uint16_t rd16(const uint8_t * b, size_t o) { uint16_t v; memcpy(&v, b + o, 2); return v; }

bool syncHeaderOf(int kind, const uint8_t * b, size_t n, SyncLocal & l) {
  if (!b || n < 40) return false;
  switch (kind) {
    case SK_PROF:
      if (!Model::profFileOk(b, n)) return false;
      l.profSince = rd64(b, 16); l.profSamples = rd32(b, 24);
      break;
    case SK_HIST:
      if (!Model::histFileOk(b, n, 0)) return false;          // fraîcheur jugée par la décision, pas ici
      l.histSaved = rd64(b, 32);
      break;
    case SK_DAYS:
      if (!DayBook::check(b, n)) return false;
      l.daysN = rd16(b, 8); l.daysSaved = rd64(b, 32);
      break;
    default: return false;
  }
  l.has[kind] = true;
  return true;
}

void syncScanLocal(SyncLocal & l) {
  memset(&l, 0, sizeof(l));
  for (int k = 0; k < SK_COUNT; k++) {
    long sz = plat::fsSize(PATHS[k]);
    if (sz < 40) continue;
    uint8_t * b = (uint8_t *)plat::psAlloc((size_t)sz);       // le fichier entier : sa taille est contrôlée
    if (!b) continue;
    if (plat::fsRead(PATHS[k], b, (size_t)sz)) syncHeaderOf(k, b, (size_t)sz, l);
    plat::psFree(b);
  }
}

void syncLocalHeader(const SyncLocal & l, char * out, size_t n) {
  snprintf(out, n, "prof=%lu;hist=%lld;days=%u", l.has[SK_PROF] ? (unsigned long)l.profSamples : 0UL,
           l.has[SK_HIST] ? (long long)l.histSaved : 0LL, l.has[SK_DAYS] ? (unsigned)l.daysN : 0U);
}

// ── Manifeste ────────────────────────────────────────────────────────
static uint32_t hex32(const char * s) { return s ? (uint32_t)strtoul(s, nullptr, 16) : 0; }

bool syncParseManifest(const char * json, size_t len, SyncManifest & m) {
  memset(&m, 0, sizeof(m));
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  m.proto = doc["proto"] | 0;
  if (m.proto != 1) return false;
  snprintf(m.id, sizeof(m.id), "%s", (const char *)(doc["id"] | ""));
  snprintf(m.base, sizeof(m.base), "%s", (const char *)(doc["base"] | ""));
  snprintf(m.name, sizeof(m.name), "%s", (const char *)(doc["name"] | ""));
  snprintf(m.fw, sizeof(m.fw), "%s", (const char *)(doc["fw"] | ""));
  m.t = doc["t"] | (int64_t)0;
  static const char * const KEYS[SK_COUNT] = { "prof", "hist", "days" };
  for (int k = 0; k < SK_COUNT; k++) {
    JsonObjectConst o = doc[KEYS[k]];
    if (o.isNull()) continue;
    SyncRemote & f = m.f[k];
    f.size = o["size"] | 0u;
    f.crc = hex32(o["crc"] | "0");
    f.ver = o["ver"] | 0;
    f.present = f.size >= 40;
  }
  m.profSamples = doc["prof"]["samples"] | 0u;
  m.profSince = doc["prof"]["since"] | (int64_t)0;
  m.histSaved = doc["hist"]["savedAt"] | (int64_t)0;
  m.daysN = doc["days"]["n"] | 0;
  m.daysSaved = doc["days"]["savedAt"] | (int64_t)0;
  // Les fichiers se lisent sous base ; une base absente ou étrangère au protocole HTTP est refusée
  size_t bl = strlen(m.base);
  if (!bl || (strncmp(m.base, "http://", 7) && strncmp(m.base, "https://", 8)) || m.base[bl - 1] != '/') return false;
  return m.f[SK_PROF].present || m.f[SK_HIST].present || m.f[SK_DAYS].present;
}

// ── Décision ─────────────────────────────────────────────────────────
static const int64_t HIST_FRESH_S = 20 * 3600;     // Model::load ne relit pas un historique plus vieux
static const int64_t HIST_NEWER_S = 60;

void syncDecide(const SyncLocal & l, const SyncManifest & m, int64_t now, bool allowPush, SyncPlan & p) {
  memset(&p, 0, sizeof(p));
  char a[24], b[24];
  // Profils : le plus appris gagne (relevés de 5 min versés au profil)
  if (!m.f[SK_PROF].present) snprintf(p.why[SK_PROF], 96, "absents du serveur");
  else if (!l.has[SK_PROF]) {
    p.take[SK_PROF] = m.profSamples > 0;
    snprintf(p.why[SK_PROF], 96, p.take[SK_PROF] ? "aucun sur la carte : ceux du serveur (%lu relev\xC3\xA9s)" : "vides des deux c\xC3\xB4t\xC3\xA9s",
             (unsigned long)m.profSamples);
  } else {
    snprintf(a, sizeof(a), "%lu", (unsigned long)m.profSamples);
    snprintf(b, sizeof(b), "%lu", (unsigned long)l.profSamples);
    if (m.profSamples > l.profSamples) {
      p.take[SK_PROF] = true;
      snprintf(p.why[SK_PROF], 96, "serveur plus appris : %s relev\xC3\xA9s contre %s", a, b);
    } else if (l.profSamples > m.profSamples) {
      p.push[SK_PROF] = allowPush;
      snprintf(p.why[SK_PROF], 96, "carte plus apprise : %s relev\xC3\xA9s contre %s%s", b, a, allowPush ? ", envoy\xC3\xA9s" : "");
    } else snprintf(p.why[SK_PROF], 96, "identiques (%s relev\xC3\xA9s)", b);
  }
  // Historique 24 h : le plus récent, s'il est encore relisible
  if (!m.f[SK_HIST].present) snprintf(p.why[SK_HIST], 96, "absent du serveur");
  else if (now > 0 && (m.histSaved <= 0 || now - m.histSaved >= HIST_FRESH_S))
    snprintf(p.why[SK_HIST], 96, "celui du serveur a plus de 20 h : ignor\xC3\xA9");
  else if (!l.has[SK_HIST] || m.histSaved > l.histSaved + HIST_NEWER_S) {
    p.take[SK_HIST] = true;
    if (!l.has[SK_HIST]) snprintf(p.why[SK_HIST], 96, "aucun sur la carte : celui du serveur");
    else snprintf(p.why[SK_HIST], 96, "serveur plus r\xC3\xA9" "cent de %lld min", (long long)((m.histSaved - l.histSaved) / 60));
  } else snprintf(p.why[SK_HIST], 96, "celui de la carte est aussi r\xC3\xA9" "cent");
  // Résumés par jour : le plus de jours ; à égalité, le plus récent
  if (!m.f[SK_DAYS].present) snprintf(p.why[SK_DAYS], 96, "absents du serveur");
  else if (!l.has[SK_DAYS]) {
    p.take[SK_DAYS] = m.daysN > 0;
    snprintf(p.why[SK_DAYS], 96, "aucun sur la carte : %u jour%s du serveur", (unsigned)m.daysN, m.daysN > 1 ? "s" : "");
  } else if (m.daysN > l.daysN || (m.daysN == l.daysN && m.daysSaved > l.daysSaved + HIST_NEWER_S)) {
    p.take[SK_DAYS] = true;
    snprintf(p.why[SK_DAYS], 96, "serveur : %u jour%s contre %u", (unsigned)m.daysN, m.daysN > 1 ? "s" : "", (unsigned)l.daysN);
  } else if (l.daysN > m.daysN) {
    p.push[SK_DAYS] = allowPush;
    snprintf(p.why[SK_DAYS], 96, "carte : %u jour%s contre %u%s", (unsigned)l.daysN, l.daysN > 1 ? "s" : "", (unsigned)m.daysN,
             allowPush ? ", envoy\xC3\xA9s" : "");
  } else snprintf(p.why[SK_DAYS], 96, "ceux de la carte sont aussi complets");
}

bool syncBlobOk(int kind, const uint8_t * b, size_t n, const SyncRemote & f, int64_t now, const char ** why) {
  const char * w = nullptr;
  if (!b || n != f.size) w = "taille diff\xC3\xA9rente du manifeste";
  else if (crc32(b, n) != f.crc) w = "CRC32 faux";
  else if (kind == SK_PROF && !Model::profFileOk(b, n)) w = "en-t\xC3\xAAte de profils illisible";
  else if (kind == SK_HIST && !Model::histFileOk(b, n, now)) w = "historique illisible ou trop ancien";
  else if (kind == SK_DAYS && !DayBook::check(b, n)) w = "r\xC3\xA9sum\xC3\xA9s illisibles";
  else if (kind < 0 || kind >= SK_COUNT) w = "fichier inconnu";
  if (why) *why = w ? w : "";
  return !w;
}

// ── Déroulé ──────────────────────────────────────────────────────────
static void kindsText(uint8_t bits, char * out, size_t n) {
  out[0] = 0;
  for (int k = 0; k < SK_COUNT; k++) {
    if (!(bits & (1 << k))) continue;
    size_t L = strlen(out);
    snprintf(out + L, n - L, "%s%s", L ? ", " : "", LABELS[k]);
  }
}

int syncRun(SyncIo & io, const char * baseUrl, int64_t now, bool allowPush, SyncStatus & st) {
  st.state = SS_RUNNING; st.took = st.pushed = 0; st.code = 0; st.text[0] = 0;
  SyncLocal l;
  syncScanLocal(l);
  syncLocalHeader(l, io.localHdr, sizeof(io.localHdr));
  // 1. Manifeste
  char url[224];
  snprintf(url, sizeof(url), "%s/sync/manifest", baseUrl);
  const size_t MCAP = 4096;
  uint8_t * mb = (uint8_t *)plat::psAlloc(MCAP + 1);
  if (!mb) { st.state = SS_FAIL; snprintf(st.text, sizeof(st.text), "m\xC3\xA9moire insuffisante"); st.at = now; return -1; }
  size_t mlen = 0;
  int code = io.get(io.ctx, url, mb, MCAP, mlen);
  SyncManifest m;
  bool okM = code == 200 && mlen > 0 && syncParseManifest((const char *)mb, mlen, m);
  plat::psFree(mb);
  if (!okM) {
    st.state = SS_FAIL; st.code = (int16_t)(code == 200 ? -100 : code); st.at = now;
    if (code == 200) snprintf(st.text, sizeof(st.text), "manifeste illisible (serveur d'une autre version ?)");
    else if (code > 0) snprintf(st.text, sizeof(st.text), "serveur : r\xC3\xA9ponse %d%s", code, code == 401 ? " (jeton requis)" : "");
    else snprintf(st.text, sizeof(st.text), "serveur injoignable (code %d)", code);
    plat::logf("[sync] %s : %s", url, st.text);
    return -1;
  }
  plat::logf("[sync] serveur \xC2\xAB %s \xC2\xBB v%s, instantan\xC3\xA9 %s", m.name, m.fw, m.id);
  // 2. Décision
  SyncPlan p;
  syncDecide(l, m, now, allowPush, p);
  for (int k = 0; k < SK_COUNT; k++)
    plat::logf("[sync] %s : %s", LABELS[k], p.why[k]);
  // 3. Téléchargements vérifiés, puis écriture (fichier par fichier : chacun est indépendant)
  int adopted = 0;
  const char * firstErr = nullptr;
  for (int k = 0; k < SK_COUNT; k++) {
    if (!p.take[k]) continue;
    const SyncRemote & f = m.f[k];
    uint8_t * b = (uint8_t *)plat::psAlloc(f.size + 1);
    if (!b) { firstErr = firstErr ? firstErr : "m\xC3\xA9moire insuffisante"; continue; }
    snprintf(url, sizeof(url), "%s%s", m.base, NAMES[k]);
    size_t got = 0;
    int c = io.get(io.ctx, url, b, f.size, got);
    const char * why = "";
    if (c != 200) { st.code = (int16_t)c; why = "t\xC3\xA9l\xC3\xA9" "chargement interrompu"; }
    else if (syncBlobOk(k, b, got, f, now, &why)) {
      if (plat::fsWrite(PATHS[k], b, got)) { st.took |= (uint8_t)(1 << k); adopted++; }
      else why = "\xC3\xA9" "criture en flash impossible";
    }
    if (*why) {
      plat::logf("[sync] %s refus\xC3\xA9s : %s (code %d)", LABELS[k], why, c);
      if (!firstErr) firstErr = why;
    }
    plat::psFree(b);
  }
  // 4. Envois : ce que la carte a appris de plus que le serveur (fichiers en flash, tels quels)
  for (int k = 0; k < SK_COUNT; k++) {
    if (!p.push[k]) continue;
    long sz = plat::fsSize(PATHS[k]);
    if (sz <= 0) continue;
    uint8_t * b = (uint8_t *)plat::psAlloc((size_t)sz);
    if (!b) continue;
    if (plat::fsRead(PATHS[k], b, (size_t)sz)) {
      snprintf(url, sizeof(url), "%supload/%s", m.base, NAMES[k]);
      int c = io.post(io.ctx, url, b, (size_t)sz, crc32(b, (size_t)sz));
      if (c == 200) st.pushed |= (uint8_t)(1 << k);
      else plat::logf("[sync] envoi des %s : code %d", LABELS[k], c);
    }
    plat::psFree(b);
  }
  // 5. Compte rendu au serveur (son registre des écrans) et phrase de l'écran
  char took[64], pushed[64];
  kindsText(st.took, took, sizeof(took));
  kindsText(st.pushed, pushed, sizeof(pushed));
  st.state = adopted ? SS_TOOK : firstErr ? SS_FAIL : SS_NOTHING;
  if (adopted && st.pushed) snprintf(st.text, sizeof(st.text), "Adopt\xC3\xA9 : %s \xC2\xB7 envoy\xC3\xA9 : %s", took, pushed);
  else if (adopted) snprintf(st.text, sizeof(st.text), "Adopt\xC3\xA9 : %s%s%s", took, firstErr ? " \xC2\xB7 " : "", firstErr ? firstErr : "");
  else if (firstErr) snprintf(st.text, sizeof(st.text), "Rien adopt\xC3\xA9 : %s", firstErr);
  else if (st.pushed) snprintf(st.text, sizeof(st.text), "D\xC3\xA9j\xC3\xA0 \xC3\xA0 jour \xC2\xB7 envoy\xC3\xA9 au serveur : %s", pushed);
  else snprintf(st.text, sizeof(st.text), "D\xC3\xA9j\xC3\xA0 \xC3\xA0 jour : rien de meilleur sur le serveur");
  st.at = now;
  char took2[24] = "", pushed2[24] = "";
  for (int k = 0; k < SK_COUNT; k++) {
    static const char * const SH[SK_COUNT] = { "prof", "hist", "days" };
    if (st.took & (1 << k)) { size_t L = strlen(took2); snprintf(took2 + L, sizeof(took2) - L, "%s%s", L ? "," : "", SH[k]); }
    if (st.pushed & (1 << k)) { size_t L = strlen(pushed2); snprintf(pushed2 + L, sizeof(pushed2) - L, "%s%s", L ? "," : "", SH[k]); }
  }
  static const char * const RES[] = { "jamais", "en-cours", "adopte", "a-jour", "echec" };
  snprintf(url, sizeof(url), "%sreport?took=%s&pushed=%s&result=%s", m.base, took2, pushed2, RES[st.state]);
  size_t rl = 0;
  uint8_t rb[64];
  io.get(io.ctx, url, rb, sizeof(rb) - 1, rl);
  plat::logf("[sync] %s", st.text);
  return adopted;
}

void syncStatusLine(const SyncStatus & st, int64_t now, char * out, size_t n) {
  if (st.state == SS_NEVER || !st.at) { snprintf(out, n, "Jamais synchronis\xC3\xA9" "e depuis le d\xC3\xA9marrage"); return; }
  if (st.state == SS_RUNNING) { snprintf(out, n, "Synchronisation en cours\xE2\x80\xA6"); return; }
  struct tm lt;
  plat::localTime(st.at, &lt);
  snprintf(out, n, "%02d/%02d %02d:%02d \xC2\xB7 %s", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min, st.text);
  (void)now;
}

} // namespace vn
