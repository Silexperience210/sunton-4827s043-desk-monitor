# Feuille de route — VelohNav Ops après la 1.3.0

Décidé le 26/10/2026 : on fait tout ce qui suit, dans cet ordre. Chaque chantier dit **où** il vit
(serveur, Sunton, téléphone), **ce qu'il mesure** et **comment on prouve qu'il marche sur le vrai parc**.

## Le principe : rien d'approximatif présenté comme un fait

Chaque chiffre affiché, envoyé ou archivé appartient à une seule de ces classes, et l'écran le dit :

| classe | d'où il vient | exemples |
|---|---|---|
| **mesuré** | lu dans le flux GBFS ou Open-Meteo, ou calculé de ces relevés par une règle écrite | vélos en station, stations·heures à vide, retraits **observés** (un minimum : deux mouvements entre deux relevés ne se voient pas) |
| **déclaré** | saisi par une personne identifiée, horodaté par son appareil | « Hamilius : 6 vélos déposés, 9 h 42, régulateur Paul » |
| **estimé** | un modèle ; toujours dit comme tel, avec sa méthode et son erreur **mesurée** | prévision à +1 h (erreur notée en continu), vélos en trajet, besoin conseillé |

Interdit, quelle que soit la tentation :

- **déduire une intervention d'un saut dans le flux.** Neuf cyclistes qui rendent leurs vélos ensemble ressemblent
  à un camion qui en dépose neuf. Une intervention n'existe que si elle est **déclarée** (chantier 1) ; le flux
  sert seulement à la **recouper** (« déclaré +6, observé +6 dans les 5 min »), et les deux sont montrés à part.
  La règle « rééquilibrage probable » du banc reste ce qu'elle est : un outil pour construire la vérité du monde
  simulé, jamais un indicateur ;
- **afficher un contrefactuel comme une mesure.** « Ruptures évitées » ne s'observe pas (on ne voit pas ce qui se
  serait passé sans le camion). Si on le montre un jour, c'est en **estimé**, avec la méthode, à côté des mesures ;
- **prouver par la simulation.** Le banc simulé sert à écarter une idée qui ne marche même pas dans un monde
  simplifié ; la preuve se fait sur le vrai parc : journal des prévisions en continu, interventions déclarées,
  banc de nuit sur l'archive réelle.

Si une donnée n'est pas fiable, on ne l'affiche pas, et on dit pourquoi (comme « — · estimation après une nuit »
pour les vélos en trajet). Mieux vaut abandonner une fonction que la faire mentir.

## Les rôles

| | fait | ne fait pas |
|---|---|---|
| **Serveur** (Umbrel, 24 h/24) | relève et apprend sans interruption ; historique long ; plan des tournées (calcul lourd) ; registre des interventions déclarées et recoupement avec le flux ; note des prévisions, banc de nuit, rapports ; notifications ; mises à jour et réglages des Sunton | ne devine rien qu'une personne doit déclarer |
| **Sunton « poste »** (bureau, dépôt) | supervision, alertes, vue Service ; reste autonome si le serveur tombe (son propre modèle, ses fichiers) ; se resynchronise | ne dépend pas du serveur pour fonctionner |
| **Sunton « régulateur »** (dans le camion) | affiche la tournée et l'état des prochains arrêts ; **fait valider chaque arrêt** (vélos réellement pris ou déposés) ; garde les validations hors connexion et les envoie dès que le réseau revient | pas de calcul de tournée, pas de GPS (l'arrêt est confirmé par le régulateur) |
| **Téléphone** | notifications ntfy ; page de secours pour valider un arrêt si le Sunton manque | |

## Où on en est (27/10/2026)

