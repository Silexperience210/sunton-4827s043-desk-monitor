#include "calendar.h"

namespace vn {

// Algorithme anonyme grégorien (Meeus / Jones / Butcher)
void easterDate(int y, int & month, int & day) {
  int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
  int h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
  int m = (a + 11 * h + 22 * l) / 451;
  month = (h + l - 7 * m + 114) / 31;
  day = ((h + l - 7 * m + 114) % 31) + 1;
}

static int dayOfYear(int y, int m, int d) {
  static const int CUM[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return CUM[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

const char * publicHolidayName(int y, int m, int d) {
  static const struct { int m, d; const char * name; } FIX[] = {
    { 1, 1, "Nouvel An" }, { 5, 1, "1er mai" }, { 5, 9, "Journ\xC3\xA9" "e de l'Europe" },
    { 6, 23, "F\xC3\xAA" "te nationale" }, { 8, 15, "Assomption" }, { 11, 1, "Toussaint" },
    { 12, 25, "No\xC3\xAB" "l" }, { 12, 26, "Saint-\xC3\x89tienne" } };
  if (m < 1 || m > 12 || d < 1 || d > 31) return nullptr;
  for (const auto & f : FIX) if (f.m == m && f.d == d) return f.name;
  int em, ed;
  easterDate(y, em, ed);
  int delta = dayOfYear(y, m, d) - dayOfYear(y, em, ed);   // Pâques + 50 j reste dans la même année
  if (delta == 1) return "lundi de P\xC3\xA2ques";
  if (delta == 39) return "Ascension";
  if (delta == 50) return "lundi de Pentec\xC3\xB4te";
  return nullptr;
}

bool isPublicHoliday(int y, int m, int d) { return publicHolidayName(y, m, d) != nullptr; }

} // namespace vn
