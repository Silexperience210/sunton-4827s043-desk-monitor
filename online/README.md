# VelohNav Ops en ligne

Le serveur qui ne s'éteint jamais : il relève les flux Vel'OH! **24 h/24 avec le cœur du firmware**
(`src/core`, compilé en WebAssembly, pas une réécriture), garde ce qu'il apprend, sert **le jumeau de
l'écran** dans un navigateur et **resynchronise les Sunton** au démarrage.

Pourquoi : une carte éteinte le week-end n'apprend jamais le profil du samedi ni du dimanche, perd son
historique 24 h (relu seulement s'il a moins de 20 h) et laisse des trous dans la vue Service. Le
serveur, lui, voit tout ; la carte lui reprend ce qui est meilleur que ce qu'elle a.

Et parce qu'il ne s'éteint jamais, il fait aussi ce qu'une carte ne peut pas faire :

- **historique long** : chaque tranche de 5 min gardée sans limite, des courbes sur des semaines ;
- **erreur des prévisions mesurée en continu**, et chaque nuit **le banc** rejoue l'archive ;
- **notifications sur le téléphone** (ntfy) : station vide ou pleine qui dure, flux muet, Sunton
  resynchronisé, et un **résumé chaque matin**.

```
            flux GBFS + Open-Meteo (mêmes URL, mêmes périodes que la carte)
                              │
                ┌─────────────▼──────────────┐
                │  serveur (Node, sans dép.) │  vn-core.wasm = src/core
                │  apprend 24 h/24           │  data/vn_prof.bin · vn_hist.bin · vn_days.bin
                │                            │  data/historique · prevision · banc · archive
                └──┬───────────────┬─────┬───┘
                   │               │     └──▶ ntfy : ruptures, flux, résumé du matin
   évènements      │               │  /sync/manifest + fichiers (CRC32)
   (rejeu exact)   │               │  ⇅ envoi de ce que la carte connaît mieux
        ┌──────────▼─────┐   ┌─────▼──────────┐
        │ jumeau d'écran │   │ Sunton 1, 2, … │
        │ (navigateur)   │   │ (core/sync)    │
        └────────────────┘   └────────────────┘
```

## Lancer

Sur une machine allumée en permanence (Umbrel, Raspberry Pi, NAS, petit VPS) :

```bash
cd online
docker compose up -d                 # → http://<machine>:8790
# ou, sans Docker (Node ≥ 18, aucune dépendance) :
node server.mjs
```

**Umbrel** : avec l'app *Portainer* (App Store) : Stacks → Add stack → **Repository**, adresse de ce dépôt,
fichier `online/docker-compose.yml` (l'image se construit depuis le dépôt : le banc de nuit est compilé
avec). Mettre le volume en chemin absolu, par exemple `/home/umbrel/velohnav/data:/data`. Ou, en SSH sur
l'Umbrel : `git clone` du dépôt puis `cd online && docker compose up -d`.

**Partir de ce que la carte sait déjà** (sinon le serveur apprend depuis zéro, et c'est la carte qui
l'amorce à sa première synchro si « Partager l'apprentissage » est actif) :

```bash
.venv/bin/python tools/carte_dump.py --sortie /tmp/vidage     # vidage série de la carte
node online/server.mjs --import /tmp/vidage                  # puis lancer le serveur normalement
```

| Variable | Défaut | Rôle |
|---|---|---|
| `VN_PORT` / `VN_HOST` | `8790` / `0.0.0.0` | écoute |
| `VN_DATA` | `./data` (`/data` en Docker) | état appris, registre des écrans, archive |
| `VN_TOKEN` | — | jeton exigé par `/sync` et `/api` (Bearer). À définir dès que le port sort du réseau local |
| `VN_REFRESH_S` | `60` | période de `station_status` (30 s au moins), comme Réglages → Rafraîchissement |
| `VN_SAVE_S` | `600` | sauvegarde sur disque (la carte : 30 min) ; aussi à l'arrêt (`docker stop`, Ctrl-C) |
| `VN_ARCHIVE_HOURS` | `6,18` | instantanés pour le banc (heure de Luxembourg) ; vide = aucun |
| `VN_ARCHIVE_KEEP_DAYS` | `0` | durée de l'archive (0 = tout garder, ~0,9 Mo par jour) |
| `VN_NAME` | `VelohNav Ops en ligne` | nom montré aux cartes et au jumeau |
| `VN_NIGHT_AT` | `03:30` | tâches de nuit (heure de Luxembourg) : jours finis compressés, prévisions de la veille notées, banc |
| `VN_BENCH_BIN` | (image Docker : réglé) | exécutable `sim/bench_prevision` ; absent = pas de banc de nuit |
| `VN_NTFY_URL` / `VN_NTFY_TOKEN` | — | sujet ntfy (`https://ntfy.sh/<sujet>`) et jeton d'un serveur ntfy protégé |
| `VN_PUBLIC_URL` | — | adresse de la page, ouverte en touchant une notification |
| `VN_NOTIFY_DIGEST` | `07:30` | heure du résumé du matin ; vide = aucun |
| `VN_NOTIFY_MIN` / `VN_NOTIFY_HOURS` / `VN_NOTIFY_MAX_H` | `15` / `7-22` / `6` | rupture signalée après N min, dans ces heures, au plus N messages par heure |

Les réglages des notifications se font aussi (et plus simplement) depuis la page, onglet **Notifications** ;
ils sont gardés dans `data/notify.json` et priment sur les variables.

## Le jumeau de l'écran (`http://<machine>:8790`)

C'est l'interface de la carte, **le même code `src/ui`** (LVGL 8.3 compilé en WebAssembly), dans un
cadre : toucher, glisser, ouvrir les fiches, tout fonctionne comme sur la dalle 480 × 272. Le jumeau
reprend l'état du serveur, puis **rejoue chaque appel du serveur au moteur, au même instant** : ses
chiffres sont ceux du serveur au bit près (`test/replication.test.mjs` le vérifie octet pour octet sur
trois jours simulés). La pastille LIVE de l'écran demande un relevé immédiat au serveur ; Réglages →
Synchro en ligne → Maintenant fait reprendre l'état. Les réglages modifiés sur le jumeau (seuils,
fraîcheur) restent dans ce navigateur.

