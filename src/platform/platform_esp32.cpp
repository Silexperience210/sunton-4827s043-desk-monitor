// ── Plateforme ESP32-S3 (Arduino) ────────────────────────────────────
#ifdef ARDUINO
#include "platform.h"
#include "vn_lv_hooks.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>

#ifndef VN_PIN_BACKLIGHT
#define VN_PIN_BACKLIGHT 2
#endif
#define VN_BL_CHANNEL 0

static SemaphoreHandle_t s_lock = nullptr;
static bool s_fsOk = false;

// ── Crochets LVGL : objets en PSRAM (la RAM interne reste au Wi-Fi / TLS) ──
extern "C" {
void * vn_lv_malloc(size_t n) {
  void * p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
void vn_lv_free(void * p) { heap_caps_free(p); }
void * vn_lv_realloc(void * p, size_t n) {
  void * q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : realloc(p, n);
}
uint32_t vn_lv_tick(void) { return millis(); }
}

namespace plat {

void begin() {
  s_lock = xSemaphoreCreateRecursiveMutex();
#if defined(VN_QEMU)
  pinMode(VN_PIN_BACKLIGHT, OUTPUT);      // l'émulateur n'a pas de périphérique LEDC
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(VN_PIN_BACKLIGHT, 5000, 8);
#else
  ledcSetup(VN_BL_CHANNEL, 5000, 8);
  ledcAttachPin(VN_PIN_BACKLIGHT, VN_BL_CHANNEL);
#endif
  setBacklight(0);
}

bool fsBegin(bool format) {
  s_fsOk = LittleFS.begin(format);        // format : premier démarrage (partition vierge)
  if (!s_fsOk && format) logf("[fs] LittleFS indisponible");
  return s_fsOk;
}

uint32_t ms() { return millis(); }

int64_t now() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (int64_t)t : 0;
}
bool timeValid() { return now() != 0; }

void localTime(int64_t epoch, struct tm * out) {
  time_t t = (time_t)epoch;
  localtime_r(&t, out);
}

void * psAlloc(size_t n) {
  void * p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}
void psFree(void * p) { heap_caps_free(p); }

bool fsWrite(const char * path, const void * data, size_t n) {
  if (!s_fsOk) return false;
  // Écriture atomique : fichier temporaire puis renommage
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t w = f.write((const uint8_t *)data, n);
  f.close();
  if (w != n) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

bool fsRead(const char * path, void * data, size_t n) {
  if (!s_fsOk) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t r = f.read((uint8_t *)data, n);
  f.close();
  return r == n;
}

long fsSize(const char * path) {
  if (!s_fsOk || !LittleFS.exists(path)) return -1;
  File f = LittleFS.open(path, "r");
  if (!f) return -1;
  long s = (long)f.size();
  f.close();
  return s;
}

bool fsRemove(const char * path) { return s_fsOk && LittleFS.exists(path) && LittleFS.remove(path); }

void logf(const char * fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.println(b);
}

void setBacklight(uint8_t pct) {
  if (pct > 100) pct = 100;
  uint32_t duty = (uint32_t)pct * 255 / 100;
#if defined(VN_QEMU)
  digitalWrite(VN_PIN_BACKLIGHT, duty ? HIGH : LOW);
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(VN_PIN_BACKLIGHT, duty);
#else
  ledcWrite(VN_BL_CHANNEL, duty);
#endif
}

void restart() {
  delay(300);
  ESP.restart();
}

void sysInfo(SysInfo & o) {
  o.heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  o.heapMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  o.psramFree = ESP.getFreePsram();
  o.psramSize = ESP.getPsramSize();
  o.uptimeS = millis() / 1000;
  o.fsUsed = s_fsOk ? LittleFS.usedBytes() : 0;
  o.fsTotal = s_fsOk ? LittleFS.totalBytes() : 0;
  o.chip = "ESP32-S3";
}

void modelLock() { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void modelUnlock() { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

} // namespace plat
#endif // ARDUINO
