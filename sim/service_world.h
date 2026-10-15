// ── Monde synthétique à vérité connue pour la vue « Service » ─────────
// Partagé par sim/test_core.cpp (règles) et sim/sim_main.cpp (captures, scénario « service »).
// Quatre semaines, du lundi 30/11/2026 au lundi 28/12/2026 (« aujourd'hui »), heure de Luxembourg.
// Chaque station a un RÔLE, et la vérité de chaque rôle est connue d'avance :
//   W_DRIFT    : vide de 8 h à 9 h chaque jour ouvrable depuis le mardi 08/12 — LE problème structurel
//   W_HEALTHY  : 8 à 12 vélos sur 20, toujours — rien à signaler
//   W_ISOLATED : un seul incident (vide 8 h–10 h le 22/12, pleine 17 h–18 h le 15/12) — un incident, pas un problème
//   W_NOISE    : chaque jour, au hasard, 0 à 2 épisodes vides ou pleins de 15 à 60 min à une heure
//                quelconque — le bruit d'une station animée ; rien de récurrent
//   W_WEEKEND  : pleine de 14 h à 16 h les samedis, dimanches et fériés — structurel, mais du week-end
//   W_NEAR     : vide à 8 h 6 jours ouvrables sur les 10 derniers, PLUS le 25/12 (férié) — juste sous le seuil,
//                et au-dessus si l'on comptait le jour férié comme un jour normal
//   W_COMMUTE  : vide à 8 h un jour ouvrable sur trois, au hasard — fréquent mais pas systématique (cas limite,
//                mesuré et rapporté, pas affirmé)
//   W_CLOSED   : hors service tout le mois
// La carte est éteinte toute la journée du jeudi 17/12 (aucun relevé).
#pragma once
#include <stdint.h>

namespace world {

enum Role { W_DRIFT, W_HEALTHY, W_ISOLATED, W_NOISE, W_WEEKEND, W_NEAR, W_COMMUTE, W_CLOSED };

static const int CAP = 20;
// Jours du monde : 0 = lundi 30/11/2026 … 28 = lundi 28/12/2026 (aujourd'hui)
static const int NDAYS = 29;
static const int OFF_DAY = 17;               // jeudi 17/12 : carte éteinte
inline int dateOf(int dw) { return dw == 0 ? 30 : dw; }            // jour du mois (30/11, 1/12…)
inline int monthOf(int dw) { return dw == 0 ? 11 : 12; }
inline int dowOf(int dw) { return (1 + dw) % 7; }                  // 0 = dimanche ; le jour 0 est un lundi
inline bool holidayW(int dw) { return monthOf(dw) == 12 && (dateOf(dw) == 25 || dateOf(dw) == 26); }
inline bool weekendW(int dw) { int w = dowOf(dw); return w == 0 || w == 6 || holidayW(dw); }

inline uint32_t hash(uint32_t a, uint32_t b, uint32_t c) {
  uint64_t z = ((uint64_t)a * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)b << 21) ^ ((uint64_t)c * 0xBF58476D1CE4E5B9ull);
  z ^= z >> 31; z *= 0x94D049BB133111EBull; z ^= z >> 29;
  return (uint32_t)z;
}

// Épisode du bruit : jour dw, station sid, numéro k → début (minutes), durée, plein ou vide ; false si aucun
inline bool noiseEpisode(int sid, int dw, int k, int & start, int & len, bool & full) {
  uint32_t h = hash((uint32_t)sid, (uint32_t)dw, (uint32_t)k);
  if ((h & 3) == 0) return false;                       // 3 chances sur 4 d'un épisode (×2 par jour)
  start = 7 * 60 + (int)((h >> 2) % (15 * 60 - 60));    // n'importe quelle heure de 7 h à 21 h
  len = 15 + 5 * (int)((h >> 14) % 10);                 // 15 à 60 min
  full = (h >> 20) & 1;
  return true;
}

// Vélos de la station (rôle, graine sid) au jour dw, minute mn (0…1439) ; closed = hors service
inline int bikes(Role r, int sid, int dw, int mn, bool & closed) {
  closed = false;
  const bool wd = !weekendW(dw);
  const int day = dateOf(dw);
  const bool dec = monthOf(dw) == 12;
  const int base = 8 + (int)(hash((uint32_t)sid, (uint32_t)dw, (uint32_t)(mn / 5)) % 5);   // 8…12
  switch (r) {
    case W_DRIFT:
      if (wd && (dec && day >= 8) && mn >= 8 * 60 && mn < 9 * 60) return 0;
      return base;
    case W_HEALTHY: return base;
    case W_ISOLATED:
      if (dec && day == 22 && mn >= 8 * 60 && mn < 10 * 60) return 0;
      if (dec && day == 15 && mn >= 17 * 60 && mn < 18 * 60) return CAP;
      return base;
    case W_NOISE:
      for (int k = 0; k < 2; k++) {
        int s, l; bool f;
        if (noiseEpisode(sid, dw, k, s, l, f) && mn >= s && mn < s + l) return f ? CAP : 0;
      }
      return base;
    case W_WEEKEND:
      if (!wd && mn >= 14 * 60 && mn < 16 * 60) return CAP;
      return base;
    case W_NEAR: {
      bool hit = dec && (day == 24 || day == 23 || day == 21 || day == 18 || day == 16 || day == 14 || day == 25);
      if (hit && mn >= 8 * 60 && mn < 9 * 60) return 0;
      return base;
    }
    case W_COMMUTE:
      if (wd && hash((uint32_t)sid, (uint32_t)dw, 99) % 3 == 0 && mn >= 8 * 60 && mn < 9 * 60) return 0;
      return base;
    case W_CLOSED: closed = true; return 0;
  }
  return base;
}

} // namespace world