À côté : l'état du serveur (dernier relevé, apprentissage, jours résumés, archive), **les écrans
Sunton qui se sont synchronisés** (dernière synchro, fichiers adoptés ou envoyés) et les fichiers de la
carte à télécharger.

## Exposer la page sur Internet (Cloudflare Tunnel)

Le serveur doit tourner en continu (il relève toutes les 60 s et écrit sur disque) : c'est le rôle de
l'Umbrel, pas d'un hébergeur « serverless » comme Vercel (pas de processus permanent, pas de disque). Pour
ouvrir la page depuis le téléphone hors de chez soi, **Cloudflare Tunnel** la publie en https sans ouvrir
de port sur la box :

1. **Définir `VN_TOKEN`** dans `docker-compose.yml` (un long mot de passe) : la page le demandera une fois,
   les Sunton aussi (`sync jeton …`). Indispensable dès que la page sort du réseau local.
2. Essai sans compte : `docker compose --profile essai up -d`, puis
   `docker compose logs tunnel-essai | grep trycloudflare` → une adresse `https://….trycloudflare.com`
   (au hasard, change à chaque redémarrage).
3. Adresse fixe : compte Cloudflare gratuit et un domaine géré par Cloudflare ; Zero Trust → Networks →
   Tunnels → *Create a tunnel* (cloudflared) → copier le jeton ; *Public hostname* → service
   `http://velohnav-ops:8790`. Puis `TUNNEL_TOKEN=… docker compose --profile tunnel up -d` (ou le jeton dans
   un fichier `.env` à côté du compose). Mettre cette adresse dans `VN_PUBLIC_URL` pour que les
   notifications l'ouvrent.

