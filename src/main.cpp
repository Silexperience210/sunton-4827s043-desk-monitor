// ── VelohNav Ops — supervision temps réel du parc Vel'OH! ────────────
// Sunton ESP32-4827S043 (ESP32-S3, 480×272 RGB, tactile) · LVGL 8.3
//
//  cœur 1 : boucle LVGL (UI)          cœur 0 : tâche réseau (Wi-Fi, GBFS, météo, flash)
//  Les deux partagent g_model sous un verrou récursif (plat::modelLock).
//
//  Le rétroéclairage s'allume dès que la dalle est prête ; chaque étape du démarrage
//  s'affiche (et part sur le port série, 115200 bauds) : jamais d'écran noir muet.
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "lvgl.h"
#include "config.h"
#include "boot.h"
#include "platform/platform.h"
#include "core/settings.h"
#include "core/model.h"
#include "hal/board.h"
#include "hal/display.h"
#include "net/net.h"
#include "ui/ui.h"

// Pile de la boucle UI : ≈ 4 Ko mesurés (rendu + mises en page LVGL), 16 Ko par sécurité
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  pinMode(VN_PIN_BACKLIGHT, OUTPUT);
  digitalWrite(VN_PIN_BACKLIGHT, LOW);              // éteint tant que l'image n'est pas prête
  Serial.begin(115200);
  delay(20);
  boot::begin();
  Serial.printf("[boot] flash %u Mo \xC2\xB7 PSRAM %u Ko \xC2\xB7 RAM interne libre %u Ko\n",
                (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  plat::begin();                                    // verrou + rétroéclairage PWM (encore éteint)

  boot::stage(boot::S_DISPLAY, "Initialisation de l'\xC3\xA9" "cran");
  lv_init();
  lv_disp_t * disp = hal::displayBegin();
  boot::screenOpen();
  plat::setBacklight(70);                           // l'écran s'allume ici
  boot::showIncident();                             // cause d'un redémarrage anormal, s'il y en a eu un
  if (!psramFound())
    boot::fatal("PSRAM introuvable", "Carte attendue : ESP32-4827S043 N16R8 (8 Mo de PSRAM)");

  boot::stage(boot::S_STORAGE, "Ouverture du stockage");
  if (!plat::fsBegin(false)) {
    boot::stage(boot::S_STORAGE, "Premier d\xC3\xA9marrage : pr\xC3\xA9paration de la m\xC3\xA9moire (jusqu'\xC3\xA0 30 s)");
    plat::fsBegin(true);
  }
  boot::stage(boot::S_SETTINGS, "Lecture des r\xC3\xA9glages");
  vn::settingsLoad();
  plat::setBacklight(vn::effectiveBrightness(0));

  boot::stage(boot::S_MEMORY, "R\xC3\xA9servation de la m\xC3\xA9moire");
  if (!vn::g_model.init())
    boot::fatal("M\xC3\xA9moire insuffisante", "La PSRAM ne permet pas d'allouer l'historique des stations");

  boot::stage(boot::S_TOUCH, "Initialisation du tactile");
  hal::touchBegin();

  boot::stage(boot::S_UI, "Construction de l'interface");
  boot::screenClose();
  ui::init(disp);
  boot::stage(boot::S_RENDER, "Premier affichage");
  lv_timer_handler();

  if (boot::safeMode()) {
    Serial.println("[boot] mode sans \xC3\xA9" "chec : r\xC3\xA9seau d\xC3\xA9sactiv\xC3\xA9 (d\xC3\xA9" "brancher/rebrancher pour en sortir)");
  } else {
    boot::stage(boot::S_NETWORK, "D\xC3\xA9marrage du r\xC3\xA9seau");
    net::begin();
  }
  boot::stage(boot::S_RUN, "Pr\xC3\xAAt");
  Serial.printf("[boot] pr\xC3\xAAt en %u ms\n", (unsigned)boot::bootMs());
  enableLoopWDT();                                  // une boucle UI figée > 5 s redémarre la carte
}

void loop() {
  plat::modelLock();
  uint32_t wait = lv_timer_handler();
  plat::modelUnlock();
  boot::tick();
#ifdef VN_QEMU
  static uint32_t t = 0;
  if (millis() - t > 5000) {
    t = millis();
    Serial.printf("[qemu] pile UI libre (min) %u o \xC2\xB7 RAM interne libre %u o \xC2\xB7 PSRAM libre %u o\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getFreePsram());
  }
#endif
  delay(wait < 5 ? wait : 5);
}
#endif
