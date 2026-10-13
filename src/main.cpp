// ── VelohNav Ops — supervision temps réel du parc Vel'OH! ────────────
// Sunton ESP32-4827S043 (ESP32-S3, 480×272 RGB, tactile) · LVGL 8.3
//
//  cœur 1 : boucle LVGL (UI)          cœur 0 : tâche réseau (Wi-Fi, GBFS, météo, flash)
//  Les deux partagent g_model sous un verrou récursif (plat::modelLock).
#ifdef ARDUINO
#include <Arduino.h>
#include "lvgl.h"
#include "config.h"
#include "platform/platform.h"
#include "core/settings.h"
#include "core/model.h"
#include "hal/display.h"
#include "net/net.h"
#include "ui/ui.h"

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n%s %s — flash %u Mo, PSRAM %u Ko\n", VN_NAME, VN_VERSION,
                (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() / 1024));
  if (!psramFound())
    Serial.println("[mem] PSRAM absente : cette carte n'est pas une ESP32-4827S043 N16R8 (8 Mo PSRAM octale requis)");
  plat::begin();
  vn::settingsLoad();
  if (!vn::g_model.init()) Serial.println("[mem] PSRAM insuffisante pour le modèle !");
  lv_init();
  lv_disp_t * disp = hal::displayBegin();
  hal::touchBegin();
  ui::init(disp);
  lv_timer_handler();                               // premier rendu (écran d'accueil)
  plat::setBacklight(vn::effectiveBrightness(0));
  net::begin();
}

void loop() {
  plat::modelLock();
  uint32_t wait = lv_timer_handler();
  plat::modelUnlock();
  delay(wait < 5 ? wait : 5);
}
#endif