## Historique long (onglet Historique)

La carte ne garde que 24 h ; le serveur écrit **chaque tranche de 5 min** de l'anneau dans
`data/historique/AAAA-MM-JJ.jsonl` (jour de Luxembourg), compressé en `.jsonl.gz` une fois le jour fini
(≈ 35 Ko par jour, ≈ 13 Mo par an). Une tranche est écrite quand la suivante s'ouvre — retraits et
retours compris — avec exactement ce que l'anneau contient : vélos de chaque station (le premier relevé
des 5 min, la règle de `Model`), totaux du réseau, météo. Au premier démarrage, **l'archive existante
est versée dans l'historique** (chaque instantané contient 24 h) ; à la main, aussi pour des vidages de
cartes :

```bash
node online/server.mjs --import-archive                 # data/archive (par défaut)
node online/server.mjs --import-archive /chemin/vidages # dossiers de tools/carte_dump.py
```

La page trace le réseau ou une station (24 h, 7 jours, 4 semaines, 3 mois), les stations·heures en
rupture et les retraits jour par jour, avec le tableau équivalent et l'export CSV.

## Erreur des prévisions (onglet Prévisions)

Tous les quarts d'heure de 6 h à 22 h, au premier relevé de la tranche, le serveur note ce que l'écran
afficherait : **prévision à +15 min, +30 min, +1 h, +2 h et +4 h**, la **persistance** (le relevé lui-même)
et le **profil seul** — les trois colonnes de `sim/bench_reel.cpp`, avec la même règle (prévision arrondie
− vélos de l'anneau à t + h). Quand l'heure visée est dans l'historique, l'erreur est connue :

- par horizon, jour par jour, par heure de la journée et par station (les plus dures à prévoir) ;
- fiabilité de l'alerte « vide / pleine dans l'heure » : part confirmée, part des ruptures annoncées ;
- chaque nuit, la journée de la veille est notée et gardée (`data/prevision/note/AAAA-MM-JJ.json`).

**Banc de nuit** : l'image Docker compile `sim/bench_prevision` (le même cœur) et le lance chaque nuit
sur l'archive, en partant du profil du plus ancien instantané (le banc écarte les tranches que ce profil
connaît déjà : pas de fuite). Rapport complet (`data/banc/AAAA-MM-JJ.md`) et extrait affiché sur la page :
MAE par horizon, par jour, et les verdicts appariés. Il faut au moins deux instantanés (une journée).

## Notifications (onglet Notifications)

Sur le téléphone, via **ntfy** (application libre, iPhone et Android ; serveur public ntfy.sh ou privé,
il existe une app Umbrel) :

1. installer ntfy et s'abonner à un sujet difficile à deviner (`velohnav-ops-7f3k`) ;
2. onglet Notifications : `https://ntfy.sh/velohnav-ops-7f3k`, Enregistrer, **Envoyer un test**.

| Message | Quand |
|---|---|
| Station vide / pleine | rupture qui dure depuis 15 min (réglable), dans les heures réglées ; plusieurs dans le même cycle → un seul message ; une fois par épisode |
| Rétablie | fin d'une rupture signalée (priorité basse) |
| Bientôt vide / pleine | échéance ≤ 30 min (désactivé par défaut : bavard) |
| Flux muet / figé / rétabli | plus aucun relevé depuis 3 périodes, ou plus aucune station ne bouge — même la nuit |
| Sunton resynchronisé | un écran a repris l'état du serveur (profils, historique, jours) |
| Résumé du matin | la veille (disponibilité, ruptures, pires stations, problèmes récurrents — les phrases de la vue Service), l'erreur de la prévision, l'état du moment, les écrans |

