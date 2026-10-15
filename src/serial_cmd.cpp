// ── Commandes série (voir serial_cmd.h) ─────────────────────────────
// L'instantané est pris sous le verrou du modèle (Model::serialize, quelques ms : la même
// copie que la sauvegarde périodique) ; l'émission (~1 min pour ~0,5 Mo à 115 200 bauds) se fait
// dans une tâche de priorité 1, sous la tâche réseau (3) et sans toucher à la boucle UI : une
// écriture série qui attend la liaison cède le processeur, rien d'autre n'attend le vidage.
#ifdef ARDUINO
#include <Arduino.h>
#include "serial_cmd.h"
#include "config.h"
#include "core/dump.h"
#include "core/model.h"
#include "platform/platform.h"

namespace serialcmd {

static vn::CmdLine s_cmd;
static volatile bool s_running = false;
static volatile bool s_stop = false;

static void writeLine(const char * s, size_t n) { Serial.write((const uint8_t *)s, n); }   // un seul appel : ligne entière

static void task(void *) {
  static vn::DumpSnap snap;
  static vn::DumpWriter w;
  static char line[vn::DUMP_LINE_MAX + 8];
  bool ok;
  {
    plat::LockGuard g;
    ok = vn::dumpSnapshot(vn::g_model, snap);
  }
  vn::dumpSetup(w, snap, plat::now(), VN_VERSION);
  if (!ok) w.abort("instantane impossible (aucune station ou memoire insuffisante)");
  size_t n;
  while (w.next(line, sizeof(line), n)) {
    if (s_stop && !w.done() && w.phase != vn::DumpWriter::PH_ABORT) {   // la ligne prête est abandonnée
      w.abort("stop demande");
      continue;
    }
    writeLine(line, n);
  }
  vn::dumpFree(snap);
  s_stop = false;
  s_running = false;
  vTaskDelete(nullptr);
}

bool dumping() { return s_running; }

static void reply(const char * s) { writeLine(s, strlen(s)); }

static void run(const char * c) {
  char b[96];
  if (!strcmp(c, "dump")) {
    if (s_running) { reply("VNDUMP OCCUPE vidage deja en cours\n"); return; }
    s_running = true; s_stop = false;
    if (xTaskCreatePinnedToCore(task, "vn_dump", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
      s_running = false;
      reply("VNDUMP ABANDON tache impossible\n");
    }
  } else if (!strcmp(c, "stop")) {
    if (s_running) s_stop = true;
  } else if (!strcmp(c, "ping")) {
    snprintf(b, sizeof(b), "VNDUMP PRET proto=%d version=%s\n", vn::DUMP_PROTO, VN_VERSION);
    reply(b);
  } else if (!strcmp(c, "aide") || !strcmp(c, "help")) {
    reply("VNDUMP AIDE commandes : dump (donnees apprises, lecture seule) | stop | ping | aide\n");
  } else {
    reply("VNDUMP INCONNU commande inconnue (aide)\n");
  }
}

void poll() {
  int budget = 64;                                  // borne le temps passé ici à chaque tour de boucle
  while (budget-- > 0 && Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    const char * cmd = s_cmd.feed((char)c);
    if (cmd) run(cmd);
  }
}

} // namespace serialcmd
#endif
