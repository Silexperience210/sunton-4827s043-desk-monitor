# Vue « Service » — ce que regarde un directeur de service

Branche `feat/vue-service`. L'écran sert d'abord **l'observateur-décideur** : l'état et la santé du
service, les tendances sur des semaines, ce qui a changé, l'anomalie **structurelle** — et des chiffres
défendables en réunion (numérateur, dénominateur, règle nommée). La tournée de camion reste dans l'écran
Alertes.

Code : `src/core/days.{h,cpp}` (résumés par jour), `src/core/service.{h,cpp}` (règles, phrases),
`src/ui/scr_service.cpp` (écran, 5e des 6 onglets du rail). Tests : `sim/test_core.cpp`
(`testDayCounts`, `testRecurRules`, `testRecurWorld`). Monde à vérité connue : `sim/service_world.h`.
Captures : `docs/captures/48_…58_`.

## 1. Ce qui est compté : la station-tranche

La carte relève `station_status` toutes les 60 s ; l'historique garde **une tranche par 5 min**
(`Model::applyStatus`). Chaque tranche réellement relevée est versée, en plus, dans le **résumé du jour**.

- **Heures de service évaluées** : 7 h – 22 h, heure de Luxembourg (15 créneaux d'une heure, les mêmes
  heures que l'alerte « inhabituel »). La nuit n'est pas comptée.
- **Station observée** dans une tranche : publiée dans `station_information` et ayant déjà reçu un statut.
- **Jour relevé** : un jour ne compte comme relevé que s'il porte au moins **une heure pleinement
relevée** (6 tranches dans une heure) — même définition pour la Synthèse et pour les récurrents.
Une seule tranche isolée, ou une soirée entière, ne fait pas un jour relevé.

**Unité affichée** : la station-tranche (une station observée 5 min), convertie en **heures**
  (12 tranches = 1 h ; « 22 758 h » = 22 758 heures-stations).

### Les quatre états — la définition de « disponible »

| État | Règle (statut de remplissage `classify`, `src/core/model.cpp`) | Ce que vit l'usager |
|---|---|---|
| **Disponible** | ouverte, **≥ 1 vélo louable ET ≥ 1 borne libre** | peut prendre et rendre |
| **Vide** | ouverte, aucun vélo louable (location suspendue = vide) | peut rendre, pas prendre — sauf si aucune borne n'est libre non plus (cas rare, classé « vide » quand même) |
| **Pleine** | ouverte, aucune borne libre (retours suspendus = pleine) | peut prendre, pas rendre |
| **Hors service** | non installée, ou ni location ni retour | ni l'un ni l'autre |

**Taux de disponibilité = disponibles ÷ observées.** Une station vide n'est **pas** « indisponible » au
sens du matériel : elle est *vide*. L'écran ne donne jamais un pourcentage seul : il affiche les heures
(« 97,3 % · 54 671 h sur 56 211 h ») et la **répartition en quatre parts** sous la barre, avec la
définition en clair. La couverture est dite à côté : « 27 j sur 30 relevés, 97,1 % des tranches des jours relevés » —
les heures où la carte était éteinte ne comptent **ni pour ni contre** (le dénominateur est le temps
observé, pas le temps calendaire).

### Ruptures et saturations par jour

Une **entrée en rupture** (en saturation) = passage à *vide* (à *pleine*) entre **deux tranches
consécutives relevées** (5 min d'écart). Une station déjà vide au premier relevé après un trou n'est pas
comptée (début inconnu) ; la dernière tranche avant 7 h sert de « précédente » à celle de 7 h. C'est un
**minimum** : une station vidée puis regarnie entre deux relevés n'est pas vue.

## 2. Le résumé par jour en flash : `/vn_days.bin`, version 1

60 jours (`VN_DAYS`), anneau en PSRAM, sauvegardé avec l'historique et les profils (même rythme, même
écriture atomique `fichier.tmp` + renommage). Ce qui est stocké, **exactement** :

**Réseau, par jour** (`DayNet`, 204 octets) : jour local (jours depuis le 01/01/1970), jour de semaine,
drapeau « férié légal » (`core/calendar`) ; et **par créneau d'une heure** (15) : tranches relevées
(≤ 12), station-tranches observées, dont hors service / vides / pleines, entrées en rupture, entrées en
saturation.

**Station, par jour** (`DaySt`, 16 octets) : tranches observées / hors service / vides / pleines sur
7 h–22 h (≤ 180), et **cinq masques de 15 bits** (un bit par créneau) :

- *relevé* : ≥ 6 tranches observées dans l'heure (30 min) ;
- *touché par le vide*, *par le plein*, *par un écart au-dessus du profil*, *en dessous du profil* :
  ≥ 3 tranches (15 min) dans cet état pendant l'heure. Les deux sens de l'écart sont **séparés** (voir §4).

**Fichier** : en-tête de 40 octets (magic `VND1`, version majeure 1, nombre de stations, nombre de
jours, taille d'un `DayNet` et d'un `DaySt` **dans ce fichier**, créneau en cours, dernière tranche,
date de sauvegarde) · sid des stations (8 octets chacun, ordre des colonnes) · jours du plus ancien au
plus récent (`DayNet` puis une ligne de `DaySt`) · compteurs de l'heure en cours (5 par station) ·
statut de la tranche précédente (1 par station).

Taille pour 143 stations et 60 jours : 40 + 143 × 8 + 60 × (204 + 143 × 16) + 143 × 6 =
**151 562 octets** (≈ 148 Kio en flash ; ≈ 205 Kio de PSRAM pour 200 emplacements).

**Relecture et migration** (testées, `testRecurWorld`) :

- **Fichier absent** (carte en v1.0.8 ou antérieure) : résumés vides, tout le reste (profils, historique
  24 h) chargé ; le résumé du jour démarre au premier relevé.
- **Tout est vérifié avant de toucher au modèle** (`DayBook::check`) : magic, version majeure, tailles
  d'enregistrement plausibles, longueur **exacte** du fichier. Magic faux, version 2 inconnue, fichier
  tronqué d'un octet, réduit à l'en-tête, nombre de jours aberrant, taille d'enregistrement absurde,
  stations annoncées ≠ présentes : **fichier ignoré, aucune station créée**, profils et historique chargés,
  aucun plantage.
- **Format plus récent de même version majeure** (champs ajoutés en fin d'enregistrement) : relu par le
  préfixe connu. Un format plus court serait complété de zéros.
- **Colonnes réordonnées** (`station_information` a changé d'ordre) : réassociées par sid.
- **Un jour incohérent** (comptes impossibles, date non croissante ou **future** — horloge revenue en
  arrière) : ce jour seul est écarté, les autres gardés ; le créneau en cours n'est repris que s'il
  prolonge le jour le plus récent relu.
- **Réinitialiser l'historique** (Réglages) efface aussi les résumés et le fichier.

## 3. L'écran

Rail : **Service** (icône jauge), entre Alertes et Réglages. Quatre volets :

**Synthèse** (`49`–`51`) — disponibilité d'**aujourd'hui sur les heures complètes** (7 h → l'heure
pleine passée ; « avant 8 h : aucune heure complète ») et sur **30 jours complets** (hier et les 29 jours
d'avant, aujourd'hui exclu), toujours en « % · h sur h » ; répartition disponible / vide / pleine / hors
service ; courbe des **entrées en rupture (barres) et en saturation (ligne) par jour** sur 30 jours (un
jour non relevé est un trou, pas un zéro) ; **comparaisons** : les 7 derniers jours contre les 7 d'avant
(en points, avec le nombre de jours relevés de chaque côté), ruptures et saturations de ces deux semaines,
et aujourd'hui contre **le même jour de la semaine précédente, mêmes heures**.

**Récurrents** (`48`, `52`, `53`) — la règle en une phrase, le périmètre examiné (« 18 jours ouvrables
et 9 de week-end ou fériés relevés sur les 28 derniers jours »), puis la liste : « Al Bréck — Vide
8 h–9 h · 10 j ouvrables sur 10 · série 12 j ». Un tap ouvre la fiche de la station. Avant d'avoir
assez de jours, l'écran le dit (« Trop tôt pour conclure : il faut au moins 7 jours ouvrables… la carte
en a 3 »). Cette liste est **distincte** des alertes instantanées : l'incident du jour reste dans Alertes.

**Classement** (`54`, `55`) — les 5 stations le plus souvent indisponibles et les 5 le plus souvent
disponibles sur 30 jours complets : part indisponible = (vide + pleine + hors service) ÷ temps observé,
avec les heures de chaque part et le temps observé. Seules les stations observées au moins **la moitié du
temps de la mieux observée** sont classées (une station apparue hier n'est pas « la meilleure » sur 2 h).

**La carte** (`56`, `57`) — fraîcheur du flux des stations (âge du dernier relevé contre le seuil du
bandeau, badge FRAIS / VIEUX), météo (âge contre 45 min), Wi-Fi (SSID, dBm, qualité), mémoire (tas libre
et au plus bas, PSRAM, flash), apprentissage (`learnPct` : créneaux de 15 min de la semaine relevés
≥ 3 fois), résumés par jour (jours gardés, depuis quand, et **couverture d'aujourd'hui** en tranches
relevées sur tranches possibles — badge TROUS sous 90 %). Pour pouvoir dire « les chiffres sont frais ».

## 4. Le détecteur de récurrent

**Règle.** Un créneau d'une heure (7 h–22 h) est *touché* un jour donné si la station y a passé
**≥ 15 min** dans l'état ; *relevé* si la carte l'a observée ≥ 30 min dans l'heure. Problème récurrent :

- jours ouvrables (lundi–vendredi non fériés) : **≥ 7 des 10 derniers jours relevés** du créneau ;
- week-end **et jours fériés légaux** : **≥ 5 des 6 derniers** ;
- au plus 28 jours calendaires en arrière ; **aujourd'hui exclu** (jours complets seulement) ;
- un jour où le créneau n'a pas été relevé (carte éteinte, Wi-Fi coupé) ne compte **ni pour ni contre** ;
- états : vide, pleine, au-dessus du profil, en dessous du profil — **jamais additionnés** ;
- un écart au profil au créneau où la station est déjà signalée vide ou pleine est le **même** problème :
  dit une fois, par l'état ;
- créneaux voisins signalés pour la même station, le même état et la même classe : **fusionnés** en une
  plage (« 14 h–16 h ») ; les nombres affichés sont ceux du créneau **le moins touché** de la plage ;
- **série** = jours relevés consécutifs touchés en remontant depuis hier — elle peut dépasser la fenêtre
  (« 10 sur 10 · série 12 j ») ; « série interrompue » si le dernier jour relevé est sain. La série se compte en jours de la classe **relevés** : elle peut traverser un jour sans données.
- Tri : la plus longue série d'abord, puis le taux.

**Fériés.** Un férié se comporte comme un dimanche (c'est ce que mesure `docs/mesures-prevision.md`) :
il est rangé **avec le week-end**, jamais comme un jour normal. Contre-épreuve dans `test_core` : la
station « quasi-seuil » (6 matins vides sur 10 + Noël) aurait été signalée à tort (7 sur 10) si Noël avait
compté comme un vendredi.

**Pourquoi les deux sens de l'écart sont séparés.** Première version : un seul masque « écart au profil »
(au-dessus OU en dessous). Sur le monde à vérité connue, elle signalait **5 des 13 stations
« pendulaires au hasard »** et 1 station de bruit : une station vide un matin sur trois a un profil
moyen (~7 vélos) dont elle s'écarte *dans les deux sens* presque chaque jour (0 vélo, ou 12). Ce n'est pas
« le même écart ». Après séparation : **0** sur ces mêmes stations.

## 5. Vérification : le monde à vérité connue

`sim/service_world.h` : quatre semaines, du lundi 30/11/2026 au lundi 28/12/2026 10 h, carte éteinte
toute la journée du 17/12, Noël (vendredi) et Saint-Étienne fériés. Rôles :

| Rôle | Vérité | Attendu |
|---|---|---|
| dérive | vide 8 h–9 h chaque jour ouvrable depuis le 08/12 | **signalée** |
| week-end | pleine 14 h–16 h samedis, dimanches, fériés | **signalée, classe week-end** |
| saine | 8 à 12 vélos sur 20 | rien |
| incident isolé | vide 8 h–10 h le 22/12, pleine 17 h–18 h le 15/12 | rien |
| quasi-seuil | vide à 8 h 6 jours ouvrables sur 10, + Noël | rien |
| hors service | tout le mois | rien (mais en tête des pires) |
| bruit | chaque jour, 0 à 2 épisodes vides ou pleins de 15–60 min à une heure quelconque | rien |
| pendulaire au hasard | vide à 8 h un jour ouvrable sur trois | cas limite, mesuré |

**`test_core` (60 stations : 8 nommées, 40 de bruit, 12 pendulaires)** — trouvées : dérive (« Vide
8 h–9 h · 10 j ouvrables sur 10 · série 12 j ») et week-end (« Pleine 14 h–16 h · 6 j de week-end/férié
sur 6 · série 9 j ») ; faux positifs : **0** sur les rôles nommés, **0** sur 41 stations de bruit,
**0** sur 13 pendulaires.

**Scénario du simulateur (`./sim … service`, 143 vraies stations)** — 2 vrais trouvés sur 2, **0** faux
positif sur les rôles nommés et les 41 stations de bruit, **1** pendulaire au hasard sur 14 signalée
(« Carmel — Vide 8 h–9 h · 8 jours ouvrables sur 10 · série interrompue » : elle a réellement été vide
8 des 10 derniers matins ouvrables ; le hasard a produit ce que la règle cherche).

**Banc de bruit (hors dépôt, 200 stations × 3 tirages, mêmes quatre semaines)** :

| Stations | Signalées (moyenne) | Loi binomiale P(≥ 7 sur 10) |
|---|---|---|
| bruit (épisodes au hasard) | 1 sur 600 (0,17 %), « en dessous du profil » | — |
| vides à 8 h 30 % des matins | 0,8 % | 1,1 % |
| 40 % | 5,7 % | 5,5 % |
| 50 % | 17 % | 17,2 % |
| 60 % | 38 % | 38,2 % |

Le détecteur se comporte exactement comme sa règle : « au moins 7 matins sur 10 ». Une station vide un
matin sur deux sera signalée un mois sur six — à lire comme « fréquent », pas comme une erreur.
Coût : `svcRecurrent` ≈ 0,8 ms pour 200 stations × 28 jours sur PC (x86-64, -O2) ; recalculé seulement
à chaque tranche versée (5 min) et quand l'écran Service est affiché.

## 6. Limites — ce qui n'est pas vérifié sur la carte

- **Rien de ceci n'a tourné sur la carte.** Écriture de ~150 Ko supplémentaires en LittleFS à chaque
  sauvegarde (durée, usure, place libre sur la partition 8 Mo pendant l'écriture du `.tmp`), PSRAM
  (~205 Ko), durée de `svcRecurrent` sous le verrou du modèle sur l'ESP32-S3 : **à mesurer**.
- Le rail passe de 5 à 6 onglets de 39 px (au lieu de 47) : l'audit tactile du simulateur est vert
  (aucune cible < 24 px), le **toucher réel** sur la dalle résistive (« r ») reste à essayer.
- Fériés : seuls les fériés **légaux** luxembourgeois sont reconnus. Vacances scolaires, grèves, chantiers,
  météo extrême ne sont pas distingués : un créneau « touché » un jour de neige compte comme un autre.
- L'écart au profil ne compte que là où le profil est appris ; et un problème récurrent **finit par être
  appris** par le profil (mémoire de 8 semaines) : l'écart au profil s'efface alors, le vide ou le plein
  restent signalés.
- Un changement d'heure (fin octobre, fin mars) donne un jour de 23 ou 25 h : les créneaux sont en heure
  locale, rien n'est faussé, mais la journée a un créneau de nuit en plus ou en moins (hors heures de
  service).
- Les entrées en rupture sont un minimum (relevé de 5 min).
- 30 jours de comparaison demandent 30 jours de carte allumée : jusque-là l'écran dit combien de jours
  sont réellement relevés.

## Note de version — `feat/vue-service`

- **Nouveau : écran Service** (nouvel onglet, le rail en compte 6) — disponibilité du jour et sur 30 jours avec numérateur et
  dénominateur, parts vide / pleine / hors service nommées, ruptures et saturations par jour sur 30 jours,
  comparaisons d'une semaine à l'autre et au même jour de la semaine précédente, problèmes récurrents
  avec leur règle affichée, 5 pires et 5 meilleures stations, état de la carte (fraîcheur, Wi-Fi, mémoire,
  apprentissage, couverture des résumés).
- **Nouveau : `/vn_days.bin`** (version 1) — 60 résumés par jour ; fichier absent ou abîmé : ignoré
  sans plantage, le reste chargé.
- `test_core` : **977 vérifications** (celles de cette vue, comptes à la main, règles du récurrent, monde à
  vérité connue, persistance et fichiers abîmés). Simulateur : scénario `service`, captures 48 à 58 ;
  les captures 00 à 47 ont été refaites (rail à 6 onglets) ; audit tactile : 392 éléments, 0 masqué.
- Non vérifié sur la carte : voir §6.