Garde-fous : au démarrage, les ruptures déjà en cours ne sont pas annoncées ; au plus 6 messages par heure
(les suivants attendent et partent groupés) ; filtre de stations. Un **webhook** générique reçoit le même
objet JSON (Home Assistant, n8n…).

## Régulation (onglet Régulation, `/regulateur.html`, Sunton du camion)

Le registre des **interventions déclarées** et le **plan des tournées** : mode d'emploi, captures, règles et API
dans [`docs/regulation.md`](../docs/regulation.md). En bref :

- onglet **Régulation** : camions et capacités, régulateurs, point exact de l'atelier (coordonnées ou lien
  collé, jamais géocodé), stock déclaré ; tournées en cours et leur plan (**estimé**) ; déclarations et leur
  recoupement avec le flux (déclaré et observé côte à côte) ; indicateurs, chacun avec sa classe ;
- **Sunton du camion** : Réglages → Rôle → Régulateur ; l'onglet « Tournée » montre le prochain arrêt et fait
  valider ce qui a vraiment été fait ; tout attend en flash quand le partage de connexion coupe ;
- **téléphone** : `http://<machine>:8790/regulateur.html`, même API, même file hors connexion, peut rejoindre la
  tournée du camion.

## Brancher un Sunton (firmware 1.3.0)

Réglages → **Synchro en ligne** → Serveur : saisir `http://<machine>:8790` (ou, en série,
`sync url http://<machine>:8790`, puis `sync jeton …` si `VN_TOKEN` est défini). Au démarrage, avant de
relire son historique, la carte :

