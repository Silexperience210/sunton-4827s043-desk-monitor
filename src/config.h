// ── VelohNav Ops — configuration globale ─────────────────────────────
// Supervision temps réel du parc Vel'OH! (JCDecaux · Luxembourg)
// Cible : Sunton ESP32-4827S043 (ESP32-S3 N16R8, dalle RGB 480×272, tactile)
#pragma once

#define VN_VERSION      "1.2.7"
#define VN_NAME         "VelohNav Ops"

// ── Données ──────────────────────────────────────────────────────────
// GBFS v3 public (sans clé) — mêmes flux que l'app VelohNav (src/utils/gbfs.js)
#define VN_GBFS_BASE    "https://api.cyclocity.fr/contracts/luxembourg/gbfs/v3"
// Météo Open-Meteo (gratuit, sans clé) — Luxembourg-Ville
#define VN_WX_URL       "https://api.open-meteo.com/v1/forecast?latitude=49.6116&longitude=6.1319" \
                        "&current=temperature_2m,precipitation,wind_speed_10m,wind_direction_10m,weather_code" \
                        "&hourly=temperature_2m,precipitation_probability,precipitation,weather_code,wind_speed_10m" \
                        "&forecast_hours=6&wind_speed_unit=kmh&timezone=Europe%2FLuxembourg"

#define VN_MAX_ST            200   // emplacements de stations (148 dans le flux aujourd'hui)
#define VN_INFO_REFRESH_S   3600   // station_information (statique)
#define VN_TYPES_REFRESH_S 86400   // vehicle_types
#define VN_WX_REFRESH_S      900   // météo
#define VN_SAVE_PERIOD_S    1800   // sauvegarde historique + profils en flash

// ── Heure locale (Luxembourg) ────────────────────────────────────────
#define VN_TZ           "CET-1CEST,M3.5.0,M10.5.0/3"
#define VN_NTP1         "pool.ntp.org"
#define VN_NTP2         "time.google.com"

// ── Historique / prédiction ──────────────────────────────────────────
#define VN_RING_SLOTS     288   // 24 h au pas de 5 min
#define VN_RING_STEP_S    300
// Profondeur d'historique RÉELLEMENT OBSERVÉE (tranches valides × 5 min, trous exclus) exigée
// pour publier « vélos en trajet ». Une carte éteinte 12 h ne doit pas compter 12 h d'historique
// sur trois relevés : un écart d'horloge n'est pas une observation.
#define VN_INUSE_MIN_MIN  360   // validé : 6 h de tranches observées ET un creux (quart le moins actif)
#define VN_INUSE_PROV_MIN 360   // provisoire : même seuil que la validation — le correctif de profondeur
                                // ne change QUE la mesure, pas le moment où le chiffre paraît
#define VN_INUSE_ORDER      6   // parc = 6ᵉ plus grande valeur comparable : tolère jusqu'à cinq tranches
                                // aberrantes sur 24 h (deux artefacts indépendants de deux tranches, une
                                // station fantôme d'un quart d'heure). Avec K = 3, deux artefacts
                                // indépendants faisaient annoncer 400 vélos en trajet pour 200 réels.
#define VN_RECENT_SLOTS    60   // 60 min au pas de 1 min (tendance court terme)
#define VN_PROFILE_DECAY_N 24   // constante de temps du profil en relevés (3 par semaine et par créneau) : 8 semaines
#define VN_FC_STEPS        25   // prévision 0…120 min au pas de 5 min
#define VN_MAX_ALERTS      96
#define VN_MAX_TRANSFERS    8
#define VN_DAYS            60   // résumés par jour gardés (vue Service, core/days.h) : 2 mois

// ── Recherche Wi-Fi (voir core/wifiscan.h) ───────────────────────────
#define VN_SCAN_MAX        40   // réseaux affichés (après dédoublonnage et tri ; au-delà, les plus faibles)
#define VN_SCAN_TRIES       5   // tentatives quand la radio répond « en cours » (‑1) ou « refusé » (‑2) :
                                // 4 × 1,5 s couvrent le délai interne du framework (6 s) après lequel
                                // scanComplete() libère un drapeau « en cours » resté levé
#define VN_SCAN_RETRY_MS 1500   // attente entre deux tentatives « en cours »
#define VN_SCAN_SETTLE_MS  150  // délai après la suspension de l'association (la radio doit rendre la main)
