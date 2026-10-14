# Mesures de la prévision — banc d'essai et leviers

Ce document est la pièce centrale de la branche `feat/prevision-meteo`. Chaque levier de prévision y
a **sa propre mesure avant/après**, faite avec le banc `sim/bench_prevision` sur le **vrai** modèle
(`src/core/model.cpp`, le code qui tourne sur la carte). Un levier qui ne gagne pas est écrit comme
tel et n'est pas livré.

Tout est rejouable :

```bash
cd sim && make bench_prevision
./bench_prevision                                  # rapport complet : 168 jours, graine 1, effet « moyen »
./bench_prevision --effet fort --graine 2          # autre monde
./bench_prevision --meteo absente                  # la carte ne reçoit jamais la météo
cd .. && python3 tools/bench_campagne.py           # 3 graines × 3 effets, en parallèle (~1 min)
python3 tools/bench_campagne.py --levier nom=1     # un levier, comparé apparié à la référence
```

## Le banc

### Le monde simulé

- **143 stations** de six types — 52 résidentielles, 34 de bureaux, 2 gares, 8 écoles, 12 parcs,
  33 mixtes — de 8 à 28 bornes (35 pour les gares), plus deux stations spéciales (ci-dessous).
  Chaque type a sa courbe de remplissage de jour ouvré et de week-end (les résidentielles se vident
  de 7 h à 9 h et se remplissent de 17 h à 20 h, les bureaux l'inverse, l'école se remplit à 8 h et
  se vide à 16 h — à midi le mercredi —, le parc se remplit le week-end après-midi…). Chaque station
  a sa propre amplitude (± 20 %) et son propre décalage horaire (± 15 min).
- Les mouvements sont **des trajets tirés au hasard** (loi de Poisson) toutes les 5 min : un flux net
  qui suit la courbe du type, plus une activité de fond qui part et revient. Une station vide ne
  peut plus prêter, une station pleine ne peut plus recevoir.
- **Régulation** : à 3 h, chaque station est ramenée à 70 % de l'écart vers son niveau de nuit ; de
  7 h à 21 h, un camion rééquilibre au plus une station toutes les 20 min (celle qui est en rupture
  depuis le plus longtemps, au moins 30 min).
- **Jours fériés** légaux du Luxembourg : la population s'y comporte comme un dimanche.
- **Météo** horaire : épisodes de pluie (chaîne de Markov dont le déclenchement dépend de la saison
  simulée — printemps humide, été plus sec mais orageux), intensité log-normale, température
  saisonnière et diurne, vent, codes WMO. Sur 168 jours (2 mars → 16 août 2026) : 10 % des heures
  sont pluvieuses, 37 jours de pluie (≥ 3 h de pluie entre 7 h et 22 h), 109 jours secs.
- **Prévision météo** telle que la carte la reçoit, toutes les 15 min : l'actuel exact et 6 points
  horaires **bruités** (calage temporel faux de ± 1 h, plus probable loin dans le temps, intensité
  approximative, fausses alertes et pluies manquées, probabilité de pluie floue). La note « conditions
  vélo » est calculée par le vrai `wxScore`.
- **Effet de la météo sur la demande** : la pluie, le vent fort, le froid et l'orage multiplient les
  trajets de chaque station par `1 − effet × sensibilité × pénalité`. La sensibilité dépend du type
  (parc 0,90 ; mixte 0,60 ; résidentiel 0,45 ; bureaux 0,40 ; gare 0,35 ; école 0,25) et de la
  station (× 0,7…1,3). Trois mondes : **nul** (la pluie ne change rien : jeu d'essai « la météo
  n'apporte rien »), **moyen** (pluie modérée ≈ −20 % de trajets pour les navetteurs, ≈ −45 % pour
  les parcs) et **fort** (× 1,6). Comme les départs et les arrivées sont freinés **au moment où il
  pleut**, la pluie change aussi la *direction* du mouvement (une pluie à 16 h retient les vélos
  arrivés au parc à 14 h).
- **Incidents** (vraies anomalies) : ~2 par jour entre 9 h et 19 h, une station est décalée d'un coup
  de ± max(6, ½ capacité) vélos (évènement, dépôt groupé…). Fenêtre de vérité : 3 h.
