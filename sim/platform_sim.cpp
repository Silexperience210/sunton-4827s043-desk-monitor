// ── Plateforme « simulateur PC » ─────────────────────────────────────
// Horloge simulée (pilotée par sim_main), fichiers dans un dossier local,
// verrou factice (un seul fil d'exécution).
#include "../src/platform/platform.h"
#include "vn_lv_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

namespace sim {
int64_t  g_epoch0 = 0;      // epoch au démarrage de la simulation
uint32_t g_ms = 0;          // ms écoulées
const char * g_fsDir = "sim_fs";
uint8_t  g_backlight = 100;
}

extern "C" {
void * vn_lv_malloc(size_t n) { return malloc(n); }
void   vn_lv_free(void * p) { free(p); }
void * vn_lv_realloc(void * p, size_t n) { return realloc(p, n); }
uint32_t vn_lv_tick(void) { return sim::g_ms; }
}

namespace plat {

void begin() {}
uint32_t ms() { return sim::g_ms; }
int64_t now() { return sim::g_epoch0 ? sim::g_epoch0 + sim::g_ms / 1000 : 0; }
bool timeValid() { return sim::g_epoch0 != 0; }

void localTime(int64_t epoch, struct tm * out) {
  static bool tzSet = false;
  if (!tzSet) { setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset(); tzSet = true; }
  time_t t = (time_t)epoch;
  localtime_r(&t, out);
}

void * psAlloc(size_t n) { return calloc(1, n); }
void psFree(void * p) { free(p); }

static void path(char * out, size_t n, const char * p) {
  mkdir(sim::g_fsDir, 0755);
  snprintf(out, n, "%s%s", sim::g_fsDir, p);
}
bool fsWrite(const char * p, const void * d, size_t n) {
  char f[256]; path(f, sizeof(f), p);
  FILE * fp = fopen(f, "wb");
  if (!fp) return false;
  bool ok = fwrite(d, 1, n, fp) == n;
  fclose(fp);
  return ok;
}
bool fsRead(const char * p, void * d, size_t n) {
  char f[256]; path(f, sizeof(f), p);
  FILE * fp = fopen(f, "rb");
  if (!fp) return false;
  bool ok = fread(d, 1, n, fp) == n;
  fclose(fp);
  return ok;
}
long fsSize(const char * p) {
  char f[256]; path(f, sizeof(f), p);
  struct stat st;
  return stat(f, &st) == 0 ? (long)st.st_size : -1;
}
bool fsRemove(const char * p) { char f[256]; path(f, sizeof(f), p); return remove(f) == 0; }

void logf(const char * fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

void setBacklight(uint8_t pct) { sim::g_backlight = pct; }
void restart() { logf("[sim] restart demandé"); }

void sysInfo(SysInfo & o) {
  o.heapFree = 187 * 1024; o.heapMin = 150 * 1024;
  o.psramFree = 7600 * 1024; o.psramSize = 8192 * 1024;
  o.uptimeS = 2 * 86400 + 4 * 3600 + sim::g_ms / 1000;
  o.fsUsed = 420 * 1024; o.fsTotal = 3392 * 1024;
  o.chip = "ESP32-S3 (simulateur)";
}

void modelLock() {}
void modelUnlock() {}

} // namespace plat
