// ── Commandes sur la liaison série (USB, 115 200 bauds) ──────────────
// « dump » recopie en lecture seule les données apprises (core/dump.h) ; « stop » l'interrompt ;
// « ping » répond « VNDUMP PRET » (l'outil PC vérifie ainsi que le micrologiciel connaît la commande) ;
// « aide » liste les commandes. Rien n'est écrit en flash, rien n'est effacé, la carte ne redémarre pas.
#pragma once

namespace serialcmd {

void poll();          // dans loop() : lit les octets reçus (non bloquant)
bool dumping();       // un vidage est en cours

} // namespace serialcmd
