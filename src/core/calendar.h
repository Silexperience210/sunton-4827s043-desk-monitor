// ── Jours fériés légaux du Luxembourg ────────────────────────────────
// Source : Code du travail, art. L.232-2 (Journée de l'Europe ajoutée par la loi du 25 avril 2019
// modifiant les articles L.232-2 et L.233-4) — liste reprise par l'ITM
// (itm.public.lu, « Quels sont au Luxembourg les jours qualifiés de jours fériés légaux ? ») et par
// luxembourg.public.lu (« Jours fériés légaux », dates 2026 et 2027), consultés le 15/10/2026.
// Onze jours :
//   fixes    : Nouvel An (1/1), 1er mai, Journée de l'Europe (9/5), célébration publique de
//              l'anniversaire du Grand-Duc (23/6), Assomption (15/8), Toussaint (1/11),
//              1er et 2e jours de Noël (25/12, 26/12) ;
//   mobiles  : lundi de Pâques (Pâques + 1 j), Ascension (+ 39 j), lundi de Pentecôte (+ 50 j),
//              Pâques = calendrier grégorien (algorithme de Meeus / Jones / Butcher).
// Ni le Vendredi saint, ni le dimanche de Pâques, ni la Pentecôte, ni le 24 ou le 31 décembre.
// Un férié qui tombe un dimanche ou un jour chômé ouvre droit à un jour de congé compensatoire
// propre à chaque salarié (à prendre dans les 3 mois) : ce n'est pas un jour férié du calendrier,
// la carte ne le connaît pas. Portable : testé par sim/test_core.cpp.
#pragma once

namespace vn {

void easterDate(int year, int & month, int & day);           // dimanche de Pâques
const char * publicHolidayName(int year, int month, int day); // nom du férié, nullptr sinon
bool isPublicHoliday(int year, int month, int day);          // date civile locale

} // namespace vn
