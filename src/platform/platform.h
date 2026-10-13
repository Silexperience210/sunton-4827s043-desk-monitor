// ── Abstraction plateforme ───────────────────────────────────────────
// Le cœur (données, prédiction) et l'UI n'utilisent que ces fonctions :
// le même code tourne sur l'ESP32-S3 (platform_esp32.cpp) et dans le
// simulateur PC (sim/platform_sim.cpp) qui produit les captures d'écran.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

namespace plat {

void     begin();                    // verrou, rétroéclairage (éteint)
bool     fsBegin(bool format);       // monte le système de fichiers (format = formater si illisible)
uint32_t ms();                       // horloge monotone (ms)
int64_t  now();                      // epoch UTC (s) — 0 tant que l'heure n'est pas synchronisée
bool     timeValid();
void     localTime(int64_t epoch, struct tm * out);   // heure de Luxembourg

void *   psAlloc(size_t n);          // grosse allocation (PSRAM sur l'appareil), mise à zéro
void     psFree(void * p);

bool     fsWrite(const char * path, const void * data, size_t n);
bool     fsRead(const char * path, void * data, size_t n);   // lit exactement n octets
long     fsSize(const char * path);                         // -1 si absent
bool     fsRemove(const char * path);

void     logf(const char * fmt, ...);

void     setBacklight(uint8_t pct);  // 0…100
void     restart();

struct SysInfo {
  uint32_t heapFree, heapMin, psramFree, psramSize;
  uint32_t uptimeS;
  uint32_t fsUsed, fsTotal;
  const char * chip;
};
void     sysInfo(SysInfo & out);

// Verrou du modèle partagé entre la tâche réseau et l'UI
void     modelLock();
void     modelUnlock();

struct LockGuard {
  LockGuard()  { modelLock(); }
  ~LockGuard() { modelUnlock(); }
};

} // namespace plat