1. lit `GET /sync/manifest` (relevés appris, date de l'historique, jours résumés, taille et CRC32 de chaque fichier) ;
2. décide **fichier par fichier** :
   - profils (`/vn_prof.bin`) : ceux du serveur s'il a **strictement plus de relevés** ;
   - historique 24 h (`/vn_hist.bin`) : celui du serveur s'il est **plus récent d'au moins une minute** et a moins de 20 h ;
   - résumés par jour (`/vn_days.bin`) : ceux du serveur s'il a **plus de jours**, ou autant mais plus récents ;
3. télécharge, vérifie (taille, CRC32, puis les contrôles d'en-tête de `Model::load`) et seulement ensuite
   écrit — un fichier faux n'efface jamais celui de la flash ;
4. si « Partager l'apprentissage » est actif, **envoie** ses profils ou ses jours quand ils valent mieux
   que ceux du serveur, qui les vérifie à son tour, les adopte et prévient les jumeaux ;
5. rend compte au serveur (registre des écrans), puis relit ses fichiers comme d'habitude.

« Maintenant » fait la même chose en cours de route (sauvegarde d'abord) et redémarre la carte si elle a
adopté un fichier. Serveur injoignable : la carte démarre avec ce qu'elle a (connexion abandonnée après 8 s,
lecture après 15 s).
Commandes série : `sync`, `sync etat`, `sync url …`, `sync jeton …`, `sync boot 0/1`, `sync envoi 0/1`.

> `.local` (mDNS) n'est pas toujours résolu par la carte : en cas d'échec, donner l'adresse IP.

## API

| Route | Jeton | Contenu |
|---|---|---|
| `GET /` | — | jumeau de l'écran + panneau serveur |
| `GET /healthz` | — | `200 ok` si le dernier relevé a moins de 3 périodes, `503` sinon |
| `GET /api/summary` | — | totaux, apprentissage, météo, compteurs d'ingestion (mêmes champs que la carte) |
| `GET /api/ring` | — | historique 24 h du réseau, au pas de 5 min |
| `GET /api/devices` | oui | écrans synchronisés |
| `GET /api/archive` | oui | instantanés archivés |
| `GET /api/export/vn_prof.bin` … `vn_stations.csv` | oui | fichiers de la carte, octets exacts |
| `POST /api/refresh` | oui | relevé immédiat |
| `GET /api/twin/snapshot`, `/api/twin/events?since=` | oui | réplication vers les jumeaux (attente longue 25 s) |
| `GET /sync/manifest`, `/sync/<id>/vn_*.bin`, `POST /sync/<id>/upload/vn_*.bin` | oui | synchro des cartes |
| `GET /api/history` | oui | jours gardés, taille sur disque |
| `GET /api/history/series?days=7&step=60&sid=` | oui | série agrégée (pas 5, 15, 30, 60, 180 ou 1440 min) du réseau ou d'une station |
| `GET /api/history/daily?days=28` | oui | résumé par jour (historique) + disponibilité (vue Service) |
| `GET /api/history/export.csv?days=28[&stations=1]` | oui | tranches en CSV (réseau, ou station par station) |
| `GET /api/stations` | oui | stations, relevé et dérivés (statut, échéances, besoin, risque) |
| `GET /api/forecast?days=14` | oui | erreur des prévisions : par horizon, par jour, par heure, stations, alertes |
| `GET /api/bench`, `/api/bench/report.md`, `POST /api/bench/run` | oui | banc de nuit : état, rapport, lancement |
| `GET`/`POST /api/notify`, `POST /api/notify/test`, `POST /api/notify/digest[?send=1]` | oui | notifications : réglages, test, résumé du matin |
| `GET`/`POST /reg/config`, `GET /reg/tour`, `POST /reg/tour/start`, `/reg/event`, `/reg/tour/end` | oui | régulation : réglages, tournée de l'appareil (`X-VN-Device`), déclarations — détail dans [`docs/regulation.md`](../docs/regulation.md) |
| `GET /reg/plan[?force=1]`, `GET /reg/state?days=7` | oui | plans des tournées ouvertes ; tournées, déclarations et indicateurs |
| `GET /regulateur.html` | — | page du régulateur (téléphone) |

## Affiner les prévisions avec l'archive

À 6 h et 18 h, le serveur range un instantané dans `data/archive/AAAAMMJJ-HHMMSS/` — la disposition de
`tools/carte_dump.py`, sans câble série. Le banc de nuit les rejoue ; à la main, avec une variante :

```bash
cd sim && make bench_prevision
./bench_prevision --reel ../online/data/archive ../online/data/archive/<le plus ancien>/vn_prof.bin --levier …
```

## Développement

```bash
online/engine/build.sh            # reconstruit public/vn-core.wasm et public/vn-twin.wasm (clang + wasi-sysroot)
node online/test/replication.test.mjs   # serveur ⇄ jumeau : mêmes octets, 3 jours simulés
cd sim && make sync_sim && cd .. && node online/test/sync.e2e.mjs   # carte simulée ⇄ vrai serveur
make -C sim bench_prevision && node online/test/features.test.mjs  # historique, prévisions, notifications, banc
node online/test/regulation.test.mjs                               # registre, recoupement, plan, HTTP
make -C sim reg_sim && node online/test/reg.e2e.mjs                # Sunton du camion simulé ⇄ vrai serveur
docker build -f online/Dockerfile -t velohnav-ops-online .          # image (depuis la racine du dépôt)
```

Fichiers : `server.mjs` (HTTP, sauvegarde, archive, import, tâches de nuit), `lib/collector.mjs` (la tâche
réseau de la carte), `lib/sync.mjs` (instantanés, manifeste, registre), `lib/history.mjs` (historique long,
import d'archive), `lib/forecast.mjs` (journal et note des prévisions), `lib/bench.mjs` (banc de nuit),
`lib/notify.mjs` (ntfy, webhook, résumé du matin), `lib/regulation.mjs` (registre, recoupement, indicateurs),
`lib/planner.mjs` (plan des tournées, estimé), `engine/` (API C du moteur, plateforme WebAssembly,
jumeau LVGL), `public/` (page, tableau de bord `board.js`, séries `vn-series.js`, page du régulateur `regulateur.html`,
moteurs, enveloppe JS).