- **Station agrandie** : une station résidentielle passe de 20 à 30 bornes au jour 57 (le flux
  `station_information` l'annonce, l'exploitant la remplit à proportion) ; une **jumelle** de 30
  bornes, mêmes paramètres, sert de témoin depuis le premier jour.
- Une station est fermée 4 jours (jours 31 à 34) : elle doit rester hors du profil.

**La vérité ne dépend jamais du modèle** : à graine égale, deux réglages du modèle voient exactement le
même monde. Les comparaisons de leviers sont donc **appariées** (seul le modèle change).

**Calibrage.** ≈ 2 200 retraits par jour sur le parc (ordre de grandeur de Vel'OH!) ; à 14 h 30 en
semaine, ≈ 3 stations vides et 2 pleines, contre 15 et 7 dans l'instantané réel du 13/10/2026
(`docs/donnees/`) : **le parc réel est plus dispersé que le parc simulé**. Les bruits d'alertes
mesurés ici sont donc plutôt un minimum.

### Ce qui est mesuré

1. **MAE du chiffre affiché** (la prévision arrondie, ce que lit l'exploitant sur la fiche) à
   +15 / +30 / +60 / +120 min, toutes les 15 min de 6 h à 22 h, sur toutes les stations ouvertes.
   En regard : la **persistance** (« rien ne bouge ») et un **oracle météo** — la même règle de
   prévision, mais dont l'écart au profil est mis à l'échelle de la demande **réelle** sur l'horizon
   (rapportée à la demande moyenne qui a façonné le profil). L'oracle est le **plafond** de ce
   qu'un terme météo multiplicatif pourrait gagner avec une connaissance parfaite. « Pluie » = un
   échantillon dont l'horizon contient de la pluie réelle ; « mûr » = à partir du jour 57.
2. **Alertes « inhabituel »** en *épisodes* (une alerte qui revient à la même station dans l'heure ne
   compte qu'une fois) par jour de pluie / jour sec, **fausses** (hors incident) et **vraies**, et le
   **rappel** : part des incidents pendant lesquels l'alerte a sonné.
3. **Maturation** : couverture (`learnPct`), confiance du profil, part des stations évaluées.
4. **Absorption** de l'agrandissement : écart moyen du profil de la station agrandie à celui de sa
   jumelle (7 h–22 h, 7 jours).
5. **Bruit** : apparitions d'alertes par jour, par type ; ruptures réelles annoncées dans l'heure
   précédente (rappel des « rupture prévue ») ; précision de ces annonces.

Pourquoi l'arrondi : à +15 min, une station sans grande activité a toutes les chances de **ne pas
bouger** ; la MAE récompense la médiane, et une prévision fractionnaire (7,6) y perd contre la
persistance (7) alors que l'écran affiche « 8 ». Noter le chiffre affiché est la seule mesure
honnête du produit. La RMSE de la prévision brute est donnée en complément.

## Étape 0 — mesure de l'existant (v1.0.8, aucun changement du modèle)

### Référence, 3 graines × 3 mondes (168 jours ; MAE en vélos, période mûre)

| mesure | effet nul | effet moyen | effet fort |
|---|---|---|---|
| MAE +15 min | 0,414 | 0,405 | 0,400 |
| MAE +30 min | 0,708 | 0,694 | 0,687 |
| MAE +60 min | 1,110 | 1,092 | 1,083 |
| MAE +120 min | 1,634 | 1,608 | 1,601 |
| MAE +60 min, horizon pluvieux | 1,118 | 1,016 | 0,954 |
| **oracle** +60 min, horizon pluvieux | 1,118 | 0,958 (−5,7 %) | 0,840 (−12 %) |
| **oracle** +60 min, tout | 1,110 | 1,088 (−0,4 %) | 1,074 (−0,8 %) |
| persistance +60 min | 1,312 | 1,282 | 1,261 |
| MAE +60 min, jours fériés | 1,376 | 1,323 | 1,288 |
| fausses alertes « inhabituel » / jour de pluie | 155,2 | 143,7 | 142,1 |
| fausses alertes « inhabituel » / jour sec | 159,6 | 159,5 | 158,7 |
| rappel des incidents | 84,2 % | 84,2 % | 82,3 % |
| ruptures (vide) annoncées dans l'heure | 51,3 % | 52,1 % | 52,0 % |
| saturations annoncées dans l'heure | 59,0 % | 60,1 % | 59,7 % |

Les écarts entre graines sont faibles (MAE +60 : ± 0,01 ; fausses alertes : ± 3 / jour).

### Détail, graine 1, effet moyen

MAE du chiffre affiché — modèle / persistance / oracle :

| échantillon | +15 min | +30 min | +60 min | +120 min |
|---|---|---|---|---|
| mûr | 0,404 / 0,397 / 0,404 | 0,691 / 0,720 / 0,690 | 1,087 / 1,269 / 1,084 | 1,604 / 2,131 / 1,596 |
| mûr · sec | 0,409 / 0,406 / 0,412 | 0,696 / 0,735 / 0,701 | 1,091 / 1,291 / 1,096 | 1,602 / 2,161 / 1,605 |
| mûr · pluie | 0,362 / 0,318 / 0,335 | 0,650 / 0,600 / 0,601 | 1,061 / 1,110 / 0,997 | 1,613 / 1,953 / 1,542 |
| jours fériés | 0,440 / 0,315 / 0,432 | 0,785 / 0,565 / 0,770 | 1,310 / 0,967 / 1,287 | 2,071 / 1,575 / 2,030 |

- Profil seul, **sans** le terme de tendance : 0,383 / 0,661 / 1,060 / 1,586.
- RMSE de la prévision brute / persistance : 0,870 / 0,944 · 1,238 / 1,455 · 1,734 / 2,288 · 2,367 / 3,501.

Maturation de la prévision (MAE par tranche de 3 semaines) :

| semaines | +15 | +30 | +60 | +120 | persistance +60 |
|---|---|---|---|---|---|
| 1–3 | 0,511 | 0,827 | 1,220 | 1,787 | 1,246 |
| 4–6 | 0,471 | 0,787 | 1,195 | 1,734 | 1,264 |
| 7–9 | 0,411 | 0,709 | 1,115 | 1,640 | 1,229 |
| 10–12 | 0,403 | 0,693 | 1,093 | 1,616 | 1,249 |
| 16–18 | 0,400 | 0,684 | 1,080 | 1,602 | 1,246 |

Maturation du profil (à midi) :

| jour | couverture `learnPct` | confiance à +1 h (parc) | stations évaluées « inhabituel » |
|---|---|---|---|
| 2 (mardi) | 21 % | 19 % | 0 % |
| 4 (jeudi) | 50 % | 56 % | 100 % |
| 6 (1er samedi) | 79 % | 0 % | 0 % |
| 8 | 100 % | 75 % | 100 % |
| 15 | 100 % | 75 % | 100 % |
| 22 | 100 % | 100 % | 100 % |

Absorption de l'agrandissement 20 → 30 bornes (écart moyen du profil à la jumelle, en vélos) :

| jours après | 0 | +7 | +14 | +28 | +42 | +56 | +84 | +111 |
|---|---|---|---|---|---|---|---|---|
| écart | 5,52 | 4,55 | 3,78 | 2,53 | 2,32 | 1,82 | 2,08 | 1,08 |

Moitié absorbée en **26 jours** ; 90 % non atteints en 16 semaines.

Bruit (apparitions par jour, jours mûrs) : vide 66, pleine 58, rupture prévue 58, saturation prévue 56,
aucun mouvement 64, **inhabituel 262** (≈ 159 épisodes).

### Ce que dit l'étape 0

1. **La prévision existante tient sa promesse là où elle compte** : −14 % d'erreur contre la
   persistance à +60 min, −25 % à +120 min. À +15 et +30 min, elle fait **jeu égal** avec « rien ne
   bouge » : à cet horizon il n'y a presque rien à prévoir.
2. **Le plafond de la météo est bas.** Même un oracle qui connaît exactement la demande réelle ne
   gagne que **0,4 % (moyen) à 0,8 % (fort)** de MAE à +60 min sur l'ensemble, 6 à 12 % sur les
   seuls horizons pluvieux. Raison : la prévision part de la **mesure réelle** ; tout l'effet de la
   pluie déjà tombée est dans le point de départ, et la météo ne peut corriger que le mouvement
   *à venir* sur ≤ 2 h, quelques vélos tout au plus.
3. **La pluie ne fait pas sonner l'alerte « inhabituel » partout** — c'est l'inverse : 142 à 155
   épisodes faux par jour de pluie contre 159 par jour sec. Le parc bouge moins sous la pluie, donc
   s'écarte moins au hasard ; l'écart systématique dû à la pluie (une résidentielle qui ne se vide
   pas) reste le plus souvent sous la tolérance. **Le problème de l'alerte n'est pas la météo, c'est
   son niveau de bruit** : ≈ 159 épisodes faux par jour pour 2 vrais, plus d'une fausse alerte par
   station et par jour.
4. **Les jours fériés sont le pire cas du modèle** : à +60 min, MAE 1,31 contre 0,97 pour la
   persistance — la prévision y est **pire que rien** (elle prévoit la vague d'un mardi ordinaire).
5. **Le terme de tendance n'aide pas dans ce monde** (profil seul : 1,060 contre 1,087 à +60 min).
   Réserve : le monde simulé n'a pas d'inertie hors pluie et incidents (pas de groupes, pas
   d'évènements progressifs) ; ce constat est à vérifier sur données réelles avant d'y toucher.
6. **Échelles de temps** : couverture du profil complète en 8 jours, confiance pleine en 3 semaines ;
   prévision stabilisée vers la 7ᵉ–9ᵉ semaine ; un changement franc est à moitié absorbé en ~4
   semaines, et pas entièrement en 16. Le « ~8 semaines » du plan est la *constante de temps* de la
   moyenne plafonnée à 24 (3 relevés par semaine et par créneau), pas le temps d'absorption.

## Étape 1 — tolérance météo : **ne gagne pas, non livrée**

Deux leviers, mesurés séparément (commit d'expérience `739b5cf`, rejouable :
`git checkout 739b5cf && python3 tools/bench_campagne.py --levier tol_meteo=1`) :

- **1a** `tol_meteo` : tolérance « inhabituel » × (1 + pénalité), pénalité = (10 − note vélo) / 10, la
  note « conditions vélo » existante (`wxScore`), météo ignorée si absente ou plus vieille que 45 min ;
- **1b** `rupture_meteo` : horizon des alertes « rupture / saturation prévue » 60 min ÷ (1 + pénalité).

Comparaison appariée, 3 graines × 3 mondes, 168 jours :

| 1a — tolérance × (1 + pénalité) | effet nul | effet moyen | effet fort |
|---|---|---|---|
| fausses alertes / jour de pluie | 155,2 → 122,0 (−21 %) | 143,7 → 116,4 (−19 %) | 142,1 → 116,9 (−18 %) |
| fausses alertes / jour sec | 159,6 → 158,8 | 159,5 → 158,1 | 158,7 → 158,2 |
| rappel des incidents, **jours de pluie** | 82,7 → 73,2 % (−9,5) | 79,9 → 72,3 % (−7,6) | 81,4 → 70,1 % (−11,3) |
| rappel des incidents, tous jours | 84,2 → 81,2 % | 84,2 → 81,5 % | 82,3 → 79,0 % |

Témoin : la même tolérance élargie **uniformément**, tous temps confondus (effet moyen) :

| tolérance × | fausses / jour de pluie | fausses / jour sec | rappel tous jours | rappel jours de pluie |
|---|---|---|---|---|
| 1,04 | −9,0 | −9,4 | −1,2 | −1,2 |
| 1,06 | −12,9 | −14,1 | −1,9 | −1,7 |
| 1,08 | −16,9 | −18,7 | −3,0 | −3,9 |

| 1b — horizon ÷ (1 + pénalité) | effet nul | effet moyen | effet fort |
|---|---|---|---|
| annonces / jour de pluie | 122,1 → 112,7 | 107,0 → 99,2 | 97,4 → 91,4 |
| précision des annonces, pluie | 58,2 → 60,7 % | 55,5 → 58,3 % | 54,6 → 57,3 % |
| ruptures réelles annoncées | 51,3 → 50,7 % | 52,1 → 51,7 % | 52,0 → 51,6 % |

**Pourquoi c'est un échec, et pas un petit gain :**

1. **Le gain est le même dans le monde où la pluie ne change rien** (effet nul : −21 % de fausses
   alertes, contre −19 % quand la pluie agit). Le levier n'exploite aucune information météo : il rend
   l'alerte moins sensible un jour sur six. C'est exactement le « gain inventé » à ne pas livrer.
2. **Il se paie là où il ne faut pas** : −7,6 à −11,3 points de rappel *les jours de pluie* — un
   incident sous la pluie (station vidée, dépôt groupé) passe sous le seuil.
3. **À perte de rappel égale, l'élargissement uniforme fait 4 fois mieux** : × 1,08 partout retire
   ≈ 18 fausses alertes par jour (pluie ou sec) pour −3 points de rappel ; la tolérance météo n'en
   retire en moyenne que ≈ 5 par jour (27 × 18 jours de pluie / 106) pour la même perte.
4. 1b : même constat (effet identique sans effet de la pluie) — des annonces plus courtes sont plus
   précises par tous les temps, ce n'est pas un levier météo.

Le présupposé de l'étape (« par jour de pluie, l'alerte sonne partout ») est faux dans le monde
simulé (voir étape 0, point 3). Ce qui fait vraiment sonner l'alerte à tort, c'est la tolérance fixe
(max(4 vélos, 20 % de la capacité)) face à une variabilité qui dépend du créneau et de la station :
voir les recommandations en fin de document.

## Étape 2 — la météo stockée avec l'historique (aucun effet sur la prévision, voulu)

- Chaque échantillon de 5 min (`NetSample`) porte désormais la météo du moment : température (°C),
  pluie (dixièmes de mm), probabilité de pluie de l'heure en cours (%), vent (km/h), code WMO. Elle
  n'est notée que si le relevé Open-Meteo est **reçu et frais** (≤ 3 périodes, 45 min) ; sinon, et
  pour toute tranche comblée après un trou, elle est **« inconnue »** (jamais 0 °C ni 0 mm).
- `/vn_hist.bin` passe en **version 2** (échantillon de 32 octets au lieu de 24). Un fichier
  **version 1** (v1.0.8) est relu champ par champ, météo « inconnue » ; une version inconnue, une
  taille qui ne correspond pas exactement à la version annoncée, un magic faux ou une tête hors de
  l'anneau sont **ignorés** (pas de lecture hors du fichier, pas de plantage). `/vn_prof.bin` est
  inchangé à cette étape.
- 27 vérifications nouvelles dans `test_core` (653 au total) : horodatage de la météo, absente /
  périmée → inconnue, trou → inconnue, aller-retour v2 octet pour octet, migration d'un vrai fichier
  v1 reconstruit, poursuite de l'écriture après migration, 8 fichiers corrompus refusés.
- Banc : chiffres **identiques** à l'étape 0 (MAE +60 = 1,1001 sur 84 jours, graine 1), comme attendu.

**Objection au plan.** L'historique 5 min est un **anneau de 24 h**, rejeté au redémarrage s'il a
plus de 20 h. Stocker la météo dedans ne rend **rien** apprenable « rétroactivement » au-delà d'une
journée. Ce qui rend la météo apprenable, c'est un apprentissage **en ligne**, dont les statistiques
suffisantes sont gardées dans `/vn_prof.bin` (étape 3). L'étape 2 sert à deux choses plus modestes :
apprendre sur une fenêtre glissante de 30 min à partir de la météo *observée pendant la fenêtre*, et
relire ce que la carte a vu après un redémarrage. La mémoire réelle (PSRAM : +2,3 Ko pour l'anneau,
fichier `/vn_hist.bin` + 2,3 Ko) et l'écriture en flash sont **non vérifiées sur la carte**.

## Étape 3a — coefficient météo global appris : **gagne, livré**

### La règle

- **Ce qu'on corrige** : le *mouvement* prévu par le profil, jamais le niveau constaté. Pour chaque
  pas de la projection : mouvement du profil × (1 − α·(x − x̄ₕ)) quand x > x̄ₕ (rien sinon), où
  x = pénalité météo 0…1 (les barèmes « maintenant » de `wxScore` / 10 : pluie, vent > 20 km/h,
  froid, neige, orage — **la même formule que la note**, factorisée dans `wxPenalty`), et x̄ₕ = la
  pénalité moyenne observée à cette heure de la journée (ce que le profil a moyenné). La part météo
  est bornée à ± 40 % de la capacité utile. Étape 3 : x = la météo actuelle, sur 2 h au plus.
- **Ce qu'on apprend** : toutes les 30 min de 7 h 30 à 22 h, pour chaque station ouverte à profil mûr,
  r = mouvement observé − mouvement du profil sur la fenêtre, sous la météo **observée pendant la
  fenêtre** (échantillons de l'étape 2). Régression r = β·d − α·(x − x̄ₕ)·d (β absorbe le biais propre
  du profil, quel que soit le temps), statistiques suffisantes à décroissance exponentielle (~60 jours),
  gardées dans `/vn_prof.bin` (version 2 ; un fichier version 1 se relit, apprentissage météo à zéro).
- **Quand on l'applique** : seulement si |α̂| > **3 erreurs-types** (robustes, par grappe : les 143
  stations d'une fenêtre partagent la même météo), après au moins 12 fenêtres pluvieuses apprises ;
  borné à [−0,5 ; 1]. Origine affichable : *défaut* (trop peu de pluie : α = 0), *sans effet mesurable*
  (α = 0), *apprise*. Météo absente ou périmée : **aucune** correction.

### Ce qui a été corrigé en route (trouvé par le banc, pas supposé)

| version | défaut observé | correction |
|---|---|---|
| x̄ global, une seule pente, erreur-type naïve | dans le monde **nul**, α = −0,5 « significatif » pendant 5 semaines (froid du matin ≈ heures de pointe, où le profil lissé sous-estime le mouvement) | x̄ par heure, terme β, erreur-type par grappe, profils mûrs seulement |
| α̂ rétréci de 2 erreurs-types | effet divisé par deux (pluie, moyen : −2,6 % au lieu de −4,5 %) | α̂ entier au-delà du seuil |
| correction aussi par beau temps (x < x̄ₕ) | +0,4 à +0,6 % d'erreur par temps sec | correction seulement quand le temps est pire que la normale |
| seuil 2 erreurs-types | une correction inventée **4,3 % des jours** dans le monde nul (6 graines) | seuil 3 : 0,0 % des jours, gain identique (moyen −4,0 % au lieu de −4,2 %) |

Les choix de seuil et de variante ont été faits sur les graines 1–3 et **validés sur des graines
neuves 4–6** (moyen −3,7 %, fort −7,5 % sur l'horizon pluvieux à +60 min, nul : 0).

### Mesure finale (6 graines, 168 jours, écart apparié levier − référence)

| mesure | effet nul | effet moyen | effet fort |
|---|---|---|---|
| MAE +15 min | 0,415 → 0,415 | 0,407 → 0,405 (−0,5 %) | 0,402 → 0,398 (−0,9 %) |
| MAE +30 min | 0,710 → 0,710 | 0,697 → 0,693 (−0,6 %) | 0,690 → 0,683 (−1,0 %) |
| MAE +60 min | 1,112 → 1,112 | 1,095 → 1,090 (−0,5 %) | 1,087 → 1,076 (−1,0 %) |
| MAE +120 min | 1,635 → 1,635 | 1,612 → 1,607 (−0,3 %) | 1,607 → 1,595 (−0,8 %) |
| MAE +60, temps sec | 1,112 → 1,112 | 1,106 → 1,106 | 1,106 → 1,106 |
| **MAE +60, horizon pluvieux** | 1,112 → 1,112 | 1,016 → **0,975 (−4,0 % ± 1,1)** | 0,953 → **0,872 (−8,4 % ± 1,7)** |
| MAE +120, horizon pluvieux | 1,645 → 1,645 | 1,535 → 1,499 (−2,3 %) | 1,471 → 1,391 (−5,5 %) |
| précision des annonces de rupture, jours de pluie | 59,3 → 59,3 % | 56,3 → 57,5 % | 56,1 → 58,0 % |
| ruptures réelles annoncées | 51,9 → 51,9 % | 52,4 → 52,3 % | 52,0 → 52,0 % |
| jours avec une correction appliquée (j ≥ 29) | **0 %** | 87 % | 94 % |
| oracle (plafond), MAE +60 pluvieux | — | 0,958 (−5,7 %) | 0,840 (−12 %) |

Le levier capte **70 % du plafond** de l'oracle sous la pluie (4,0 / 5,7 ; 8,4 / 12). Sur l'ensemble des
créneaux, le gain est petit (−0,5 à −1 %) **parce que le plafond lui-même est petit** (étape 0) ; il est
mesurable (écart apparié ± 0,2 %) et sans contrepartie : il n'agit que sous la pluie, ne fait rien
quand la pluie est sans effet et rien sans météo. Maturation : le coefficient n'existe qu'après
~6 semaines (3 semaines de profil mûr, puis assez de fenêtres pluvieuses) ; avant, l'écran dit « défaut ».

**`--meteo absente` reproduit la référence à la dernière décimale** (comportement strictement
identique sans météo, vérifié sur toutes les mesures du banc).

## Étape 3b — modulation par station : **ne gagne pas, non livrée**

α par station, rétréci vers α global selon sa précision (Bayes empirique : wᵢ = τ² / (τ² + seᵢ²)).

- Mesure (6 graines, levier 3a en référence) : **écart exactement nul** sur toutes les mesures, dans
  les trois mondes : τ² estimé = 0, tout revient au global.
- Diagnostic (graine 1) : ~144 fenêtres pluvieuses par station sur la mémoire de 60 jours ;
  α̂ᵢ dispersés de ± 0,40 (moyen) pour une erreur-type moyenne de 0,45. L'écart **réel** entre stations
  (sensibilité du parc 0,9 contre 0,25 pour une école, ≈ ± 0,2) est noyé dans le bruit d'estimation —
  il y a pourtant du signal (corrélation 0,47 à 0,57 avec la vraie sensibilité).
- Plafond : même en imposant la vraie variance entre stations (τ² = 0,04 ou 0,1), l'écart reste
  **±0,001** de MAE. Il n'y a rien à gagner à cet horizon : la correction météo d'une station est déjà
  petite, et la moduler de ± 40 % déplace des fractions de vélo.

Le plan voulait la modulation « seulement là où il y a assez d'échantillons pluvieux pour l'estimer
honnêtement » : **il n'y en a nulle part** en deux mois de mémoire, avec une pluie sur 10 % des heures.
Code mesuré au commit d'expérience suivant, puis retiré.

## Étape 4 — la météo prévue, au bon horizon : **livrée au-delà de 2 h seulement**

Le banc évalue désormais aussi **+240 min** (la courbe 24 h, `Model::project`, même règle) et isole deux
cas : la pluie **qui commence** dans l'horizon (sec maintenant, pluie à t+h) et celle **qui s'arrête**.
Option `--prevision-parfaite` : la carte reçoit la vraie météo future (plafond du levier).

### 4a — météo prévue à t+h partout (y compris sur 2 h) : ne gagne pas

x(τ) = pénalité de l'heure prévue qui contient τ (pluie comptée pour son espérance, probabilité ×
pénalité, comme la tendance de `wxScore`), la mesure actuelle dans l'heure en cours. 6 graines, levier 3a
en référence :

| MAE +60 min | effet moyen | effet fort | effet fort, prévision **parfaite** |
|---|---|---|---|
| horizon pluvieux | 0,975 → 0,978 (+0,3 %) | 0,872 → 0,877 (+0,5 %) | 0,864 → 0,859 (−0,5 %) |
| pluie qui commence | 1,099 → 1,088 (−1,0 %) | 1,065 → 1,043 (−2,1 %) | 1,080 → 1,043 (−3,4 %) |
| pluie qui s'arrête | 1,003 → 1,011 (+0,8 %) | 0,931 → 0,939 (+0,8 %) | 0,942 → 0,953 (+1,1 %) |

Sur 2 h, « la météo actuelle persiste » est déjà presque la bonne prévision (les épisodes de pluie
simulés durent ~4 h). La prévision gagne quand la pluie commence et perd autant quand elle s'arrête —
**y compris quand elle est parfaite**. Hypothèse (non démontrée) : α est appris sur des fenêtres de
30 min de pluie *observée* et sous-estime l'effet ; prolonger le freinage après la fin de la pluie
compense en partie. Quoi qu'il en soit, **à 2 h, la prévision n'apporte rien de mesurable**.

### 4b — météo actuelle sur 2 h, météo prévue au-delà : gagne, livré

C'est là que la correction de l'étape 3 s'arrêtait faute d'information. Variante choisie au vu des
graines 1–6, **validée sur des graines neuves 7–9** :

| mesure (graines 7–9) | effet nul | effet moyen | effet fort |
|---|---|---|---|
| MAE +15 … +120 min | inchangées | inchangées | inchangées |
| MAE +240, tout | 2,261 → 2,261 | 2,238 → 2,236 (−0,1 %) | 2,227 → 2,222 (−0,2 %) |
| **MAE +240, horizon pluvieux** | 2,250 → 2,250 | 2,132 → **2,119 (−0,6 % ± 0,1)** | 2,067 → **2,037 (−1,5 % ± 0,1)** |
| MAE +240, pluie qui commence | 2,215 → 2,215 | 2,165 → 2,149 (−0,7 %) | 2,158 → 2,121 (−1,7 %) |

Petit, mais sans contrepartie et sur une zone de la courbe 24 h qui n'avait aucune information météo.
Au-delà des 6 heures annoncées : aucune correction. Tests : la correction suit la pluie annoncée et
**s'arrête quand elle cesse** (« reprise après la pluie »), et rien n'est corrigé si la pluie annoncée
s'arrête avant la vague du matin.

**Ce que la fiche peut dire, honnêtement.** « 12 vélos à 14 h, dont −4 liés à la pluie annoncée » n'est
juste qu'au-delà de 2 h (courbe 24 h). Sur la fiche (≤ 2 h), la part météo vient de la **météo
actuelle** : l'écran le dit (« liés à la météo actuelle »), puisque c'est elle, mesurée, qui prévoit le
mieux à cet horizon.

## Affichage (provenance de la part météo)

- **Fiche station** : une ligne « Météo : dont +1 vélo à 16:42 lié à la météo actuelle (Pluie 1,4 mm/h) ·
  coefficient appris 0,55 ± 0,12 (184 fenêtres pluvieuses) » — part à +2 h (bout de l'horizon de la
  fiche) ; sinon la raison de l'absence : « météo indisponible : prévision sans météo », « météo de plus
  de 45 min », « effet de la météo pas encore appris (trop peu de pluie observée) », « non mesurable sur
  ce parc », « aucun effet notable (temps normal pour l'heure…) ».
- **Prévision 24 h** : ligne « MÉTÉO » avec la part la plus marquée de la courbe ; au-delà de 2 h, elle
  est dite « liée à la météo annoncée » (sans citer la météo actuelle, qui ne la décrit pas).
- **Provenance « Prévision +1 h »** : la règle inclut la part météo ; le détail donne l'état de la météo
  (apprise / pas encore apprise / sans effet mesurable / indisponible / périmée) et la part totale en vélos.
- Captures : `docs/captures/37_fiche_meteo.png`, `38_prevision_24h_meteo.png`,
  `39_provenance_prevision_meteo.png`, `40_fiche_meteo_indisponible.png` ; fiches existantes mises à jour
  (`09`, `33`, `36`). L'état « coefficient appris » des captures est **injecté** par le scénario du
  simulateur (le jeu de données réel n'a pas d'historique météo) ; le scénario vérifie le texte affiché.
