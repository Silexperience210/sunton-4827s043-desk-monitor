// ── VelohNav Ops — configuration globale ─────────────────────────────
// Supervision temps réel du parc Vel'OH! (JCDecaux · Luxembourg)
// Cible : Sunton ESP32-4827S043 (ESP32-S3 N16R8, dalle RGB 480×272, tactile)
#pragma once

#define VN_VERSION      "1.0.4"
#define VN_NAME         "VelohNav Ops"

// ── Écran ────────────────────────────────────────────────────────────
#define VN_SCREEN_W     480
#define VN_SCREEN_H     272

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
#define VN_RECENT_SLOTS    60   // 60 min au pas de 1 min (tendance court terme)
#define VN_PROFILE_CAP_N   24   // 3 relevés/jour/créneau → fenêtre ≈ 8 semaines par (jour, quart d'heure)
#define VN_FC_STEPS        25   // prévision 0…120 min au pas de 5 min
#define VN_MAX_ALERTS      96
#define VN_MAX_TRANSFERS    8
