# Journal des versions

Les notes de chaque version, de la plus récente à la plus ancienne. La procédure de mise à jour d'une carte
(sauvegarde de la partition de fichiers, flash sans effacement, retour arrière) est dans le
[README](README.md#mise-à-jour-dune-carte-déjà-en-service).

## 1.6.0 — la régulation rejoint `main`, et le camion ne fausse plus l'apprentissage

> **1.5.0 → 1.6.0 : aucune migration sur la carte.** Mêmes fichiers appris. Nouveaux fichiers, absents tant qu'on
> ne s'en sert pas : `/vn_reg.cfg` (rôle), `/vn_regq.bin` (file des déclarations du Sunton régulateur), `/vn_regsnap.json`. Côté serveur,
> `/vn_live.bin` passe en version 3 (les versions 1 et 2 sont relues) ; le registre de régulation est dans
> `<données>/regulation/`.

- **Interventions déclarées, tournées, rôle « régulateur »** (chantiers 1 et 2 de la feuille de route) : le code
  écrit le 27/10 sur la branche `dashboard-online` n'avait jamais rejoint `main`. Il y est. Mode d'emploi :
  [`docs/regulation.md`](docs/regulation.md) ; feuille de route : [`docs/feuille-de-route.md`](docs/feuille-de-route.md).
  Réglages → **Rôle de ce Sunton** → Régulateur : l'onglet « Tournée » fait valider chaque arrêt, garde les
  validations en flash hors connexion et les envoie dès que le réseau revient. Sur le serveur : onglet
  Régulation, page de secours `regulateur.html` pour le téléphone.
- **Mis aux règles posées depuis par `main`** : les routes `/reg/…` sont protégées comme les autres (jeton ; sans
  jeton, écritures fermées hors requête locale ; origine tierce refusée ; corps JSON exigé) ; le Sunton du camion
  n'envoie jamais son jeton en clair (même règle que la synchro : `https`, ou réseau privé) ; les tests de
  régulation entrent dans `npm test` avec leurs planchers.
- **Le camion ne fausse plus l'apprentissage** (la dernière des cinq idées). Une prévision traversée par une
  intervention **déclarée** n'est ni jugée par l'apprentissage du modèle 2, ni comptée dans la note des
  prévisions ; les deux disent combien ils en ont écarté. Jamais déduit du flux : sans déclaration, rien n'est
  écarté. Limites dans `docs/regulation.md` (profils non touchés, déclarations tardives, serveur seulement).
- **À éprouver sur le terrain** — rien de ce chantier n'a tourné sur une vraie carte ni dans un vrai camion :
  transport HTTP de la carte, partage de connexion du téléphone, lisibilité, recoupement sur de vraies
  interventions, qualité du plan (`docs/regulation.md`, dernière section).

## 1.5.0 — la prévision dit son erreur, et quatre vérifications tournent sur le vrai parc

> **1.4.1 → 1.5.0 : aucune migration.** Aucun format de fichier ne change, ni sur la carte ni sur le serveur.
> Les notes de prévision du serveur sont recalculées une fois au démarrage, à partir du journal déjà gardé.

Règle suivie (feuille de route) : une estimation se montre avec son erreur mesurée, et rien de nouveau ne décide de
quoi que ce soit avant d'avoir fait ses preuves sur le parc réel. Une seule chose change donc sur les écrans — une
ligne de plus dans la fiche. Tout le reste se mesure à côté, dans l'onglet Prévisions du serveur.

- **Fiche d'une station : « Erreur mesurée : ± 1,2 vélo à +1 h · ± 1,9 à +2 h (sur 412 prévisions jugées de la
  station) ».** C'est l'erreur absolue moyenne du réglage appliqué, sur les prévisions déjà jugées de la station
  (`Model::fcErrAt`). Station trop peu jugée : l'erreur du parc, dite « sur le parc ». Rien de jugé : « pas encore
  mesurée ». Le « CONFIANCE % » voisin reste ce qu'il est : la maturité du profil, pas une précision.
- **Cette annonce est elle-même vérifiée.** Le serveur compare l'erreur annoncée par chaque fiche à l'erreur
  réellement faite une heure plus tard : « fidèle », « trop optimiste » ou « trop prudente », après 7 journées.
