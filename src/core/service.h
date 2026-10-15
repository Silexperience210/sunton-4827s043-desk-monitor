// ── Vue « Service » : les règles, lues sur les résumés par jour ──────
// Ce que regarde un directeur de service : disponibilité sur la journée et sur 30 jours, ruptures
// et saturations par jour, pires et meilleures stations, comparaisons d'une semaine à l'autre, et
// surtout les problèmes RÉCURRENTS (le même écart, au même créneau, jour après jour).
// Toutes les règles sont ici, pures (aucune UI) : testées par sim/test_core.cpp, décrites dans
// docs/vue-service.md, et dites à l'écran par les phrases de ce module.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "days.h"

namespace vn {

struct Model;

// ── Disponibilité ────────────────────────────────────────────────────
// Unité : la station-tranche de 5 min sur les heures de service (7 h–22 h) — une station observée
// pendant 5 min. Affichée en station·heures (12 tranches = 1 h).
//   disponible  = ouverte, ≥ 1 vélo louable ET ≥ 1 borne libre ;
//   vide        = ouverte mais aucun vélo louable (on peut y rendre, pas y prendre) ;
//   pleine      = ouverte mais aucune borne libre (on peut y prendre, pas y rendre) ;
//   hors service= non installée, ou ni location ni retour.
// Taux de disponibilité = disponibles ÷ observées. Une station vide n'est PAS hors service :
// l'écran donne les quatre parts, jamais un taux seul.
struct SvcCount {
  uint32_t obs, closed, empty, full;   // station-tranches observées, dont hors service / vides / pleines
  uint32_t evEmpty, evFull;            // entrées en rupture / en saturation constatées
  uint32_t slots, slotsMax;            // tranches relevées / tranches possibles sur les jours présents
  int      days, daysAsked;            // jours présents dans les résumés / jours demandés
  uint32_t avail() const { return obs - closed - empty - full; }
};
// Somme des jours [dayFrom, dayTo] (inclus), créneaux [0, hTo) (hTo = DAY_NH : la journée entière)
void svcSum(const DayBook & b, int32_t dayFrom, int32_t dayTo, int hTo, SvcCount & out);
// Créneaux COMPLETS d'aujourd'hui à l'instant now (0 avant 8 h, DAY_NH à partir de 22 h)
int  svcDoneHours(int64_t now);

// ── Problèmes récurrents ─────────────────────────────────────────────
// Un créneau (une heure de 7 h à 22 h) est « touché » un jour donné si la station y a passé au moins
// 15 min (3 tranches) dans l'état — vide, pleine, au-dessus ou en dessous du profil (AN_HIGH, AN_LOW :
// deux états distincts, jamais additionnés) —, et
// « relevé » si la carte l'a observée au moins 30 min (6 tranches) dans l'heure.
// Deux classes de jours, jamais mélangées : jours ouvrables (lundi–vendredi) ; week-end ET jours
// fériés légaux (un férié se comporte comme un dimanche : il ne compte pas comme un jour normal).
// Règle : sur les wdWin derniers jours ouvrables où le créneau a été relevé (au plus `lookback`
// jours en arrière), au moins wdNeed touchés → problème récurrent ; week-end/fériés : weNeed sur weWin.
// Seuls les jours COMPLETS comptent : aujourd'hui est exclu (l'incident du jour relève de l'écran Alertes).
// Les jours sans relevé du créneau (carte éteinte) ne comptent ni pour ni contre.
// Série : jours relevés consécutifs de la classe, touchés, en remontant depuis hier (au plus `lookback`
// jours, au-delà de la fenêtre : « 10 sur 10 · série 12 j »).
// Créneaux voisins signalés pour la même station, le même état et la même classe : fusionnés
// en une plage (les nombres affichés sont ceux du créneau le MOINS touché de la plage).
enum RecKind : uint8_t { RK_EMPTY, RK_FULL, RK_HIGH, RK_LOW, RK_COUNT };
struct RecurOpts {
  int wdWin = 10, wdNeed = 7;          // jours ouvrables : 7 sur les 10 derniers relevés
  int weWin = 6, weNeed = 5;           // week-end et fériés : 5 sur les 6 derniers relevés
  int lookback = 28;                   // jours calendaires examinés au plus
};
extern RecurOpts g_recur;
struct Recur {
  int16_t st;
  uint8_t kind;                        // RecKind
  uint8_t weekend;                     // classe : 0 = jours ouvrables, 1 = week-end et fériés
  uint8_t h0, h1;                      // créneaux [h0, h1] : de (7 + h0) h à (8 + h1) h
  uint8_t hits, days;                  // jours touchés / jours relevés (créneau le moins touché)
  uint8_t streak;                      // série en cours (0 = le jour relevé le plus récent n'est pas touché)
  int32_t last;                        // dernier jour touché
};
bool svcWeekendClass(const DayNet & d);
// Remplit out (trié : série décroissante, puis taux) ; renvoie le nombre de problèmes trouvés (≤ max)
int  svcRecurrent(const DayBook & b, const Model & m, int32_t today, Recur * out, int max, const RecurOpts & o = g_recur);
// Jours (dans la fenêtre `lookback`) où au moins un créneau a été relevé, par classe
void svcRecurDays(const DayBook & b, int32_t today, const RecurOpts & o, int & weekdays, int & weekendDays);

// ── Classement des stations ──────────────────────────────────────────
// Part du temps indisponible = (vide + pleine + hors service) ÷ observée, sur la période. Seules les
// stations observées au moins la moitié du temps de la mieux observée sont classées (une station
// apparue hier n'est pas « la meilleure » sur 2 h).
struct StRank {
  int16_t  st;
  uint32_t obs, closed, empty, full;
  float bad() const { return obs ? (float)(closed + empty + full) / obs : 0.f; }
};
int  svcRank(const DayBook & b, const Model & m, int32_t dayFrom, int32_t dayTo, StRank * out, int max);
void svcSortWorst(StRank * r, int n);  // part indisponible décroissante
void svcSortBest(StRank * r, int n);   // part indisponible croissante (à égalité : la plus observée d'abord)

// ── Phrases de l'écran ───────────────────────────────────────────────
// Pourcentage à une décimale (virgule) de num ÷ den, « — » si den = 0
void svcPct(char * out, size_t n, uint32_t num, uint32_t den);
// Station-tranches → station·heures (« 2 431 h » ; au dixième sous 10 h : « 1,5 h »)
void svcHours(char * out, size_t n, uint32_t slots);
// « Vide 8 h–9 h · 9 jours ouvrables sur 10 · série 9 j » (« série interrompue » si le dernier jour relevé est sain)
void svcRecurText(const Recur & r, char * out, size_t n);
const char * svcKindName(uint8_t kind);    // « Vide », « Pleine », « Au-dessus du profil », « En dessous du profil »
// La règle du détecteur, en une phrase (affichée au-dessus de la liste)
void svcRecurRule(const RecurOpts & o, char * out, size_t n);
// « jeu. 08/10 »
void svcDayLabel(int32_t day, char * out, size_t n);

} // namespace vn
