#include "gbfs.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <math.h>

namespace vn {

// ── Dates ISO 8601 → epoch UTC ───────────────────────────────────────
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {   // H. Hinnant
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

int64_t parseIso8601(const char * s) {
  if (!s) return 0;
  int Y, M, D, h = 0, m = 0, sec = 0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
  if (Y < 2000 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
  int64_t t = daysFromCivil(Y, M, D) * 86400LL + h * 3600 + m * 60 + sec;
  // Décalage éventuel « +02:00 » (le flux est en Z, mais on reste tolérant)
  const char * tz = s + 19;
  while (*tz == '.' || isdigit((unsigned char)*tz)) tz++;
  if ((*tz == '+' || *tz == '-') && isdigit((unsigned char)tz[1])) {
    int oh = 0, om = 0;
    sscanf(tz + 1, "%2d:%2d", &oh, &om);
    int off = (oh * 3600 + om * 60) * (*tz == '+' ? 1 : -1);
    t -= off;
  }
  return t;
}

// ── Noms de stations ─────────────────────────────────────────────────
// UTF-8 ↔ points de code (latin de base, Latin-1, œ) : suffisant pour les
// noms Vel'OH! (luxembourgeois / français / allemand).
static int utf8Decode(const char * s, uint32_t * cp, int max) {
  int n = 0;
  const unsigned char * p = (const unsigned char *)s;
  while (*p && n < max) {
    uint32_t c = *p;
    if (c < 0x80) { p++; }
    else if ((c & 0xE0) == 0xC0 && p[1]) { c = ((c & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
    else { p++; continue; }
    cp[n++] = c;
  }
  return n;
}
static size_t utf8Encode(uint32_t c, char * o) {
  if (c < 0x80) { o[0] = (char)c; return 1; }
  if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
  if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
  o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}
static uint32_t cpLower(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c == 0x152) return 0x153;
  return c;
}
static uint32_t cpUpper(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
  if (c == 0x153) return 0x152;
  return c;
}
static bool cpIsLetter(uint32_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
}

static bool wordIn(const uint32_t * w, int n, const char * const * list) {
  for (int i = 0; list[i]; i++) {
    int L = (int)strlen(list[i]);
    if (L != n) continue;
    bool eq = true;
    for (int k = 0; k < n && eq; k++) eq = cpLower(w[k]) == (uint32_t)tolower((unsigned char)list[i][k]);
    if (eq) return true;
  }
  return false;
}

void cleanStationName(const char * in, char * out, size_t n) {
  if (!n) return;
  out[0] = 0;
  if (!in) return;
  // 1) Préfixe numérique « #00001- » / « 00001 - » / « 00001 – »
  const char * p = in;
  const char * q = p;
  if (*q == '#') q++;
  const char * digits = q;
  while (isdigit((unsigned char)*q)) q++;
  if (q > digits) {
    const char * r = q;
    while (*r == ' ') r++;
    if (*r == '-') r++;
    else if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x80 && (unsigned char)r[2] == 0x93) r += 3;
    else r = nullptr;
    if (r) { while (*r == ' ') r++; p = r; }
  }
  // 2) Casse « titre » intelligente
  uint32_t cp[96];
  int len = utf8Decode(p, cp, 96);
  while (len && cp[len - 1] == ' ') len--;
  static const char * const SMALL[] = { "de", "du", "des", "la", "le", "les", "et", "au", "aux", "sur", "en",
                                        "der", "den", "dem", "vum", "vun", "op", "a", nullptr };
  static const char * const ACRO[]  = { "BGL", "BNP", "PWC", "INS", "CAP", "CHL", "CFL", "ZAC", "BCEE", "ING",
                                        "EIB", "AVL", "CHU", "RTL", "P+R", nullptr };
  size_t o = 0;
  int i = 0, word = 0;
  while (i < len) {
    if (cp[i] == ' ') { if (o + 1 < n) out[o++] = ' '; i++; continue; }
    int s = i;
    while (i < len && cp[i] != ' ') i++;
    int L = i - s;
    uint32_t w[96];
    memcpy(w, cp + s, L * sizeof(uint32_t));
    bool hasDigit = false, hasDot = false, roman = L >= 2;
    for (int k = 0; k < L; k++) {
      if (w[k] >= '0' && w[k] <= '9') hasDigit = true;
      if (w[k] == '.') hasDot = true;
      if (w[k] != 'I' && w[k] != 'V' && w[k] != 'X') roman = false;
    }
    // mot entre parenthèses : on teste le cœur
    int cs = 0, ce = L;
    while (cs < ce && !cpIsLetter(w[cs])) cs++;
    while (ce > cs && !cpIsLetter(w[ce - 1])) ce--;
    bool keep = hasDigit || hasDot || roman || wordIn(w + cs, ce - cs, ACRO);
    if (!keep) {
      bool small = word > 0 && wordIn(w, L, SMALL);
      bool start = !small;
      for (int k = 0; k < L; k++) {
        uint32_t c = w[k];
        if (!cpIsLetter(c)) {
          // d'/l' en tête de mot : l'élision reste minuscule, la suite prend la majuscule
          if (c == '\'' || c == 0x2019) {
            bool sAfter = k + 1 < L && (w[k + 1] == 'S' || w[k + 1] == 's') && (k + 2 == L || !cpIsLetter(w[k + 2]));
            start = !sAfter;
            if (k == 1 && word > 0) w[0] = cpLower(w[0]);
          } else if (c == '-' || c == '/' || c == '(') start = true;
          continue;
        }
        w[k] = start ? cpUpper(c) : cpLower(c);
        start = false;
      }
    }
    for (int k = 0; k < L; k++) {
      char tmp[4];
      size_t b = utf8Encode(w[k], tmp);
      if (o + b >= n) break;
      memcpy(out + o, tmp, b);
      o += b;
    }
    word++;
  }
  out[o < n ? o : n - 1] = 0;
}

bool isNonPublic(const char * name) {
  if (!name) return false;
  // \bATELIER\b insensible à la casse
  for (const char * p = name; *p; p++) {
    if (strncasecmp(p, "atelier", 7) == 0) {
      bool before = p == name || !isalpha((unsigned char)p[-1]);
      bool after = !isalpha((unsigned char)p[7]);
      if (before && after) return true;
    }
  }
  return false;
}

// ── Types de véhicules ───────────────────────────────────────────────
bool ElecTypes::isElec(const char * typeId) const {
  if (!typeId) return false;
  if (known) {
    for (int i = 0; i < n; i++) if (strcmp(id[i], typeId) == 0) return true;
    return false;
  }
  // Heuristique (identique au JS) : « elec », « ebike », « e-bike »
  char low[32];
  size_t k = 0;
  for (; typeId[k] && k < sizeof(low) - 1; k++) low[k] = (char)tolower((unsigned char)typeId[k]);
  low[k] = 0;
  return strstr(low, "elec") || strstr(low, "ebike") || strstr(low, "e-bike");
}

bool gbfsParseTypes(const char * buf, size_t len, ElecTypes & et) {
  JsonDocument filter;
  JsonObject f = filter["data"]["vehicle_types"].add<JsonObject>();
  f["vehicle_type_id"] = true;
  f["propulsion_type"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, buf, len, DeserializationOption::Filter(filter))) return false;
  JsonArrayConst arr = doc["data"]["vehicle_types"].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  et.n = 0;
  for (JsonObjectConst t : arr) {
    const char * pid = t["propulsion_type"] | "human";
    const char * vid = t["vehicle_type_id"] | "";
    if (strcmp(pid, "human") != 0 && *vid && et.n < 6) {
      strncpy(et.id[et.n], vid, sizeof(et.id[0]) - 1);
      et.id[et.n][sizeof(et.id[0]) - 1] = 0;
      et.n++;
    }
  }
  et.known = true;
  return true;
}

// ── station_information ──────────────────────────────────────────────
static void readSid(JsonVariantConst v, char * out, size_t n) {
  if (v.is<const char *>()) { strncpy(out, v.as<const char *>(), n - 1); out[n - 1] = 0; }
  else if (v.is<long>()) snprintf(out, n, "%ld", v.as<long>());
  else out[0] = 0;
}

static const char * pickLang(JsonVariantConst v, const char * lang) {
  if (v.is<const char *>()) return v.as<const char *>();
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.isNull()) return "";
  const char * first = nullptr;
  for (JsonObjectConst e : a) {
    const char * txt = e["text"] | (const char *)nullptr;
    if (!txt || !*txt) continue;
    if (!first) first = txt;
    const char * l = e["language"] | "";
    if (strcmp(l, lang) == 0) return txt;
  }
  return first ? first : "";
}

int gbfsParseInfo(const char * buf, size_t len, InfoRec * out, int max) {
  JsonDocument filter;
  JsonObject f = filter["data"]["stations"].add<JsonObject>();
  f["station_id"] = true; f["name"] = true; f["lat"] = true; f["lon"] = true;
  f["address"] = true; f["capacity"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, buf, len, DeserializationOption::Filter(filter))) return -1;
  JsonArrayConst arr = doc["data"]["stations"].as<JsonArrayConst>();
  if (arr.isNull()) return -1;
  int n = 0;
  for (JsonObjectConst s : arr) {
    if (n >= max) break;
    InfoRec & r = out[n];
    memset(&r, 0, sizeof(r));
    readSid(s["station_id"], r.sid, sizeof(r.sid));
    if (!r.sid[0]) continue;
    cleanStationName(pickLang(s["name"], "fr"), r.name, sizeof(r.name));
    r.cap = (uint16_t)(s["capacity"] | 0);
    // Stations fantômes (nom vide, capacité 0) et dépôts : hors parc public
    if ((!r.name[0] && r.cap == 0) || isNonPublic(r.name)) continue;
    if (!s["lat"].is<float>() || !s["lon"].is<float>()) continue;
    r.lat = s["lat"].as<float>();
    r.lon = s["lon"].as<float>();
    if (!isfinite(r.lat) || !isfinite(r.lon)) continue;
    const char * addr = s["address"] | "";
    strncpy(r.addr, addr, sizeof(r.addr) - 1);
    if (!r.name[0]) snprintf(r.name, sizeof(r.name), "Station %s", r.sid);
    n++;
  }
  return n;
}

// ── station_status ───────────────────────────────────────────────────
int gbfsParseStatus(const char * buf, size_t len, StatusRec * out, int max,
                    const ElecTypes & et, int64_t * lastUpdated) {
  JsonDocument filter;
  filter["last_updated"] = true;
  JsonObject f = filter["data"]["stations"].add<JsonObject>();
  f["station_id"] = true;
  f["num_vehicles_available"] = true;
  f["num_bikes_available"] = true;          // GBFS v2 (tolérance)
  f["vehicle_types_available"] = true;
  f["num_vehicles_disabled"] = true;
  f["num_bikes_disabled"] = true;
  f["num_docks_available"] = true;
  f["num_docks_disabled"] = true;
  f["is_installed"] = true; f["is_renting"] = true; f["is_returning"] = true;
  f["last_reported"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, buf, len, DeserializationOption::Filter(filter))) return -1;
  JsonArrayConst arr = doc["data"]["stations"].as<JsonArrayConst>();
  if (arr.isNull()) return -1;
  if (lastUpdated) {
    JsonVariantConst lu = doc["last_updated"];
    *lastUpdated = lu.is<const char *>() ? parseIso8601(lu.as<const char *>()) : (lu.is<long>() ? lu.as<long>() : 0);
  }
  int n = 0;
  for (JsonObjectConst s : arr) {
    if (n >= max) break;
    StatusRec & r = out[n];
    memset(&r, 0, sizeof(r));
    readSid(s["station_id"], r.sid, sizeof(r.sid));
    if (!r.sid[0]) continue;
    int typed = 0, elec = 0;
    for (JsonObjectConst t : s["vehicle_types_available"].as<JsonArrayConst>()) {
      int c = t["count"] | 0;
      typed += c;
      if (et.isElec(t["vehicle_type_id"] | "")) elec += c;
    }
    int bikes = s["num_vehicles_available"].is<int>() ? s["num_vehicles_available"].as<int>()
              : s["num_bikes_available"].is<int>() ? s["num_bikes_available"].as<int>() : typed;
    if (bikes < 0) bikes = 0;
    r.bikes    = (uint16_t)bikes;
    r.elec     = (uint16_t)(elec < bikes ? elec : bikes);
    r.docks    = (uint16_t)((int)(s["num_docks_available"] | 0) > 0 ? (int)(s["num_docks_available"] | 0) : 0);
    r.bikesDis = (uint16_t)(s["num_vehicles_disabled"].is<int>() ? s["num_vehicles_disabled"].as<int>()
                                                                  : (int)(s["num_bikes_disabled"] | 0));
    r.docksDis = (uint16_t)(s["num_docks_disabled"] | 0);
    r.installed = s["is_installed"].is<bool>() ? s["is_installed"].as<bool>() : true;
    r.renting   = r.installed && (s["is_renting"].is<bool>() ? s["is_renting"].as<bool>() : true);
    r.returning = r.installed && (s["is_returning"].is<bool>() ? s["is_returning"].as<bool>() : true);
    JsonVariantConst lr = s["last_reported"];
    r.lastReported = lr.is<const char *>() ? parseIso8601(lr.as<const char *>()) : (lr.is<long>() ? lr.as<long>() : 0);
    n++;
  }
  return n;
}

} // namespace vn