- **Risque de rupture dans l'heure, en probabilité** (chantier 7). Pas une formule : la part des situations
  passées semblables (même relevé, même prévision, même alerte) qui ont fini vides, ou pleines, dans l'heure.
  Chaque journée est annoncée avec les seules journées qui la précèdent, puis comparée à ce qui s'est passé, par
  tranche de probabilité. **La liste en direct n'apparaît que si « les 70 % annoncés se réalisent 70 fois sur
  100 »** ; sinon elle reste masquée et la page dit où en est la vérification. Comme le journal existant contient
  déjà ce qu'il faut, les journées notées depuis la mise en service comptent tout de suite.
- **Selon le type de jour** (chantier 6, premier pas). Avant de créer un type « vacances scolaires » ou
  « événement » : le modèle s'y trompe-t-il plus ? Erreur à +1 h du modèle et de la persistance par type de jour.
  Vacances : calendrier officiel du ministère (2026/2027 et 2027/2028). Événements : déclarés à la main dans
  `calendrier.json`. Aucun nouveau type de jour n'est ajouté au modèle tant que ce tableau ne le justifie pas.
- **Besoin conseillé : deux règles comparées.** Les écrans conseillent de viser 50 % à +1 h. Le serveur note à côté
  « autant de marge avant le vide qu'avant le plein sur les 4 h prévues », et rejoue les deux sur ce qui s'est
  réellement passé ensuite. C'est une **estimation**, présentée comme telle (on ne voit jamais ce qui se serait
  passé avec le camion). La règle affichée ne change pas.
- **Non fait, et pourquoi : écarter les passages du camion de l'apprentissage.** Il faut les interventions
  **déclarées** (les déduire du flux est interdit par la feuille de route), et ce registre n'est pas dans `main` :
  les chantiers 1 et 2 (registre, tournées, rôle « régulateur ») sont restés sur la branche `dashboard-online`,
  11 commits et près de 5 000 lignes écrits avant la revue de sécurité et le modèle 2. Les fusionner est un
  chantier à part ; cette amélioration vient juste après.

## 1.4.1 — ce qui restait de travers en 1.4.0, et la preuve des couples par station

> **1.4.0 → 1.4.1 : aucune migration.** Mêmes fichiers sur la carte (`/vn_prof.bin` v6, `/vn_hist.bin` v2,
> `/vn_days.bin`). Côté serveur, `/vn_live.bin` passe en version 2 ; celui d'une 1.4.0 est relu tel quel.

- **Un redémarrage du serveur ne perd plus 4 h d'apprentissage.** Les prévisions d'apprentissage du modèle 2
  attendent leur échéance jusqu'à 4 h ; elles n'étaient pas dans l'état vivant (`/vn_live.bin`). Un moteur relancé
  — le serveur après une mise à jour, le jumeau qui démarre sur un instantané — les perdait, et ses couples par
  station s'écartaient de ceux du serveur : `replication.test.mjs` était rouge depuis la 1.4.0 (74/75). Elles y
  sont maintenant ; le jumeau calcule de nouveau au bit près. La carte, elle, les perd toujours à un redémarrage
  (au plus 4 h d'apprentissage : choix d'origine, inchangé — les garder coûterait 80 Ko de plus écrits en flash
  toutes les 30 min).
