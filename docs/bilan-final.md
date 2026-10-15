# Bilan final — `feat/final` (candidat à `main`, version 1.1.0)

Branche partie de `feat/bench-reel` (`a1c69d9`, le sommet des cinq missions), avec `feat/vue-service`
complété (`4455590`) et `fix/hors-ligne` (v1.0.9) fusionné (`540f4a9`). Rien n'est fusionné sur `main`,
rien n'est tagué, publié ni flashé.

Deux temps : **corriger** ce que la revue indépendante (`rapport-review-finale.md`) accusait, puis
**resserrer** (retirer ce qui n'a jamais gagné sa place). Chaque commit a passé la boucle verte :
`test_core` sans échec, scénarios `ops`, `hors-ligne` et `service` du simulateur à 0, `pio run` en `c` et
en `r` sans avertissement (builds propres, `-t clean`).

## 1. Les accusations de la revue, instruites une par une

| # | gravité | accusation | verdict | commit | preuve |
|---|---|---|---|---|---|
| B1 | bloquant | le correctif hors ligne v1.0.9 n'est pas dans la pile | **corrigé** | `540f4a9` | fusion, conflits du simulateur résolus à la main ; `sim … hors-ligne` 0 écart, `test_core` 953/0 à la fusion |
| B2 | bloquant | `VN_VERSION` vaut « 1.0.8 », puis « 1.0.9 », nom d'un autre micrologiciel | **corrigé** | `55236d9` | `VN_VERSION "1.1.0"` ; test `testVersion` : avant 955/1, après 956/0 ; `ping` du vidage simulé → `version=1.1.0` |
| B3 | bloquant (procédure) | le retour à une 1.0.x perd les profils | **corrigé côté outil** (le code n'est pas en cause) | `5ef25bb` | `tools/sauvegarde_littlefs.py` (lecture seule, contrôle du superbloc, refuse d'écraser) ; `test_sauvegarde_littlefs.py` 11/0 ; README « Mise à jour d'une carte déjà en service ». **La lecture réelle par esptool reste à faire sur la carte.** |
| M1 | majeur | « monde nul : 0 jour corrigé » est faux sur des graines neuves | **fondé, document corrigé** | `f3162e0` | rejoué : graines 1–6 → 0/6 ; graines 7–30 → **8/24 touchées, 3,8 % des jours en moyenne, 32,1 % au pire** ; α final = 0 partout ; effet sur l'erreur +0,0001. Chiffres identiques à ceux de la revue. Le firmware ne change pas (un seuil plus dur serait un nouveau levier à mesurer : proposition § 3) |
| M2 | majeur | la CI ne joue ni `ops`, ni `service`, ni les deux tests Python ; un scénario inconnu sort en 0 | **corrigé** | `bdd1609` | `build.yml` : `sim ops`, `sim service`, `test_carte_dump.py`, `test_bench_reel.py`, `test_sauvegarde_littlefs.py`, scénario inconnu → code 64. **Non vérifié sur GitHub Actions** (rien n'est poussé) |
| M3 | majeur | quatre protections absentes (PM6, PM3b, AC4, BR2) | **corrigé** | `51d8cc2` | les quatre mutations survivaient, elles sont tuées. AC4 (« levier 1a allumé par défaut ») n'a plus d'objet depuis le retrait de 1a (`748750f`) : son test est parti avec le levier |
| m1 | mineur | `/vn_days.bin` abîmé → débordement d'entier dans `civilDay` | **corrigé** | `6e7ac21` | UBSan : 2 « runtime error » avant, 0 après ; `testDaysBadTimes` : avant 967/1, après 968/0. Les fichiers n'ont toujours **pas de somme de contrôle** (non fait : changement de format sans gain mesuré) |
| m2 | mineur | deux chiffres périmés (« −1,3 % » des fériés ; tableau « fuite ») | **fondé pour la fuite, à moitié pour le −1,3 %** | `0c79177` | fuite rejouée : 0,9877 / 1,0653, profil seul 0,9564 / 1,0955, **7,3 %** (pas 10 %). Fériés rejoués sur les trois mondes : dimanche −25,8 %, **mi-chemin −1,2 %**, ouvré **+20,1 %**. Le « −1,3 % » n'était pas inventé, c'était le monde mi-chemin, présenté comme le monde dimanche. La conclusion (exclure perd) tient dans les trois mondes |
| m3 | mineur | +20 Ko de RAM interne statique non annoncés | **corrigé (14 Ko sur 20)** | `1e6ab14` | `Model::load::tmp` (6 432 o) vérifié sur place ; `svcRank::acc` et `refreshRank::r` (2 × 4 000 o) en PSRAM le temps de l'appel. RAM statique `c` 92 844 → 78 412 o ; RAM interne libre (QEMU) 182 060 → 195 660 o |
| m4 | mineur | deux commits de `feat/vue-service` absents de `feat/final` | **corrigé** | `4455590` | fusionnés (`c66b216`, `5cc2ce4`) |
| m5 | mineur | libellés : « flash utilisée 420 ko » sans dénominateur ; « météo apprise : −2 vélos » sans « sur le parc » ; capture 40 incohérente | **non corrigé : ce sont des écrans** | — | propositions 2, 3 et 4 du § 3. Cause de la capture 40 trouvée : l'en-tête garde la dernière météo valide. Le scénario injecte un état que la carte ne connaît pas (météo « jamais reçue » après avoir été reçue). Sur la carte, le cas réel est une météo **périmée**, et l'en-tête affiche alors une température ancienne sans dire son âge |
| m6 | mineur | une station fermée tout le mois occupe la tête des « 5 plus souvent indisponibles » | **non corrigé : choix d'écran** | — | proposition 5 du § 3 |

Commits de la reprise : `1e6ab14` (m3), `f3162e0` (M1), `0c79177` (m2), puis le resserrage. B1, B2, B3, M2,
M3, m1 et m4 viennent de la tentative interrompue ; leurs preuves sont dans les messages de commit et ont été
revérifiées par la boucle verte de chaque commit suivant.

## 2. Resserrage : ce qui est parti

| commit | retiré | pourquoi c'était du poids mort | « la même chose fonctionne sans » |
|---|---|---|---|
| `748750f` | alerte « inhabituel » : leviers **1a** (tolérance = k × dispersion), **1b** (inertie), **témoin**, **fériés (c)** — `AlertOpts`/`g_alertOpt`, `AN_PENDING`, `AnomTolSrc`, `anomRun*`, `freshReading`, phrases « seuil ± T (k × …) » et « À confirmer », 5 clés du banc, `tools/bench_balayage.py`, captures 44–47 | critère non atteint, rien d'activé sur la carte ; chemins inatteignables avec le réglage livré | verdict et tolérance = formule fixe, inchangés ; phrases de la fiche identiques |
| `14cc5c3` | **statistiques météo par station** (`Model::wxS`, étape 3b) — `/vn_prof.bin` **v4** | apprises toutes les 30 min, sauvées et relues, jamais lues (3b : écart exactement nul) | v1, v2, v3 relus ; le bloc par station des v2/v3 est sauté ; α et météo du parc identiques (test) ; mutation « bloc v3 non sauté » tuée (4 échecs) |
| `ede8e97` | **rajeunissement ciblé** (`g_pred.rejuvN`) et **« férié exclu »** (`HL_SKIP`, 5a-i) | non livrés, perdants mesurés (rejoué aujourd'hui : exclure perd de +0,4 / +3,8 / +7,8 %) | branches jamais prises avec le réglage livré ; le banc refuse désormais ces clés au lieu de retomber en silence sur un autre réglage |
| `c53fea7` | code mort : `net::requestSave()` (jamais appelée : son drapeau était toujours faux), `svcRateText()` (aucun écran ne l'appelle), `Model::wxXbar` et `Model::infoAt` (écrits, jamais lus), `VN_SCREEN_W/H` | aucun lecteur dans `src/` | sauvegarde périodique inchangée ; les tests de `svcRateText` portent sur `svcPct`/`svcHours`, les fonctions que l'écran utilise vraiment |

**Gardé, parce que mesuré gagnant** : calcul « en trajet », prévision (3a, 4b), fraîcheur, provenance, vue
Service, calendrier des fériés et profil du dimanche (5a-ii), mémoire N = 24. Les interrupteurs
`flux_meteo`, `prevue_meteo`, `ferie_dimanche`, `ferie_appris` (0/1) et `profil_n` restent : ils servent de
référence v1.0.8 au banc.

**Gardé, parce que visible à l'écran** : la dispersion apprise par créneau, affichée « pour information » sur
la fiche (proposition 1).

`sim/dump_sim` était déjà ignoré (`5cc2ce4`). `git status --ignored` ne montre que des produits de build
(`sim/sim`, `sim/test_core`, `sim/bench_prevision`, `sim/dump_sim`, `sim/build/`, `tools/__pycache__/`).

### Avant / après

« Avant » = `6e7ac21` (fin des corrections de la tentative précédente) ; « après » = `c53fea7`. Builds propres
(`pio run -t clean`), QEMU = même émulateur que la CI (`tools/qemu_boot_test.py`, 40 s).

| mesure | avant `6e7ac21` | m3 `1e6ab14` | alerte `748750f` | 3b `14cc5c3` | prévision `ede8e97` | **après `c53fea7`** | écart |
|---|---|---|---|---|---|---|---|
| flash utilisée `c` (o) | 1 490 353 | 1 490 509 | 1 488 569 | 1 488 213 | 1 488 049 | **1 487 973** | **−2 380** |
| flash utilisée `r` (o) | 1 477 713 | 1 477 857 | 1 475 901 | 1 475 637 | 1 475 433 | **1 475 369** | **−2 344** |
| `firmware.bin` `c` / `r` (o) | 1 490 720 / 1 478 080 | | | | | **1 488 336 / 1 475 728** | −2 384 / −2 352 |
| RAM statique `c` (o) | 92 844 | 78 412 | 78 388 | 78 380 | 78 364 | **78 348** | **−14 496** |
| RAM statique `r` (o) | 92 612 | 78 180 | 78 140 | 78 132 | 78 132 | **78 116** | **−14 496** |
| RAM interne libre au démarrage (QEMU) | 182 060 | 195 660 | 195 684 | 195 692 | 195 692 | **195 708** | **+13 648** |
| PSRAM libre au démarrage (QEMU) | 2 492 647 | 2 492 647 | 2 495 847 | 2 502 263 | 2 502 251 | **2 502 263** | **+9 616** |
| PSRAM réservée (4 093 Ko − libre) | 1 698 585 | 1 698 585 | 1 695 385 | 1 688 969 | 1 688 981 | **1 688 969** | **−9 616** |
| `/vn_prof.bin`, 148 stations (o) | 405 864 | | | 401 128 | | **401 128** | −4 736 à chaque sauvegarde |
| avertissements `c` / `r` | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | **0 / 0** | |
| vérifications `test_core` | 968 | 968 | 949 | 956 | 951 | **951** | −17 |
| `test_carte_dump.py` / `test_bench_reel.py` / `test_sauvegarde_littlefs.py` | 38 / 28 / 11 | | | | | **38 / 28 / 11** | |
| captures du simulateur (7 scénarios) | 92 | | 88 | | | **88** | −4 (leviers retirés) |

Le passage de m3 coûte +156 o de code (deux allocations en PSRAM) pour 14 432 o de RAM interne rendus. Les
écarts de 12 o de PSRAM entre deux mesures QEMU sans changement d'allocation sont du bruit de démarrage.

`test_core` −17 = −19 + 7 − 5 : 19 vérifications des leviers 1a/1b/(c) et du témoin sont parties avec eux ;
7 sont arrivées (relecture des v2/v3 avec un bloc par station, refus « v4 annoncé v3 ») ; 5 sont parties avec
le rajeunissement (4 remplacées par 1) et l'exclusion (variante « exclu » et sa vérification du lendemain).

### Ce qui prouve que rien d'autre n'a bougé

- **Écran** : les 7 scénarios du simulateur (défaut, `ops`, `hors-ligne`, `service`, `totaux`, `boot`,
  `boot-wifi-ko`) ont été joués avec `6e7ac21` et avec `c53fea7`. Les **88 captures communes sont identiques
  octet pour octet** ; seules manquent `10b`–`10e` (leviers allumés à la main, jamais à l'écran de la
  carte). Les deux écrans du démarrage sous QEMU sont identiques eux aussi. **Aucun écran n'a bougé**, il n'y a
  donc pas de nouvelle capture ; `docs/captures/44_` à `47_` sont retirées, 48–58 gardent leur nom.
- **Chiffres du banc** : 11 exécutions (graines 1–3 × nul/moyen/fort au réglage livré, et la référence
  v1.0.8 sur 2 graines), 168 jours, avec le banc d'avant le nettoyage (`f3162e0`) et celui d'après : **2 222
  valeurs comparées, 0 différente**. Exemple, monde moyen, graine 1 : MAE +60 1,0663, 155,07 fausses
  alertes par jour, rappel 90,5 %.
- **Chiffres annoncés, rejoués** : monde nul (M1), fuite et fériés (m2) ci-dessus. Les autres chiffres du
  document ne bougent pas, le banc rendant les mêmes valeurs.

## 3. Propositions de retrait ou de changement côté écran (le propriétaire tranche)

Rien de ceci n'est fait : chaque point touche ce qui se voit.

1. **Retirer l'« écart habituel ± X, mesuré sur N jours (pour information) » de la fiche.**
   *Pourquoi* : depuis le retrait de 1a, il ne décide de rien. *Ce que ça libère* : l'apprentissage de la
   dispersion à chaque tranche de 5 min, `dispSince` + `dispPendDev` + `dispPendCell` (≈ 2,8 Ko de PSRAM), et
   surtout, avec un `/vn_prof.bin` v5 à cellule de 3 octets comme la v1.0.8, **≈ 134 Ko de PSRAM** (200
   stations) et **≈ 99 Ko de moins à chaque sauvegarde** (401 128 → ≈ 301 700 o pour 148 stations, toutes
   les 30 min, moins d'usure de la flash). *Ce qu'on perd* : une indication de la variabilité d'une station à
   cette heure, utile pour juger soi-même une alerte, et la matière d'un futur levier 1a s'il était remesuré
   sur le vrai parc.
2. **« flash utilisée 420 ko » → « flash utilisée 420 ko sur 3 456 ko »** (onglet Service → La carte ;
   `fsTotal` existe déjà). Ajout d'un dénominateur, rien ne se perd.
3. **« météo apprise : −2 vélos » → « … −2 vélos sur le parc »** (provenance « Prévision +1 h »). C'est une
   somme sur le parc, aujourd'hui dite seulement dans la règle.
4. **En-tête : une météo périmée ne devrait pas s'afficher comme fraîche.** Aujourd'hui l'en-tête garde la
   dernière température et la dernière icône sans limite d'âge, alors que la fiche dit déjà « météo de plus de
   45 min ». Proposition : griser la température au-delà de 45 min (même seuil que le modèle). Pour la
   capture 40, il faudrait de plus que le scénario simule une météo périmée plutôt que « jamais reçue ».
5. **Classement « 5 plus souvent indisponibles »** : écarter, ou mettre à part sur une ligne
   « hors service tout le mois : N stations », les stations hors service sur toute la période (chantier,
   station retirée), qui sinon occupent la tête en permanence. *Ce qu'on perd* : rien d'invisible, la ligne le
   dit ; le calcul affiché reste celui de la règle.
6. **« Coefficient appris »** (fiche, provenance) : dans un parc où la pluie ne change rien, il peut s'afficher
   des jours durant (M1 : 3,8 % des jours en moyenne, jusqu'à 32 %). Deux options : afficher « appris, à
   confirmer » tant que le coefficient n'a pas tenu, par exemple, 14 jours de suite ; ou durcir le seuil.
   La seconde est un nouveau levier, **à mesurer au banc avant d'être livré**.

## 4. Ce qui ne se vérifie que sur la carte

Dans l'ordre de la recette (`rapport-review-finale.md` § 6, mise à jour) :

1. **Avant le flash** : `python3 tools/sauvegarde_littlefs.py sauver --port /dev/ttyUSB0` (lecture réelle par
   esptool, CH340, jamais faite) ; noter `learnPct`, le nombre de stations et le tas libre en 1.0.8.
2. Flash de l'application **sans « Erase Flash »** ; démarrage complet jusqu'à « Prêt », sans boucle du
   chien de garde ; `[fs] historique restauré` dans le journal.
3. **Migration réelle v1 → v4** : `learnPct` inchangé, courbe « habituel » d'une station connue inchangée ;
   après 30 min, `/vn_prof.bin` réécrit en v4 (≈ 401 Ko pour 148 stations).
4. **Mémoire** : « tas au plus bas » > 20 Ko après 1 h avec Wi-Fi, TLS et un `dump`. Repères QEMU (sans
   Wi-Fi ni TLS) : RAM interne libre au démarrage ≈ 195,7 Ko, soit **≈ 13,6 Ko de mieux** que la pile revue ;
   PSRAM libre ≈ 2 502 Ko.
5. **Dalle** : rail de 6 onglets, chaque onglet au doigt (et au stylet sur la « r »), sous-onglets Service,
   défilement des fiches longues.
6. **Radio et météo** : couper la box 10 min ; bandeau de fraîcheur, « météo de plus de 45 min », retour.
   (Proposition 4 : regarder ce que montre l'en-tête pendant ce temps.)
7. **Écriture flash** : durée d'une sauvegarde, place libre LittleFS pendant l'écriture des `.tmp`.
8. **`dump` réel** (`tools/carte_dump.py --port /dev/ttyUSB0`) : pas de redémarrage à l'ouverture du port,
   durée, sommes justes ; `ping` → `version=1.1.0`.
9. **Onglet Classement** : pas d'à-coup à son rafraîchissement (il alloue maintenant 2 × 4 Ko de PSRAM le
   temps de l'appel).
10. **Hors ligne** : rejouer le « mur de zéros » du rapport hors-ligne.
11. **Sur la durée** : fériés (25/12/2026, 01/01/2027, lundi de Pâques 2027), coefficient météo (ne doit pas
    rester « appris » sans pluie réelle marquée), récurrents, disponibilité contre le contrat Vel'OH!.
12. **Retour arrière** si besoin : reflasher la 1.0.8 (ou la 1.0.9), **puis**
    `tools/sauvegarde_littlefs.py restaurer`. Sans cela, profils perdus (la 1.0.8 refuse le v4 comme elle
    refusait le v3).

La CI elle-même n'a pas tourné sur cet état (rien n'est poussé) : ses étapes ont été rejouées localement.
