// ── Plateforme « moteur en ligne » (WebAssembly ou natif) ─────────────
// Le cœur du firmware (src/core) tourne tel quel ; seule cette couche change :
//  - l'horloge est fournie par l'hôte (Node ou navigateur) avant chaque appel ;
//  - les « fichiers » vivent en mémoire : l'hôte les dépose (vn_fs_put) et les relit
//    (vn_fs_get), donc /vn_prof.bin, /vn_hist.bin et /vn_days.bin sont octet pour
//    octet ceux qu'écrirait la carte ;
//  - l'heure locale est celle de config.h (VN_TZ = CET-1CEST,M3.5.0,M10.5.0/3),
//    calculée ici sans base de fuseaux (wasi-libc n'en a pas).
#include "../../src/platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <vector>

namespace eng {
int64_t g_now = 0;          // epoch UTC (s), 0 = heure inconnue
double  g_ms = 0;           // horloge monotone de l'hôte (ms)
std::map<std::string, std::vector<uint8_t>> g_fs;
std::string g_log;          // journal (plat::logf), vidé par l'hôte
plat::SysInfo g_sys = { 0, 0, 0, 0, 0, 0, 0, "VelohNav Ops en ligne" };
}

// ── Heure de Luxembourg (règle de l'UE : CET/CEST) ───────────────────
// Jours depuis le 01/01/1970 d'une date civile (algorithme de H. Hinnant)
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
static void civilFromDays(int64_t z, int64_t & y, unsigned & m, unsigned & d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}
// Dernier dimanche du mois, 01:00 UTC (passage à l'heure d'été / d'hiver dans toute l'UE)
static int64_t lastSunday0100Utc(int64_t year, unsigned month) {
  int64_t last = daysFromCivil(year, month + 1, 1) - 1;          // month = 3 ou 10
  int wd = (int)(((last % 7) + 11) % 7);                         // 0 = dimanche (01/01/1970 = jeudi)
  return (last - wd) * 86400 + 3600;
}
static int utcOffsetS(int64_t t) {
  int64_t y; unsigned m, d;
  civilFromDays(t >= 0 ? t / 86400 : (t - 86399) / 86400, y, m, d);
  return (t >= lastSunday0100Utc(y, 3) && t < lastSunday0100Utc(y, 10)) ? 7200 : 3600;
}

namespace plat {

void begin() {}
bool fsBegin(bool) { return true; }
uint32_t ms() { return (uint32_t)(uint64_t)eng::g_ms; }
int64_t now() { return eng::g_now; }
bool timeValid() { return eng::g_now > 0; }

void localTime(int64_t epoch, struct tm * out) {
  memset(out, 0, sizeof(*out));
  const int off = utcOffsetS(epoch);
  int64_t l = epoch + off;
  int64_t day = l >= 0 ? l / 86400 : (l - 86399) / 86400;
  int64_t sec = l - day * 86400;
  int64_t y; unsigned m, d;
  civilFromDays(day, y, m, d);
  out->tm_year = (int)(y - 1900);
  out->tm_mon = (int)m - 1;
  out->tm_mday = (int)d;
  out->tm_hour = (int)(sec / 3600);
  out->tm_min = (int)(sec % 3600 / 60);
  out->tm_sec = (int)(sec % 60);
  out->tm_wday = (int)(((day % 7) + 11) % 7);
  out->tm_yday = (int)(day - daysFromCivil(y, 1, 1));
  out->tm_isdst = off == 7200;
}

void * psAlloc(size_t n) { return calloc(1, n); }
void psFree(void * p) { free(p); }

bool fsWrite(const char * p, const void * d, size_t n) {
  const uint8_t * b = (const uint8_t *)d;
  eng::g_fs[p] = std::vector<uint8_t>(b, b + n);
  return true;
}
bool fsRead(const char * p, void * d, size_t n) {
  auto it = eng::g_fs.find(p);
  if (it == eng::g_fs.end() || it->second.size() < n) return false;
  memcpy(d, it->second.data(), n);
  return true;
}
long fsSize(const char * p) {
  auto it = eng::g_fs.find(p);
  return it == eng::g_fs.end() ? -1 : (long)it->second.size();
}
bool fsRemove(const char * p) { return eng::g_fs.erase(p) > 0; }

void logf(const char * fmt, ...) {
  char b[512];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  if (eng::g_log.size() > 64 * 1024) eng::g_log.clear();
  eng::g_log += b;
  eng::g_log += '\n';
}

void setBacklight(uint8_t) {}
void restart() { logf("[en ligne] redémarrage demandé (ignoré)"); }
void sysInfo(SysInfo & o) { o = eng::g_sys; }
void modelLock() {}
void modelUnlock() {}

} // namespace plat