- **Couples par station : mesurés hors échantillon, en continu.** [`docs/mesures-modele.md`](docs/mesures-modele.md)
  le dit : ce levier, livré en 1.4.0, n'avait aucune mesure sur des données qu'il n'avait pas vues. Le serveur note
  désormais chaque prévision deux fois — avec le couple de la station (ce que l'écran affiche) et avec le seul
  couple de sa classe de jour — sur les mêmes relevés. L'onglet Prévisions donne l'écart, son intervalle à 95 %
  (une journée = une grappe) et un verdict **seulement après 7 journées complètes** : « gagne », « perd » ou
  « non concluant ». Rien n'est affiché ni appris avec ce témoin.
- **Le mot de passe Wi-Fi enregistré n'est plus réaffiché.** Toucher le réseau en cours dans la feuille Wi-Fi le
  montrait en clair. Le champ reste vide ; le laisser vide garde le mot de passe enregistré.
- **Le jeton de synchro ne part plus en clair hors du réseau local.** En `https`, ou en `http` vers une adresse
  privée ou un nom local (`192.168.…`, `10.…`, `umbrel`, `….local`, Tailscale…), rien ne change. En `http` vers
  une adresse publique, la carte n'envoie plus rien et l'écran dit pourquoi (« passer l'adresse en https »). Même
  règle pour l'adresse que le serveur annonce : derrière un tunnel, régler `VN_PUBLIC_URL`. Le certificat du
  serveur n'est toujours pas vérifié (voir « Données et limites » du README).
- **Deux tests rouges sur `main` remis au vert, pour de bonnes raisons** : `replication.test.mjs` (ci-dessus), et
  `tools/test_bench_reel.py`, dont un contrôle affirmait encore le défaut de l'ancienne règle (« le profil seul
  bat le modèle ») — faux depuis le modèle 2, donc en échec. Il vérifie maintenant ce qui est vrai : au rejeu, le
  modèle fait mieux que le profil seul et que la persistance, dans le même sens que le banc simulé.
- **Publication** : un tag `vX.Y.Z` crée une Release GitHub avec les deux images (`.github/workflows/release.yml`,
  le tag doit être la version de `src/config.h`). `dist/` peut cesser de grossir.
- Dépôt : numéro de version identique partout (firmware, serveur, moteurs WebAssembly, binaires de `dist/`),
  méthode de prévision du README réécrite pour le modèle 2, ce journal sorti du README.

## 1.4.0 — modèle de prévision 2

> **1.3.0 → 1.4.0 : `/vn_prof.bin` passe en version 6.** Une 1.4.0 relit sans perte le fichier d'une 1.1.0 à
> 1.3.0 (v4) et le réécrit en v6, que **les versions antérieures refusent** : revenir en arrière sans sauvegarde,
> c'est repartir d'un profil vide. Le serveur en ligne doit être en 1.4.0 lui aussi pour accepter ces profils.

- **Règle de prévision** : `prévu(h) = actuel + a(h)·[profil(t+h) − profil(t)] + b(h)·[profil(t) − actuel]`, les
  coefficients `a` et `b` appris en ligne par classe de jour (ouvré | week-end et férié) et par horizon (15, 30,
  60, 120, 240 min), avec oubli (demi-vie 7 jours). Tant qu'une classe n'a pas une journée complète de prévisions
  jugées : la persistance. L'ancienne règle (profil + tendance) faisait moins bien que la persistance de +15 à
  +60 min sur les données réelles ; elle ne sert plus qu'aux échéances « vide / pleine dans X min ».
- **Un couple par station** à +60, +120 et +240 min, rétréci vers celui de sa classe.
- Mesures, candidats réfutés et réserves : [`docs/mesures-modele.md`](docs/mesures-modele.md).

## 1.3.0 et avant

> **1.2.9 → 1.3.0 : aucune migration.** Mêmes formats de fichiers ; la 1.3.0 ajoute seulement
> `/vn_sync.cfg` (adresse du serveur en ligne, en texte), absent tant qu'on ne règle rien.

### 1.3.0 — synchro en ligne : une carte éteinte le week-end repart avec le week-end appris

Réglages → **Synchro en ligne** (ou en série : `sync url http://<machine>:8790`). Au démarrage, avant de
relire son historique, la carte demande au serveur (`online/`) son manifeste et décide fichier par
fichier (`src/core/sync.cpp`) : profils s'il a **strictement plus de relevés**, historique 24 h s'il est
**plus récent** et encore relisible (< 20 h), résumés par jour s'il en a **plus**. Chaque fichier est
vérifié — taille, CRC32, puis les contrôles d'en-tête de `Model::load`, factorisés dans
`Model::profFileOk` / `histFileOk` — **avant** d'être écrit : un téléchargement faux n'efface jamais la
flash. Avec « Partager l'apprentissage », la carte envoie au serveur ses profils et ses jours quand ils
valent mieux que les siens (premier amorçage d'un serveur neuf, sans câble). « Maintenant » fait la même
chose en cours de route et redémarre la carte si un fichier a été adopté. Serveur absent ou injoignable :
la carte démarre avec ce qu'elle a. Testé sans carte : 39 vérifications dans `test_core`, et
`online/test/sync.e2e.mjs` fait dialoguer la vraie synchro du firmware (`sim/sync_sim`) avec le vrai
serveur. La ligne de commande série accepte désormais 160 caractères (au lieu de 24) pour l'adresse.

> **1.1.0 → 1.2.0 : aucune migration.** La 1.2.0 n'ajoute aucun format de fichier (`/vn_hist.bin`
> reste en v2) : elle se flashe par-dessus et retrouve l'historique, les profils et les résumés tels
> quels. C'est la 1.1.0 qui a introduit les formats actuels.

> **1.2.0 → 1.2.1 : aucune migration.** La 1.2.1 ne change que la recherche Wi-Fi (voir plus bas) :
> même flash par-dessus, mêmes fichiers relus à l'identique.

> **1.2.1 → 1.2.2 : aucune migration.** La 1.2.2 ne change qu'un critère de calcul (voir plus bas) :
> mêmes fichiers, mêmes formats.

### 1.2.2 — « vélos en trajet » : une profondeur d'historique RÉELLEMENT observée

Signalé par le propriétaire : « le device était hors tension 12 h ». La validité du chiffre et la
ligne `hist. Xh YY` venaient de l'**écart d'horloge** entre la tranche la plus ancienne et la plus
récente de l'anneau : une carte éteinte douze heures affichait donc douze heures d'historique avec
trois relevés, et pouvait se déclarer validée dessus. Une coupure n'est pas une observation.

Désormais la profondeur est celle des tranches **réellement renseignées et comparables** à la
population courante (× 5 min, trous, coupures et changements de couverture exclus) : validé à
**6 h observées** *et* un creux ; le provisoire garde son seuil, seule la mesure change — aucune
décision produit n'est prise au passage.

Trois autres endroits mentaient de la même façon et sont corrigés :

- **fiche « Variation sur 1 h »** : « Historique disponible : 1440 min » après une coupure, alors que
  la carte n'avait que 145 tranches — elle affiche maintenant la profondeur observée ;
- **tuile d'activité du tableau de bord** : « 1 h : 0 retraits · 0 retours » dès le premier relevé
  après une coupure, parce que le critère comptait des lignes d'anneau au lieu de tranches
  renseignées — il en faut désormais douze pleines ;
- **quart « calme »** : il se mesure sur les mêmes tranches que le parc (couverture égale), sinon un
  moment jugé calme pour trois stations pouvait servir à valider un parc calculé sur six.

Un **recul d'horloge** (resynchronisation NTP vers l'arrière) n'inscrit plus deux fois la même
tranche, ce qui gonflait la profondeur. Et la tranche de référence est **datée de son âge** : la
fiche écrit « Réf. 00:30 (−15 h) », y compris quand la référence reste dans la même journée.

