// ── Configuration du serveur en ligne ────────────────────────────────
// Les URL et les périodes sont celles de la carte (src/config.h) : le serveur relève exactement
// les mêmes flux, au même rythme, pour apprendre exactement la même chose — sans s'éteindre.
import { resolve } from 'node:path';

const env = (k, d) => (process.env[k] === undefined || process.env[k] === '' ? d : process.env[k]);
const int = (k, d) => {
  const v = parseInt(env(k, ''), 10);
  return Number.isFinite(v) ? v : d;
};

export const CONFIG = {
  port: int('VN_PORT', 8790),
  host: env('VN_HOST', '0.0.0.0'),
  dataDir: resolve(env('VN_DATA', './data')),
  // Jeton facultatif : protège /sync et /api/twin (données publiques dérivées, mais un serveur
  // exposé sur Internet n'a pas à servir ses profils à n'importe qui). Vide = ouvert (réseau local).
  token: env('VN_TOKEN', ''),
  name: env('VN_NAME', 'VelohNav Ops en ligne'),

  // src/config.h
  gbfsBase: env('VN_GBFS_BASE', 'https://api.cyclocity.fr/contracts/luxembourg/gbfs/v3'),
  wxUrl: env('VN_WX_URL',
    'https://api.open-meteo.com/v1/forecast?latitude=49.6116&longitude=6.1319' +
    '&current=temperature_2m,precipitation,wind_speed_10m,wind_direction_10m,weather_code' +
    '&hourly=temperature_2m,precipitation_probability,precipitation,weather_code,wind_speed_10m' +
    '&forecast_hours=6&wind_speed_unit=kmh&timezone=Europe%2FLuxembourg'),
  refreshS: Math.max(30, int('VN_REFRESH_S', 60)),     // g_set.refreshSec (60 s par défaut, 30 s au moins)
  infoRefreshS: 3600,                                    // VN_INFO_REFRESH_S
  typesRefreshS: 86400,                                  // VN_TYPES_REFRESH_S
  wxRefreshS: 900,                                       // VN_WX_REFRESH_S
  savePeriodS: Math.max(60, int('VN_SAVE_S', 600)),       // la carte : VN_SAVE_PERIOD_S (30 min) ; ici 10 min, une coupure franche coûte moins

  // Archive des instantanés pour le banc (sim/bench_prevision --reel) : même disposition que
  // tools/carte_dump.py — <données>/archive/AAAAMMJJ-HHMMSS/{vn_hist.bin, vn_prof.bin, vn_days.bin,
  // vn_stations.csv, manifeste.json}. Heures locales de Luxembourg, séparées par des virgules.
  archiveHours: env('VN_ARCHIVE_HOURS', '6,18').split(',').map((h) => parseInt(h, 10)).filter((h) => h >= 0 && h < 24),
  archiveKeepDays: int('VN_ARCHIVE_KEEP_DAYS', 0),      // 0 = tout garder

  // Réplication vers les jumeaux d'écran : évènements gardés en mémoire
  eventsKept: int('VN_EVENTS_KEPT', 240),
  userAgent: 'VelohNavOps/%v (serveur en ligne)',
};
