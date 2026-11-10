// ── VelohNav Ops — supervision temps réel du parc Vel'OH! ────────────
// Sunton ESP32-4827S043 (ESP32-S3, 480×272 RGB, tactile) · LVGL 8.3
//
//  cœur 1 : boucle LVGL (UI)          cœur 0 : tâche réseau (Wi-Fi, GBFS, météo, flash)
//  Les deux partagent g_model sous un verrou récursif (plat::modelLock).
//
//  Le rétroéclairage (GPIO 2, actif à l'état haut) s'allume avant toute init.
//  Un échec de la dalle ne doit plus laisser l'écran noir : l'ancien code le
//  gardait éteint, puis plantait dans ESP_ERROR_CHECK si la PSRAM ou le tampon
//  de rebond refusait l'allocation.
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "lvgl.h"
#include "config.h"
#include "boot.h"
#include "platform/platform.h"
#include "core/settings.h"
#include "core/sync.h"
#include "core/regul.h"
#include "core/model.h"
#include "hal/board.h"
#include "hal/display.h"
#include "net/net.h"
#include "ui/ui.h"
#include "serial_cmd.h"

// Pile de la boucle UI : ≈ 4 Ko mesurés (rendu + mises en page LVGL), 16 Ko par sécurité
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  pinMode(VN_PIN_BACKLIGHT, OUTPUT);
  digitalWrite(VN_PIN_BACKLIGHT, HIGH);
  Serial.begin(115200);
  delay(20);
  boot::begin();
  Serial.printf("[boot] flash %u Mo \xC2\xB7 PSRAM %u Ko \xC2\xB7 RAM interne libre %u Ko\n",
                (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  plat::begin();

#ifndef VN_QEMU
  if (!psramFound()) {
    Serial.println("[boot] PSRAM absente. N16R8 : env esp32-4827s043c (OPI). Carte 8 Mo : env esp32-4827s043c-qspi.");
    plat::setBacklight(100);
    for (;;) delay(1000);
  }
#endif

  boot::stage(boot::S_DISPLAY, "Initialisation de l'\xC3\xA9" "cran");
  lv_init();
  lv_disp_t * disp = hal::displayBegin();
  if (!disp) {
    Serial.println("[boot] dalle RGB non démarrée, rétroéclairage laissé allumé");
    plat::setBacklight(100);
    for (;;) delay(1000);
  }
  boot::screenOpen();
  plat::setBacklight(100);
  boot::showIncident();
  boot::stage(boot::S_STORAGE, "Ouverture du stockage");
  if (!plat::fsBegin(false)) {
    boot::stage(boot::S_STORAGE, "Premier d\xC3\xA9marrage : pr\xC3\xA9paration de la m\xC3\xA9moire (jusqu'\xC3\xA0 30 s)");
    plat::fsBegin(true);
  }
  boot::stage(boot::S_SETTINGS, "Lecture des r\xC3\xA9glages");
  vn::settingsLoad();
  vn::syncCfgLoad();                                // serveur en ligne (/vn_sync.cfg), facultatif
  vn::regCfgLoad();                                 // rôle : poste de supervision ou Sunton du camion (/vn_reg.cfg)
  if (vn::g_regCfg.role == vn::ROLE_REGUL) {
    // Gestes pas encore envoyés et dernier état reçu : un redémarrage (contact coupé) ne perd rien
    bool q = vn::regQueueLoad(vn::g_reg), s = vn::regSnapLoad(vn::g_reg);
    Serial.printf("[regul] r\xC3\xB4le r\xC3\xA9gulateur \xC2\xB7 %u geste(s) en file%s \xC2\xB7 %s\n", (unsigned)vn::g_reg.nq, q ? "" : " (aucune file)",
                  s ? "dernier \xC3\xA9tat relu" : "aucun \xC3\xA9tat gard\xC3\xA9");
  }
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
  serialcmd::poll();                                // « dump » : données apprises vers le PC (lecture seule)
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