### 1.2.7 — le parc tient compte de deux artefacts, et le chiffre dit d'où il vient

Le parc était la troisième plus grande valeur comparable : ce budget ne tolérait que **deux tranches
aberrantes sur 24 h, toutes causes confondues**. Une revue l'a mesuré : deux artefacts indépendants de
deux tranches (une station fantôme le matin, un cache le soir) faisaient annoncer **400 vélos en trajet
pour 200 réels, validé, pendant vingt-quatre heures**. La valeur retenue passe au **sixième** rang :
elle absorbe exactement cinq tranches aberrantes — deux artefacts indépendants, ou une station fantôme
d'un quart d'heure. Le test (l) fige le cas ; (l') fige la borne exacte (cinq tranches passent, six
dépassent) et tombe à K = 5 comme à K = 3.

**Le quart calme était mal mesuré.** L'activité de la référence se comparait aux seules tranches **plus
anciennes** qu'elle (et le rattrapage comparait à −1 : code mort). Une référence prise en pleine
vidange au démarrage passait donc pour un creux, et −40 % s'affichaient comme **validés** le premier
jour. Le comptage porte désormais sur toutes les tranches comparables, avant comme après la référence ;
la couverture est aussi bornée par le haut (une tranche plus couverte ne compte plus comme calme). Le
test (m) le tient : référence en vidange, 108 tranches plus calmes après → **provisoire**, plus validé.

**Le chiffre porte sa provenance.** La tuile écrit « parc ≈ 856 · réf. −15 h » (ou « réf. courante »),
et la fiche donne l'heure exacte, la profondeur d'historique et la couverture. Le parc reste une
**estimation** — la fiche dit les deux sens d'erreur — et le clamp « parc ≥ vélos garés » tolère
désormais ±1 % (ou ±5 vélos), ce qui supprime les bascules dues au seul bruit de lecture de la nuit
(54 bascules sur 480 relevés à ±6, mesurées par la revue).

**Trois défauts de la 1.2.7 corrigés après revue** (revue K7, `RAPPORT-K7.md`) : les fiches
contenaient encore des chaînes avalées par le compilateur (« rßérence », « Ürite ») — la séquence
`\xHH` suivie d'un chiffre hexadécimal déborde (`\xA9f` = `0xA9F` tronqué à `0x9F`) ; un test vérifie
désormais qu'aucune fiche ne contient les octets de cette famille, et que le détecteur les voit. La
provenance de la tuile était **tronquée** (« parc ≈ 880 ·… ») : le label fait **80 px** à la police
`vn_inter_11` et « parc ≈ 1 419 · −20h » en demande 99 (mesuré, crénage compris) ; la tuile écrit donc
« ≈ 1 419 · −20h » (74 px), et « parc ≈ 880 » quand la référence est le relevé courant. Et l'invariant « parc ≥ vélos
garés » ne tient plus **toujours** : il était relâché de la tolérance de bruit ; seule la *référence*
est jetée au-delà du bruit, le niveau est corrigé dans tous les cas.

