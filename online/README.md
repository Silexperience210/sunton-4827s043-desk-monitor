# VelohNav Ops en ligne

Le serveur qui ne s'éteint jamais : il relève les flux Vel'OH! **24 h/24 avec le cœur du firmware**
(`src/core`, compilé en WebAssembly, pas une réécriture), garde ce qu'il apprend, sert **le jumeau de
l'écran** dans un navigateur et **resynchronise les Sunton** au démarrage.

Pourquoi : une carte éteinte le week-end n'apprend jamais le profil du samedi ni du dimanche, perd son
historique 24 h (relu seulement s'il a moins de 20 h) et laisse des trous dans la vue Service. Le
serveur, lui, voit tout ; la carte lui reprend ce qui est meilleur que ce qu'elle a.

```
            flux GBFS + Open-Meteo (mêmes URL, mêmes périodes que la carte)
                              │
                ┌─────────────▼──────────────┐
                │  serveur (Node, sans dép.) │  vn-core.wasm = src/core
                │  apprend 24 h/24           │  data/vn_prof.bin · vn_hist.bin · vn_days.bin
                └──┬───────────────┬─────────┘
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

**Umbrel** : l'app *Portainer* (App Store) accepte ce `docker-compose.yml` tel quel (Stacks → Add stack →
coller le fichier, chemin du volume en absolu, par exemple `/home/umbrel/velohnav/data:/data`).

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
| `VN_SAVE_S` | `1800` | sauvegarde sur disque (la carte : 30 min) ; aussi à l'arrêt |
| `VN_ARCHIVE_HOURS` | `6,18` | instantanés pour le banc (heure de Luxembourg) ; vide = aucun |
| `VN_ARCHIVE_KEEP_DAYS` | `0` | durée de l'archive (0 = tout garder, ~0,9 Mo par jour) |
| `VN_NAME` | `VelohNav Ops en ligne` | nom montré aux cartes et au jumeau |

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

## Affiner les prévisions avec l'archive

À 6 h et 18 h, le serveur range un instantané dans `data/archive/AAAAMMJJ-HHMMSS/` — la disposition de
`tools/carte_dump.py`, sans câble série. Le banc les rejoue directement :

```bash
cd sim && make bench_prevision
./bench_prevision --reel ../online/data/archive ../online/data/archive/<le plus ancien>/vn_prof.bin
```

## Développement

```bash
online/engine/build.sh            # reconstruit public/vn-core.wasm et public/vn-twin.wasm (clang + wasi-sysroot)
node online/test/replication.test.mjs   # serveur ⇄ jumeau : mêmes octets, 3 jours simulés
cd sim && make sync_sim && cd .. && node online/test/sync.e2e.mjs   # carte simulée ⇄ vrai serveur
```

Fichiers : `server.mjs` (HTTP, sauvegarde, archive, import), `lib/collector.mjs` (la tâche réseau de la
carte), `lib/sync.mjs` (instantanés, manifeste, registre), `engine/` (API C du moteur, plateforme
WebAssembly, jumeau LVGL), `public/` (page, moteurs, enveloppe JS).
