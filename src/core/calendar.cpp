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

bool isPublicHoliday(int y, int m, int d) {
  static const int FIX[][2] = { { 1, 1 }, { 5, 1 }, { 5, 9 }, { 6, 23 }, { 8, 15 }, { 11, 1 }, { 12, 25 }, { 12, 26 } };
  for (const auto & f : FIX) if (f[0] == m && f[1] == d) return true;
  int em, ed;
  easterDate(y, em, ed);
  int delta = dayOfYear(y, m, d) - dayOfYear(y, em, ed);
  return delta == 1 || delta == 39 || delta == 50;
}

} // namespace vn