Coût documenté : sur une nuit **en rampe** sans palier de trente minutes, un biais permanent de quelques
vélos à une quinzaine selon la pente ; sur une nuit avec palier, il est nul. Sur une vidange qui démarre
à froid, le biais de pente (−5 rangs) dure environ vingt heures, jusqu'au premier palier — et il est
désormais **provisoire**, donc affiché comme « — » et non comme une mesure.

### 1.2.6 — « vélos en trajet » reste une estimation, et le dit

Le parc estimé est une valeur **prudente** (la troisième plus grande valeur comparable, pour ignorer
les relevés aberrants). On avait d'abord écrit le nombre de vélos en circulation comme un **plancher**
(« ≥ 156 ») ; une revue l'a réfuté avec des chiffres : **deux artefacts indépendants** surestiment
encore le parc (« ≥ 400 » annoncés pour 200 réels). Un plancher qui peut être faux est pire qu'une
estimation assumée : la tuile et la fiche disent donc une **estimation**, et la règle écrite explique
pourquoi (« une surestimation du parc la fait monter, une sous-estimation la fait descendre »).

Deux autres restes de la revue sont corrigés :

- le badge « AUCUN RELEVÉ » ne s'affiche plus chaque nuit avant 8 h (fausse alerte : aucune heure de
  service n'est encore complète) ;
- la courbe 30 jours applique la **même définition de « jour relevé »** que le cœur (une heure
  pleinement relevée), et le classement compte aussi les stations inactives parmi les écartées.

