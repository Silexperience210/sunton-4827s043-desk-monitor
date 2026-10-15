#include "service.h"
#include "model.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>

namespace vn {

RecurOpts g_recur;

// ── Disponibilité ────────────────────────────────────────────────────
void svcSum(const DayBook & b, int32_t dayFrom, int32_t dayTo, int hTo, SvcCount & out) {
  memset(&out, 0, sizeof(out));
  out.daysAsked = dayTo >= dayFrom ? (int)(dayTo - dayFrom + 1) : 0;
  if (hTo > DAY_NH) hTo = DAY_NH;
  if (hTo < 0) hTo = 0;
  for (int k = 0; k < b.count; k++) {
    const DayNet * d = b.dayAt(k);
    if (d->day < dayFrom) break;               // jours stockés en ordre croissant
    if (d->day > dayTo) continue;
    out.days++;
    out.slotsMax += (uint32_t)hTo * DAY_SLOTS_H;
    for (int h = 0; h < hTo; h++) {
      out.slots += d->slots[h];
      out.obs += d->obs[h]; out.closed += d->closed[h]; out.empty += d->empty[h]; out.full += d->full[h];
      out.evEmpty += d->evEmpty[h]; out.evFull += d->evFull[h];
    }
  }
}

int svcDoneHours(int64_t now) {
  int h = localHour(now) - DAY_H0;
  return h < 0 ? 0 : h > DAY_NH ? DAY_NH : h;
}

// ── Problèmes récurrents ─────────────────────────────────────────────
bool svcWeekendClass(const DayNet & d) { return d.dow == 0 || d.dow == 6 || (d.flags & DF_HOLIDAY); }

// Un créneau, une station, un état, une classe : la règle appliquée
struct Cell { bool flag; uint8_t hits, days, streak; int32_t last; };

static uint16_t hitMask(const DaySt & r, int kind) {
  return kind == RK_EMPTY ? r.hitE : kind == RK_FULL ? r.hitF : kind == RK_HIGH ? r.hitH : r.hitL;
}

static Cell evalCell(const DayBook & b, int st, int kind, bool weekend, int h, int32_t today, const RecurOpts & o) {
  Cell c{};
  const int win = weekend ? o.weWin : o.wdWin, need = weekend ? o.weNeed : o.wdNeed;
  const uint16_t bit = (uint16_t)(1u << h);
  bool open = true;
  // La fenêtre (win jours relevés) fait le compte ; la série, elle, peut remonter au-delà (lookback)
  for (int k = 0; k < b.count && (c.days < win || open); k++) {
    const DayNet * d = b.dayAt(k);
    if (d->day >= today) continue;             // jours COMPLETS seulement : aujourd'hui relève des alertes
    if (d->day < today - o.lookback) break;
    if (svcWeekendClass(*d) != weekend) continue;
    const DaySt & r = b.stAt(k)[st];
    if (!(r.seen & bit)) continue;             // créneau non relevé ce jour-là : ni pour ni contre
    bool hit = hitMask(r, kind) & bit;
    if (c.days < win) {
      c.days++;
      if (hit) { c.hits++; if (!c.last) c.last = d->day; }
    }
    if (open) { if (hit && c.streak < 255) c.streak++; else open = false; }
  }
  c.flag = need > 0 && c.hits >= need;
  return c;
}

void svcRecurDays(const DayBook & b, int32_t today, const RecurOpts & o, int & weekdays, int & weekendDays) {
  weekdays = weekendDays = 0;
  for (int k = 0; k < b.count; k++) {
    const DayNet * d = b.dayAt(k);
    if (d->day >= today) continue;
    if (d->day < today - o.lookback) break;
    bool any = false;
    for (int h = 0; h < DAY_NH && !any; h++) any = d->slots[h] >= DAY_SEEN_SLOTS;
    if (!any) continue;
    if (svcWeekendClass(*d)) weekendDays++; else weekdays++;
  }
}

static bool recBefore(const Recur & a, const Recur & b) {
  if (a.streak != b.streak) return a.streak > b.streak;
  uint32_t l = (uint32_t)a.hits * b.days, r = (uint32_t)b.hits * a.days;   // a.hits/a.days > b.hits/b.days
  if (l != r) return l > r;
  if (a.h1 - a.h0 != b.h1 - b.h0) return a.h1 - a.h0 > b.h1 - b.h0;
  return a.st < b.st;
}

int svcRecurrent(const DayBook & b, const Model & m, int32_t today, Recur * out, int max, const RecurOpts & o) {
  int n = 0;
  if (!b.net || b.count == 0 || max <= 0) return 0;
  for (int i = 0; i < m.n && i < VN_MAX_ST; i++) {
    if (!m.st[i].active) continue;
    for (int we = 0; we < 2; we++) {
      Cell cell[RK_COUNT][DAY_NH];
      for (int kd = 0; kd < RK_COUNT; kd++)
        for (int h = 0; h < DAY_NH; h++) cell[kd][h] = evalCell(b, i, kd, we, h, today, o);
      // Un écart au profil au créneau où la station est déjà signalée vide ou pleine : même problème,
      // dit une seule fois (par l'état, plus parlant que « écart »)
      for (int h = 0; h < DAY_NH; h++)
        if (cell[RK_EMPTY][h].flag || cell[RK_FULL][h].flag) cell[RK_HIGH][h].flag = cell[RK_LOW][h].flag = false;
      for (int kd = 0; kd < RK_COUNT; kd++) {
        for (int h = 0; h < DAY_NH; h++) {
          if (!cell[kd][h].flag) continue;
          Recur r{};
          r.st = (int16_t)i; r.kind = (uint8_t)kd; r.weekend = (uint8_t)we;
          r.h0 = (uint8_t)h;
          const Cell * w = &cell[kd][h];
          uint8_t streak = w->streak;
          int32_t last = w->last;
          while (h + 1 < DAY_NH && cell[kd][h + 1].flag) {
            h++;
            const Cell & c = cell[kd][h];
            if ((uint32_t)c.hits * w->days < (uint32_t)w->hits * c.days) w = &c;
            if (c.streak < streak) streak = c.streak;
            if (c.last > last) last = c.last;
          }
          r.h1 = (uint8_t)h;
          r.hits = w->hits; r.days = w->days; r.streak = streak; r.last = last;
          if (n < max) out[n++] = r;
          else {                                // plein : remplace le moins grave s'il l'est moins
            int worst = 0;
            for (int q = 1; q < n; q++) if (recBefore(out[worst], out[q])) worst = q;
            if (recBefore(r, out[worst])) out[worst] = r;
          }
        }
      }
    }
  }
  for (int a = 1; a < n; a++) {                 // tri par insertion (n petit)
    Recur x = out[a];
    int q = a - 1;
    while (q >= 0 && recBefore(x, out[q])) { out[q + 1] = out[q]; q--; }
    out[q + 1] = x;
  }
  return n;
}

// ── Classement ───────────────────────────────────────────────────────
int svcRank(const DayBook & b, const Model & m, int32_t dayFrom, int32_t dayTo, StRank * out, int max) {
  static StRank acc[VN_MAX_ST];
  memset(acc, 0, sizeof(acc));
  uint32_t best = 0;
  for (int k = 0; k < b.count; k++) {
    const DayNet * d = b.dayAt(k);
    if (d->day < dayFrom) break;
    if (d->day > dayTo) continue;
    const DaySt * row = b.stAt(k);
    for (int i = 0; i < m.n && i < VN_MAX_ST; i++) {
      acc[i].obs += row[i].obs; acc[i].closed += row[i].closed;
      acc[i].empty += row[i].empty; acc[i].full += row[i].full;
    }
  }
  for (int i = 0; i < m.n && i < VN_MAX_ST; i++) if (m.st[i].active && acc[i].obs > best) best = acc[i].obs;
  int n = 0;
  if (!best) return 0;
  for (int i = 0; i < m.n && i < VN_MAX_ST && n < max; i++) {
    if (!m.st[i].active || acc[i].obs * 2 < best) continue;
    acc[i].st = (int16_t)i;
    out[n++] = acc[i];
  }
  return n;
}

// a « avant » b pour le classement des pires : part indisponible décroissante (produits croisés,
// exacts), à égalité la plus observée d'abord, puis l'index
static bool worse(const StRank & a, const StRank & b) {
  uint64_t l = (uint64_t)(a.closed + a.empty + a.full) * b.obs, r = (uint64_t)(b.closed + b.empty + b.full) * a.obs;
  if (l != r) return l > r;
  if (a.obs != b.obs) return a.obs > b.obs;
  return a.st < b.st;
}
static bool better(const StRank & a, const StRank & b) {
  uint64_t l = (uint64_t)(a.closed + a.empty + a.full) * b.obs, r = (uint64_t)(b.closed + b.empty + b.full) * a.obs;
  if (l != r) return l < r;
  if (a.obs != b.obs) return a.obs > b.obs;
  return a.st < b.st;
}
static void sortBy(StRank * r, int n, bool (*before)(const StRank &, const StRank &)) {
  for (int a = 1; a < n; a++) {
    StRank x = r[a];
    int q = a - 1;
    while (q >= 0 && before(x, r[q])) { r[q + 1] = r[q]; q--; }
    r[q + 1] = x;
  }
}
void svcSortWorst(StRank * r, int n) { sortBy(r, n, worse); }
void svcSortBest(StRank * r, int n) { sortBy(r, n, better); }

// ── Phrases ──────────────────────────────────────────────────────────
static void intSep(char * out, size_t n, uint32_t v) {      // 2 431 (espace fine insécable, comme fmtInt)
  char digits[16];
  snprintf(digits, sizeof(digits), "%u", (unsigned)v);
  size_t L = strlen(digits), o = 0;
  for (size_t i = 0; i < L && o + 1 < n; i++) {
    out[o++] = digits[i];
    size_t rem = L - i - 1;
    if (rem && rem % 3 == 0 && o + 3 < n) { memcpy(out + o, "\xE2\x80\xAF", 3); o += 3; }
  }
  out[o] = 0;
}

void svcPct(char * out, size_t n, uint32_t num, uint32_t den) {
  if (!den) { snprintf(out, n, "\xE2\x80\x94"); return; }
  uint32_t p = (uint32_t)(((uint64_t)num * 1000 + den / 2) / den);     // dixièmes de %, arrondi
  snprintf(out, n, "%u,%u\xC2\xA0%%", (unsigned)(p / 10), (unsigned)(p % 10));
}

void svcHours(char * out, size_t n, uint32_t slots) {
  if (slots < 10 * DAY_SLOTS_H) {               // moins de 10 h : au dixième (« 1,5 h »)
    uint32_t d = (slots * 10 + DAY_SLOTS_H / 2) / DAY_SLOTS_H;
    snprintf(out, n, "%u,%u\xC2\xA0h", (unsigned)(d / 10), (unsigned)(d % 10));
    return;
  }
  char a[24];
  intSep(a, sizeof(a), (slots + DAY_SLOTS_H / 2) / DAY_SLOTS_H);
  snprintf(out, n, "%s\xC2\xA0h", a);
}

void svcRateText(char * out, size_t n, uint32_t num, uint32_t den) {
  char p[16], a[24], b[24];
  svcPct(p, sizeof(p), num, den);
  svcHours(a, sizeof(a), num);
  svcHours(b, sizeof(b), den);
  snprintf(out, n, "%s \xC2\xB7 %s sur %s", p, a, b);
}

const char * svcKindName(uint8_t kind) {
  return kind == RK_EMPTY ? "Vide" : kind == RK_FULL ? "Pleine" : kind == RK_HIGH ? "Au-dessus du profil" : "En dessous du profil";
}

void svcRecurText(const Recur & r, char * out, size_t n) {
  snprintf(out, n, "%s %d\xC2\xA0h\xE2\x80\x93%d\xC2\xA0h \xC2\xB7 %u %s sur %u \xC2\xB7 s\xC3\xA9rie %u\xC2\xA0j",
           svcKindName(r.kind), DAY_H0 + r.h0, DAY_H0 + r.h1 + 1, r.hits,
           r.weekend ? (r.hits > 1 ? "week-ends/f\xC3\xA9ri\xC3\xA9s" : "week-end/f\xC3\xA9ri\xC3\xA9")
                     : (r.hits > 1 ? "jours ouvrables" : "jour ouvrable"),
           r.days, r.streak);
}

void svcRecurRule(const RecurOpts & o, char * out, size_t n) {
  snprintf(out, n, "R\xC3\xA9" "current = \xE2\x89\xA5\xC2\xA0%d des %d derniers jours ouvrables relev\xC3\xA9s "
           "(week-end/f\xC3\xA9ri\xC3\xA9s : %d sur %d), au m\xC3\xAAme cr\xC3\xA9neau d'une heure, "
           "\xE2\x89\xA5\xC2\xA0" "15\xC2\xA0min dans l'\xC3\xA9tat. Aujourd'hui exclu.",
           o.wdNeed, o.wdWin, o.weNeed, o.weWin);
}

void svcDayLabel(int32_t day, char * out, size_t n) {
  static const char * D[] = { "dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam." };
  int y, m, d;
  dayCivil(day, y, m, d);
  snprintf(out, n, "%s %02d/%02d", D[dayDow(day)], d, m);
}

} // namespace vn
