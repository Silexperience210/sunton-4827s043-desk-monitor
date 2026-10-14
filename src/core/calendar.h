// ── Jours fériés légaux du Luxembourg ────────────────────────────────
// Fixes : 1ᵉʳ janvier, 1ᵉʳ mai, 9 mai (Journée de l'Europe), 23 juin (Fête nationale),
// 15 août, 1ᵉʳ novembre, 25 et 26 décembre. Mobiles (Pâques grégorien) : lundi de Pâques,
// Ascension (+39 j), lundi de Pentecôte (+50 j). Portable : testé par sim/test_core.cpp.
#pragma once

namespace vn {

void easterDate(int year, int & month, int & day);       // dimanche de Pâques
bool isPublicHoliday(int year, int month, int day);      // date civile locale

} // namespace vn