Les chaînes corrompues sont éliminées partout (`\xHH` suivi d'un chiffre hexadécimal), y compris
« rßérence », qui subsistait dans la règle de validité et produisait un avertissement à chaque build.

### 1.2.5 — l'onglet Service ne promet plus ce qu'il compte

Sept défauts de chiffres et de libellés, relevés par une revue indépendante qui a confronté
`docs/vue-service.md` au code et pratiqué 26 mutations sur le banc :

- **le volet « La carte » se rafraîchit tout seul** : hors ligne, il restait figé sur « reçu à
  l'instant · FRAIS · OK », c'est-à-dire le panneau chargé de prouver la fraîcheur qui mentait ;
- **« COMPLET » ne s'affiche plus quand aucun relevé n'a eu lieu aujourd'hui** (dénominateur nul) ;
- **les comparaisons de ruptures disent le nombre de jours de chaque côté** (« 237 (7 j) contre 212
  (6 j) ») et « non relevé » remplace un zéro inventé quand le jour de référence manque ;
- **la courbe « 30 j » couvre exactement la même fenêtre que la tuile** (30 jours, aujourd'hui exclu) :
  elle en additionnait 29, avec une fenêtre différente de la tuile voisine ;
- **un jour relevé seulement hors 7 h–22 h n'est plus compté comme un jour relevé**, et n'apparaît
  plus comme un zéro sur la courbe (c'est un trou) ;
- **« % des tranches » précise qu'il porte sur les jours relevés** ;
- **« 6 week-ends/fériés sur 6 » devient « 6 jours de week-end/férié sur 6 »** — c'étaient des jours,
  pas des week-ends.

Deux précisions de sens : la « série » d'un problème récurrent s'écrit maintenant « série 12 j
relevés » (elle peut traverser des jours sans données), et le tableau des états dit « vide = aucun
vélo louable » au lieu de promettre qu'on peut rendre — le cas « 0 vélo et 0 borne libre » existe.
Le classement affiche aussi le nombre de stations écartées.

Cinq règles qui n'étaient pas vraiment testées le sont désormais : « ≥ 3 tranches » pour un état
touché, « ≥ 6 tranches » pour un créneau relevé, la tranche de 6 h 55 qui sert de précédente à 7 h, un
jour relevé seulement la nuit qui n'en est pas un, et une station observée moins de la moitié du temps
qui est écartée du classement avec son compte affiché.

Deux corrections issues de la revue de ces correctifs : l'axe de la courbe écrit « hier » sous sa
dernière barre (il disait « auj. »), et la ligne d'un problème récurrent reste lisible — abrégée en
« j », elle ne tronque plus la série. La carte mémoire ne se rafraîchit qu'une fois toutes les 30 s :
`sysInfo` interroge LittleFS, et le minuteur de fraîcheur l'aurait sollicité soixante fois par minute
sous le verrou du modèle.

### 1.2.4 — parc lu à la troisième plus grande valeur, et fiches lisibles

Trois corrections issues d'une revue indépendante, avec des cas chiffrés :

- **la couverture est comparée dans les deux sens** : une tranche qui couvrait **plus** de stations
  qu'aujourd'hui était jugée comparable. Trois stations retirées du référentiel faisaient annoncer
  « 94 vélos en trajet, validés » pour une vraie valeur de 49 ;
- **le parc est la troisième plus grande valeur comparable**, au lieu du maximum : un relevé unique ou
  double anormalement haut (redémarrage, valeur en cache, station fantôme) est écarté et **compté**
  dans la fiche (« 2 relevés plus hauts écartés »), au lieu de gonfler le chiffre — et sans refuser la
  mesure pendant les ~23 h où cet artefact reste dans l'anneau ;
- **les fiches ne sont plus corrompues** : la séquence d'échappement `\xA9` du mot « Réf. » avalait la
  lettre suivante, et le firmware livré affichait « Rß. 03:05 » et « RÞLLEMENT OBSERVÞS ». Corrigé
  dans toutes les chaînes concernées, avec un test qui compare le texte rendu.

Les tests ont été refaits là où ils ne mordaient pas : le contrôle « pic isolé » restait vert même en
retirant la fonctionnalité (le garde-fou « calme » invalidait tout seul), et deux scénarios
dépendaient du jour de la semaine ou d'une série sans mouvement. Ils s'appuient maintenant sur une
station fantôme à deux tranches et sur une couverture réduite.

### 1.2.3 — un maximum isolé n'est pas un parc

Le parc estimé est le plus haut relevé de la fenêtre. Un **relevé unique** plus haut que tous les
autres — redémarrage, valeur en cache, station fantôme — suffisait donc à fabriquer un parc trop
grand, donc des « vélos en trajet » inventés. La validation exige désormais un **palier** : au moins
deux tranches comparables confirment le maximum à 1 % près (deux vélos au minimum). La fiche nomme le
cas : « max isolé ».

*Pourquoi pas une comparaison « maximum 24 h contre maximum 6 h » ?* Parce que la baisse du parc en
journée est **normale** : les vélos sont dehors, pas disparus. Ce critère marquerait la tuile
provisoire chaque après-midi, pour une raison qui n'est pas une erreur de mesure. Quatre tests de non-régression figent ces cas : coupure
(trois relevés espacés de 6 h → ni validé ni provisoire, 15 min d'historique), couverture inégale,
recul d'horloge, et le seuil du provisoire inchangé.

### 1.2.1 — la recherche Wi-Fi fonctionne aussi sans réseau

Le pilote refuse tout balayage **pendant qu'une association est en cours**, et la carte, quand son
réseau enregistré est injoignable, passait son temps à s'associer : la recherche échouait alors
systématiquement, si bien qu'on ne pouvait plus découvrir — donc plus choisir — un autre réseau.
La recherche **suspend maintenant l'association le temps du balayage** puis la relance, et un refus
du pilote est réessayé au lieu d'être pris pour un échec définitif. Deux corrections liées :

- le réseau saisi à l'écran n'est **enregistré qu'une fois l'association réussie** : une faute de
  frappe ne devient plus le réseau du démarrage ;
- **plus de plantage à la validation du mot de passe** : la feuille et son clavier étaient détruits
  par deux suppressions différées, dans un ordre qui laissait le clavier écrire dans un objet déjà
  libéré — l'écran redémarrait avant toute tentative d'association, ce qui interdisait de changer de
  réseau ;
- le journal série ne dit plus « déconnexion … délai dépassé » quand la carte coupe elle-même pour
  changer de réseau ;
- le journal série dit ce qui s'est passé : `association suspendue`, `réseau enregistré : « X »`,
  `réseau « X » NON enregistré : l'ancien réseau reste celui du démarrage`.

Recette faite sur la carte : recherche associée (5 réseaux), changement pour « Bitcoin » avec
enregistrement après succès, mot de passe refusé (code 15) puis **recherche non associée réussie**
(`5 bornes, 4 réseaux listés … association suspendue puis reprise`), et retour au réseau enregistré.
