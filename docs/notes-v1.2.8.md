## Ce qui change

L'onglet « Alertes » a été audité de bout en bout, puis relu indépendamment. Trois défauts le
rendaient faux dans le cas même où il sert le plus : quand le flux se fige.

### La liste ne perd plus les ruptures

La liste était plafonnée à 96 et se remplissait **dans l'ordre des stations**, avant tout tri : au-delà,
les alertes suivantes n'existaient pas. Mesuré sur le vrai moteur, flux figé : 32 alertes, puis **174
à quatre heures et cinq minutes** — 142 stations « aucun mouvement » basculant dans la même minute —
et **5 ruptures sur 13 perdues**, le sélecteur annonçant 9 au lieu de 13.

Les compteurs comptent désormais **toutes** les alertes, et la liste garde les **96 plus graves** :
à liste pleine, une alerte plus grave évince la moins grave en place. Le sous-titre dit la troncature
(« 174 alertes, 96 affichées ») plutôt que de la taire.

### Un flux figé est une alerte de réseau, pas cent quarante stations à vérifier

Quand presque aucune station n'a bougé depuis vingt minutes en heures actives, l'onglet affiche **une**
alerte « Aucune station n'a bougé depuis… · flux figé », et les « aucun mouvement » individuels
disparaissent — c'est eux qui satureient la liste et évincaient les ruptures.

### Une station arrêtée n'est plus une rupture

`classify()` donne « vide » à une station qui ne loue plus (zéro vélo louable). Elle s'affichait donc
« **Vide · URGENT** » — et occupait deux des trois lignes visibles avec sa seconde alerte « Location
suspendue ». Dans le flux réel : #23 REIMERWEE, arrêtée depuis le 13 mai, et #33 K. ADENAUER depuis le
24 juin. Une station qui ne loue plus ou ne rend plus est désormais un **arrêt**, une seule ligne, et
au-delà de sept jours sans rapport elle est annoncée « hors service depuis plus d'une semaine ».

### L'âge du relevé est dit, et la coche verte ne ment plus

L'état vide affiche « ✓ Rien à signaler » même sur un relevé périmé. Il dit maintenant « **Relevé
figé** — données vieilles de 42 min », avec une horloge ambre. L'âge pris en compte est le plus vieux
de deux : notre dernier téléchargement **et** le `last_updated` du flux — parce qu'un fournisseur qui
gèle en répondant « 200 » laisse le premier intact.

### Une ligne par station

Trois lignes tiennent à l'écran. Une station qui cumule rupture, suspension et maintenance en occupait
trois. C'est désormais une ligne par station, le motif le plus grave, et un badge « +2 » pour les autres.

### Journal des ruptures

Un anneau de 200 épisodes en mémoire (PSRAM, perdu au redémarrage) retient le début et la fin de chaque
rupture. La fiche station affiche « vide 8 h 03 → 8 h 41 (38 min) · aujourd'hui : 3 ruptures, 1 h 12 au
total » — la seule trace d'une rupture terminée avant qu'on regarde l'écran.

## Ce qui est vérifié

Banc : **1 058 vérifications, 0 échec**, dont six nouvelles sur cet onglet : plafond atteint avec les
19 ruptures conservées, compteurs vrais (somme = total), ordre déterministe sur deux recomputes,
station suspendue qui n'est pas « vide », arrêt de plus de sept jours signalé, flux figé réduit à une
alerte réseau sans les individuelles, et journal avec durée. Mutations vérifiées : sans éviction, sans
garde « renting », sans suppression des « aucun mouvement » ou sans journal, les tests tombent.

**Non couvert** : la charge d'une vraie matinée chargée n'a pas été mesurée (l'anneau de la carte ne
contenait qu'une nuit) ; le seuil « aucun mouvement » de vingt minutes est déduit de l'activité observée
à midi (23 à 24 stations changent toutes les quatre minutes), pas d'une saison ; et le journal ne
survit pas à un redémarrage, ni ne couvre les prévisions (qui clignotent autour de leurs seuils).

## Mise à jour

Aucune migration : aucun format de fichier ne change. Flash par-dessus, sans effacement.