| chantier | état |
|---|---|
| 1. Interventions déclarées | **livré dans le code** : registre et recoupement sur le serveur, rôle « régulateur » du firmware (écran Tournée, file hors connexion en flash), page de secours sur le téléphone, onglet Régulation. À éprouver sur le terrain. Mode d'emploi : [`regulation.md`](regulation.md) |
| 2. Tournées de régulation | **livré dans le code** : plan pour 1 ou 2 camions (capacité du camion du jour, chargement déclaré, atelier), recalculé toutes les 30 s à partir du déclaré, présenté comme une estimation avec sa marge d'erreur mesurée. Reste : la vérification « pas pire que les transferts au plus proche » sur l'archive réelle, puis la mesure avant / après sur le vrai parc |
| 0, 3 – 8 | à faire, dans l'ordre |

Restes connus des chantiers 1 et 2, dits plutôt que cachés :

- **un jeton par appareil** : aujourd'hui un seul jeton serveur (`VN_TOKEN`), chaque appareil s'annonce par son
  identifiant (`X-VN-Device`) ; la feuille de route prévoyait un jeton par appareil ;
- indicateurs à ajouter : vélos déplacés **par régulateur**, ruptures du jour **sans** intervention ;
- la séparation des retraits observés (régulation déclarée / clients) dans l'historique ;
- le point exact de l'atelier, rue des Romains, reste à saisir (onglet Régulation → Réglages).

## Les chantiers

### 0. Prérequis : recette sur le terrain, puis 2 à 4 semaines de vraies données

- Flasher la 1.3.0 sur une carte ; synchro avec le serveur (transport HTTP jamais essayé sur la carte) ;
  une vraie notification ntfy reçue ; plein écran du jumeau sur le téléphone ; la recette de `docs/bilan-final.md` § 4.
- Laisser le serveur tourner : le journal des prévisions, l'historique long et le banc de nuit travaillent
  alors sur le **vrai** parc, qui est plus dispersé que le monde simulé (15 stations vides contre 3 à 14 h 30).

### 1. Interventions déclarées — la fondation

- **Registre** sur le serveur : station, sens (prise / dépose), nombre réel, heure de l'appareil, régulateur,
  appareil, tournée et arrêt prévus s'il y en a. Écriture seulement par un appareil authentifié (jeton par appareil).
- **Sunton régulateur** : un rôle du firmware (Réglages → Rôle). Écran « prochain arrêt » : station, consigne,
  état actuel de la station (mesuré) ; boutons − / + pour le nombre **réel** ; « Fait » ; « Impossible »
  (station inaccessible, camion plein…) avec un motif. File d'attente en flash si la connexion manque.
- **Recoupement** : après chaque validation, le serveur regarde le flux de la station sur les 10 minutes
  suivantes et classe : *confirmé* (variation compatible), *non visible* (flux muet, station hors service),
  *écart* (déclaré +6, observé +2). Rien n'est corrigé en silence : la déclaration reste, l'observation est à côté.
- **Indicateurs, seulement sur du déclaré + du mesuré** : vélos déplacés par jour et par régulateur ; délai entre
  le début d'une rupture (mesuré) et l'intervention déclarée qui l'a traitée ; ruptures du jour **sans**
  intervention ; part des arrêts confirmés par le flux.
- **Matériel du camion** (à vérifier sur place) : Sunton + alimentation USB 12 V ; connexion par le partage du
  téléphone ou un petit routeur 4G ; lisibilité au soleil ; la version **résistive** (`-r`) se manie avec des gants,
  la capacitive non.
- **L'outil interne de JCDecaux** (fermé, libération des vélos seulement) ne remplace pas la déclaration : voir
  les décisions plus bas.

### 2. Tournées de régulation

- Le serveur planifie, pour chaque camion et sa capacité, une suite d'arrêts (prendre / déposer, combien), en
  visant l'état **prévu à l'heure d'arrivée** ; il replanifie quand une validation s'écarte du plan ou qu'une
  rupture nouvelle apparaît. Le Sunton régulateur reçoit la tournée ; chaque arrêt validé alimente le chantier 1.
