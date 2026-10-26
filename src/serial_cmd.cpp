// ── Commandes série (voir serial_cmd.h) ─────────────────────────────
// L'instantané est pris sous le verrou du modèle (Model::serialize, quelques ms : la même
// copie que la sauvegarde périodique) ; l'émission (~1 min pour ~0,5 Mo à 115 200 bauds) se fait
// dans une tâche de priorité 1, sous la tâche réseau (3) et sans toucher à la boucle UI : une
// écriture série qui attend la liaison cède le processeur, rien d'autre n'attend le vidage.
#ifdef ARDUINO
#include <Arduino.h>
#include <stdlib.h>
#include "serial_cmd.h"
#include "config.h"
#include "core/dump.h"
#include "core/model.h"
#include "core/sync.h"
#include "net/net.h"
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

// ── Synchronisation avec le serveur en ligne (core/sync.h) ───────────
// « sync » la lance (la tâche réseau s'en charge ; adoption = redémarrage), « sync etat » dit la dernière,
// « sync url … », « sync jeton … », « sync boot 0/1 », « sync envoi 0/1 » règlent /vn_sync.cfg.
static void syncCommand(const char * a) {
  char b[200];
  while (*a == ' ') a++;
  if (!*a) {
    if (!vn::g_sync.url[0]) { reply("VNSYNC REFUS aucun serveur : sync url http://<adresse>:8790\n"); return; }
    net::requestSync();
    snprintf(b, sizeof(b), "VNSYNC DEMANDE %s/sync/manifest (resultat : sync etat)\n", vn::g_sync.url);
    reply(b);
  } else if (!strcmp(a, "etat")) {
    char line[160];
    net::Status ns = net::status();
    vn::syncStatusLine(ns.sync, plat::now(), line, sizeof(line));
    snprintf(b, sizeof(b), "VNSYNC ETAT url=%s boot=%d envoi=%d jeton=%s | %s\n", vn::g_sync.url[0] ? vn::g_sync.url : "-",
             vn::g_sync.atBoot, vn::g_sync.push, vn::g_sync.token[0] ? "oui" : "non", line);
    reply(b);
  } else if (!strncmp(a, "url ", 4)) {
    char u[128];
    if (!vn::syncNormalizeUrl(a + 4, u, sizeof(u))) { reply("VNSYNC REFUS adresse http(s) attendue\n"); return; }
    snprintf(vn::g_sync.url, sizeof(vn::g_sync.url), "%s", u);
    snprintf(b, sizeof(b), "VNSYNC OK url=%s (%s)\n", u, vn::syncCfgSave() ? "enregistree" : "ECHEC d'ecriture");
    reply(b);
  } else if (!strcmp(a, "url")) {
    vn::g_sync.url[0] = 0;
    snprintf(b, sizeof(b), "VNSYNC OK aucun serveur (%s)\n", vn::syncCfgSave() ? "enregistre" : "ECHEC d'ecriture");
    reply(b);
  } else if (!strncmp(a, "jeton", 5)) {
    const char * v = a + 5;
    while (*v == ' ') v++;
    snprintf(vn::g_sync.token, sizeof(vn::g_sync.token), "%s", v);
    snprintf(b, sizeof(b), "VNSYNC OK jeton %s (%s)\n", *v ? "enregistre" : "efface", vn::syncCfgSave() ? "ok" : "ECHEC d'ecriture");
    reply(b);
  } else if (!strncmp(a, "boot ", 5) || !strncmp(a, "envoi ", 6)) {
    bool boot = a[0] == 'b';
    bool on = atoi(a + (boot ? 5 : 6)) != 0;
    if (boot) vn::g_sync.atBoot = on; else vn::g_sync.push = on;
    snprintf(b, sizeof(b), "VNSYNC OK %s=%d (%s)\n", boot ? "boot" : "envoi", on ? 1 : 0, vn::syncCfgSave() ? "enregistre" : "ECHEC d'ecriture");
    reply(b);
  } else reply("VNSYNC INCONNU (aide)\n");
}

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
  } else if (!strncmp(c, "sync", 4) && (c[4] == 0 || c[4] == ' ')) {
    syncCommand(c[4] ? c + 5 : "");
  } else if (!strcmp(c, "aide") || !strcmp(c, "help")) {
    reply("VNDUMP AIDE commandes : dump (donnees apprises, lecture seule) | stop | ping | aide\n");
    reply("VNSYNC AIDE sync (maintenant) | sync etat | sync url <adresse> | sync jeton <jeton> | sync boot 0/1 | sync envoi 0/1\n");
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
