// ── Démarrage : écran d'étapes, cause du dernier redémarrage, mode sans échec ─
// Le rétroéclairage s'allume dès que la dalle est initialisée : si un problème
// survient ensuite, il est affiché à l'écran (et sur le port série, 115200 bauds)
// au lieu de laisser un écran noir muet.
#pragma once
#include <stdint.h>

namespace boot {

enum Stage : uint8_t {
  S_NONE = 0, S_START, S_DISPLAY, S_STORAGE, S_SETTINGS, S_MEMORY, S_TOUCH, S_UI, S_RENDER, S_NETWORK, S_RUN,
  S_COUNT
};

void begin();                                   // à appeler en tout premier (lit la mémoire RTC)
void stage(Stage s, const char * text);         // étape en cours (série + écran + mémoire RTC)
void screenOpen();                              // écran de démarrage (après hal::displayBegin)
void screenClose();                             // avant ui::init
bool showIncident();                            // affiche la cause d'un redémarrage anormal (≈ 6 s)
bool safeMode();                                // ≥ 3 redémarrages anormaux de suite : réseau coupé
[[noreturn]] void fatal(const char * title, const char * detail);
void tick();                                    // dans loop()
uint32_t bootMs();

} // namespace boot