- Ce qui se mesure sur le vrai parc : temps de traitement des ruptures (avant / après), ruptures et stations·heures
  à vide par jour, écarts entre le plan et le réel. Les comparaisons avant / après sont faites à jours comparables
  (même jour de la semaine, météo voisine) et présentées comme des comparaisons, pas comme une preuve de cause.
- Le simulateur sert seulement à vérifier, avant le terrain, que le plan ne fait pas pire que les transferts
  « au plus proche » d'aujourd'hui.

### 3. Rapport mensuel automatique

Le 1er du mois : disponibilité selon les définitions de la vue Service, stations et créneaux en difficulté,
problèmes récurrents, comparaison au mois précédent, interventions déclarées et leurs délais ; tableaux bruts en
annexe ; chaque chiffre avec sa classe (mesuré / déclaré / estimé). Lien envoyé par notification.

### 4. Mises à jour sans câble et réglages communs

Firmware servi par le serveur, vérifié (taille, CRC, signature) avant l'écriture dans l'emplacement libre (les
tables de partition `default_8MB` / `default_16MB` en ont deux) ; retour automatique à l'ancien si le nouveau ne
démarre pas. Réglages (seuils, rôle, nom de l'écran) décidés une fois pour tous les Sunton. À éprouver sur une
carte de test avant toute la flotte.

### 5. Rejouer une journée sur une vraie carte

Dans la page : fond de carte OpenStreetMap, curseur sur n'importe quel jour au pas de 5 min, stations colorées
vide → pleine, interventions déclarées sur la frise ; comparer deux jours côte à côte. Tout vient de l'historique
long (mesuré) et du registre (déclaré).

### 6. Calendrier : vacances scolaires et événements

Vacances scolaires (calendrier officiel du ministère, comme les fériés) comme type de jour ; événements
(Schueberfouer, concerts, matchs, marathon) saisis à la main au départ — donc **déclarés**. Livrés seulement si le
banc sur l'archive **réelle** et le journal en continu montrent un gain, avec les verdicts appariés habituels.

### 7. Prévision en probabilité, puis modèle serveur

- « Risque de rupture dans l'heure : 70 % », avec la vérification que les 70 % annoncés se réalisent bien 70 fois
  sur 100 (journal en continu) ; sinon on ne l'affiche pas.
- Un modèle entraîné sur des mois d'historique, noté exactement comme celui de la carte ; servi aux Sunton
  **seulement s'il gagne** sur au moins 4 semaines réelles. La carte garde son modèle pour le hors connexion.

### 8. Alertes prises en charge

Bouton « Je m'en occupe » dans la notification ntfy et sur les Sunton ; « Hamilius : pris en charge par Paul,
9 h 40 » visible partout (déclaré), pour ne pas envoyer deux camions au même endroit.

### Petits correctifs (bilan final, § 3)

- Griser la température de l'en-tête au-delà de 45 min (même seuil que le modèle).
- « Flash utilisée 420 ko sur 3 456 ko ».
- Classement : mettre à part les stations hors service tout le mois.
- « Coefficient appris, à confirmer » tant qu'il n'a pas tenu 14 jours.

## Décisions (26/10/2026)

- **1 à 2 camions** en même temps : le plan des tournées (chantier 2) reste un petit problème, résolu
  exactement ou presque sur le serveur, pas une heuristique grossière.
- **Connexion : partage du téléphone du régulateur.** Le Sunton régulateur doit donc supporter les coupures
  (tunnel, téléphone en veille, batterie) : chaque validation est écrite en flash d'abord, envoyée ensuite, et
  l'écran dit combien attendent l'envoi. Réglage Wi-Fi du Sunton : le point d'accès du téléphone.
- **Terminal : Sunton dédié dans le camion, téléphone en secours** (même API, même registre).
- **Capacité : 10 à 25 vélos selon le camion utilisé.** La capacité n'est donc pas une constante : au départ de
  la tournée, le régulateur choisit sur son Sunton le camion du jour (liste des camions et de leur capacité tenue
  sur le serveur) et **déclare le chargement de départ** (vélos déjà à bord). Le serveur planifie avec ces deux
  valeurs déclarées, et le chargement à bord se tient à jour à chaque arrêt validé (prise +n, dépose −n) ;
  l'écran l'affiche (« à bord : 14 / 25 ») et refuse une validation qui le rendrait négatif ou supérieur à la
  capacité, avec un message plutôt qu'une correction silencieuse.

- **L'atelier fait partie de la tournée.** Le régulateur y dépose des vélos et en reprend de temps en temps des
  neufs. La tournée a donc deux sortes d'arrêts :
  - **station** : vélos en service déposés ou pris, et vélos **hors service** pris (le flux donne
    `num_vehicles_disabled` par station : de quoi recouper, comme pour les autres) ;
  - **atelier** : vélos déposés (hors service ou à réviser), vélos **neufs** (ou réparés) repris.

  Le chargement à bord se tient par catégorie (en service / hors service), dans la même capacité. Le serveur
  planifie un passage à l'atelier quand le camion doit décharger des vélos hors service ou recharger des vélos
  neufs, et seulement s'il y en a de prêts — le stock prêt à l'atelier est **déclaré** (le flux GBFS ne voit pas
  l'atelier).

  Ce que ça apporte en plus, sans rien deviner : les vélos absents des stations se décomposent en
  « à l'atelier » (**déclaré** : entrés − sortis) et le reste (en trajet ou inconnu, **estimé** comme aujourd'hui),
  et les vélos neufs mis en service sont comptés (**déclaré**) au lieu d'apparaître comme un parc qui grossit.

- **L'outil interne de JCDecaux ne peut pas servir de source.** Il est fermé, et les régulateurs ne s'en servent
  que pour **libérer** les vélos (les décrocher des bornes) : pas de déposes, pas d'atelier, pas de compte par arrêt,
  et aucun accès pour le lire. Le **Sunton régulateur reste donc la source déclarée** de toutes les interventions.
  Conséquences :
  - **le moins de double saisie possible** : à chaque arrêt, le Sunton propose les nombres prévus par la tournée,
    le régulateur confirme d'un toucher ou corrige avec − / + ; la libération dans l'outil JCDecaux reste
    nécessaire pour décrocher les vélos, notre déclaration ne la remplace pas ;
  - **le recoupement marche dans les deux sens** : une libération fait baisser les vélos de la station dans le flux,
    une dépose les fait monter ;
  - **les retraits observés se séparent enfin honnêtement** : une baisse observée dans une station qui correspond à
    une prise déclarée (même station, quelques minutes) est comptée comme régulation (**déclaré**), le reste comme
    retraits des clients (**mesuré**, toujours un minimum). Aujourd'hui, une prise du camion compte comme
    retrait de client ;
  - **les vélos absents des stations se décomposent** : dans le camion (**déclaré**, le chargement à bord), à
    l'atelier (**déclaré**), le reste en trajet ou inconnu (**estimé**).
  - Si JCDecaux fournit un jour un export ou une API de ses libérations, on le branche comme **seconde** source
    déclarée, pour recouper — pas pour remplacer la déclaration complète.

- **Un seul atelier : rue des Romains, à Bertrange.** Le point exact est **déclaré** dans les réglages du serveur
  (numéro, ou point posé sur la carte) — jamais deviné par un géocodage approximatif : la rue est longue, et
  quelques centaines de mètres changent l'ordre d'une tournée.

## Encore à trancher

1. Point exact de l'atelier, rue des Romains : se colle une fois dans l'onglet Régulation → Réglages (coordonnées
   ou lien OpenStreetMap / Google Maps). Tant qu'il manque, le plan ne propose pas d'arrêt à l'atelier.
