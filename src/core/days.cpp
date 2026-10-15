#include "days.h"
#include "model.h"
#include "calendar.h"
#include "../platform/platform.h"
#include <string.h>

namespace vn {

static_assert(sizeof(DayNet) == 204, "format v1 de /vn_days.bin (DayNet)");
static_assert(sizeof(DaySt) == 12, "format v1 de /vn_days.bin (DaySt)");
static_assert(sizeof(DaysFileHdr) == 40, "en-tête de /vn_days.bin");

// ── Dates locales (jours depuis le 01/01/1970, calendrier grégorien) ──
int32_t civilDay(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
void dayCivil(int32_t z, int & y, int & m, int & d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = z - era * 146097;
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yoe + era * 400 + (m <= 2);
}
int32_t localDay(int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  return civilDay(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}
int localHour(int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  return lt.tm_hour;
}
bool dayHoliday(int32_t day) {
  int y, m, d;
  dayCivil(day, y, m, d);
  return isPublicHoliday(y, m, d);
}

// ── Anneau ───────────────────────────────────────────────────────────
bool DayBook::init() {
  net  = (DayNet *)plat::psAlloc(sizeof(DayNet) * VN_DAYS);
  st   = (DaySt *)plat::psAlloc(sizeof(DaySt) * (size_t)VN_DAYS * VN_MAX_ST);
  hc   = (uint8_t *)plat::psAlloc((size_t)VN_MAX_ST * 4);
  prev = (uint8_t *)plat::psAlloc(VN_MAX_ST);
  if (!net || !st || !hc || !prev) return false;
  clear();
  return true;
}

void DayBook::clear() {
  memset(net, 0, sizeof(DayNet) * VN_DAYS);
  memset(st, 0, sizeof(DaySt) * (size_t)VN_DAYS * VN_MAX_ST);
  memset(hc, 0, (size_t)VN_MAX_ST * 4);
  memset(prev, 255, VN_MAX_ST);
  head = -1; count = 0; hcKey = 0; prevT = 0;
  version++;
}

int DayBook::index(int back) const {
  if (head < 0 || back < 0 || back >= count) return -1;
  return (head - back + VN_DAYS) % VN_DAYS;
}
const DayNet * DayBook::dayAt(int back) const { int i = index(back); return i < 0 ? nullptr : net + i; }
const DaySt * DayBook::stAt(int back) const { int i = index(back); return i < 0 ? nullptr : st + (size_t)i * VN_MAX_ST; }
int DayBook::find(int32_t day) const {
  for (int b = 0; b < count; b++) {
    const DayNet * d = dayAt(b);
    if (d->day == day) return b;
    if (d->day < day) return -1;               // jours stockés en ordre croissant
  }
  return -1;
}

static inline uint8_t inc8(uint8_t v) { return v < 255 ? v + 1 : v; }
static inline uint16_t inc16(uint16_t v) { return v < 65535 ? v + 1 : v; }

void DayBook::addSlot(const Model & m, int64_t slotT) {
  if (!net || slotT <= 0) return;
  if (prevT && slotT <= prevT) return;         // horloge revenue en arrière, ou tranche déjà versée
  const int32_t day = localDay(slotT);
  const int hour = localHour(slotT);
  if (head >= 0 && day < net[head].day) return;
  if (head < 0 || day != net[head].day) {      // nouveau jour : la case la plus ancienne est recyclée
    head = (head + 1) % VN_DAYS;
    if (count < VN_DAYS) count++;
    DayNet & dn = net[head];
    memset(&dn, 0, sizeof(dn));
    dn.day = day;
    dn.dow = (uint8_t)dayDow(day);
    dn.flags = dayHoliday(day) ? DF_HOLIDAY : 0;
    memset(st + (size_t)head * VN_MAX_ST, 0, sizeof(DaySt) * VN_MAX_ST);
  }
  const bool consecutive = prevT && slotT - prevT == VN_RING_STEP_S;
  const bool inHours = hour >= DAY_H0 && hour < DAY_H1;
  const int h = hour - DAY_H0;
  const int32_t key = day * 100 + hour;
  if (key != hcKey) { memset(hc, 0, (size_t)VN_MAX_ST * 4); hcKey = key; }
  DayNet & dn = net[head];
  DaySt * row = st + (size_t)head * VN_MAX_ST;
  if (inHours) dn.slots[h] = inc8(dn.slots[h]);
  for (int i = 0; i < m.n && i < VN_MAX_ST; i++) {
    const Station & s = m.st[i];
    if (!s.active || !s.live) { prev[i] = 255; continue; }
    const uint8_t cur = s.status;
    if (inHours) {
      DaySt & r = row[i];
      uint8_t * c = hc + (size_t)i * 4;
      const uint16_t bit = (uint16_t)(1u << h);
      dn.obs[h] = inc16(dn.obs[h]); r.obs = inc8(r.obs); c[0] = inc8(c[0]);
      if (cur == ST_CLOSED) { dn.closed[h] = inc16(dn.closed[h]); r.closed = inc8(r.closed); }
      else if (cur == ST_EMPTY) { dn.empty[h] = inc16(dn.empty[h]); r.empty = inc8(r.empty); c[1] = inc8(c[1]); }
      else if (cur == ST_FULL) { dn.full[h] = inc16(dn.full[h]); r.full = inc8(r.full); c[2] = inc8(c[2]); }
      if (s.anom == AN_HIGH || s.anom == AN_LOW) c[3] = inc8(c[3]);
      if (consecutive && prev[i] != 255) {
        if (cur == ST_EMPTY && prev[i] != ST_EMPTY) dn.evEmpty[h] = inc16(dn.evEmpty[h]);
        if (cur == ST_FULL && prev[i] != ST_FULL) dn.evFull[h] = inc16(dn.evFull[h]);
      }
      if (c[0] >= DAY_SEEN_SLOTS) r.seen |= bit;
      if (c[1] >= DAY_HIT_SLOTS) r.hitE |= bit;
      if (c[2] >= DAY_HIT_SLOTS) r.hitF |= bit;
      if (c[3] >= DAY_HIT_SLOTS) r.hitA |= bit;
    }
    prev[i] = cur;
  }
  for (int i = m.n; i < VN_MAX_ST; i++) prev[i] = 255;
  prevT = slotT;
  version++;
}

// ── Persistance ──────────────────────────────────────────────────────
// /vn_days.bin, version 1 :
//   DaysFileHdr (40 octets) · sid des nSt stations (8 octets chacun, ordre des colonnes) ·
//   nDays jours, du plus ancien au plus récent : DayNet (netSz octets) puis nSt × DaySt (stSz octets) ·
//   compteurs du créneau en cours (nSt × 4) · statut de la tranche précédente (nSt).
// netSz et stSz sont ceux du fichier : un enregistrement plus court (format plus ancien) est relu
// par son préfixe, le reste à zéro ; plus long (format plus récent de même version majeure), par
// le préfixe connu. Version majeure inconnue, magic faux, taille incohérente : fichier ignoré.
// Un jour incohérent (date non croissante ou future, compte impossible) est écarté, pas le fichier.
bool DayBook::serialize(const Model & m, uint8_t *& out, size_t & len) const {
  out = nullptr; len = 0;
  if (!net || m.n <= 0) return false;
  const int nSt = m.n;
  size_t sz = sizeof(DaysFileHdr) + (size_t)nSt * 8 + (size_t)count * (sizeof(DayNet) + (size_t)nSt * sizeof(DaySt))
              + (size_t)nSt * 5;
  uint8_t * b = (uint8_t *)plat::psAlloc(sz);
  if (!b) return false;
  DaysFileHdr h{};
  h.magic = DAYS_MAGIC; h.ver = DAYS_VER; h.nSt = (uint16_t)nSt; h.nDays = (uint16_t)count;
  h.netSz = sizeof(DayNet); h.stSz = sizeof(DaySt);
  h.hcKey = hcKey; h.prevT = prevT; h.savedAt = m.lastUpdate;
  size_t o = 0;
  memcpy(b + o, &h, sizeof(h)); o += sizeof(h);
  for (int i = 0; i < nSt; i++) { memcpy(b + o, m.st[i].sid, 8); o += 8; }
  for (int k = count - 1; k >= 0; k--) {
    int idx = index(k);
    memcpy(b + o, net + idx, sizeof(DayNet)); o += sizeof(DayNet);
    memcpy(b + o, st + (size_t)idx * VN_MAX_ST, sizeof(DaySt) * (size_t)nSt); o += sizeof(DaySt) * (size_t)nSt;
  }
  memcpy(b + o, hc, (size_t)nSt * 4); o += (size_t)nSt * 4;
  memcpy(b + o, prev, (size_t)nSt); o += (size_t)nSt;
  out = b; len = o;
  return true;
}

static bool dayNetSane(const DayNet & d) {
  if (d.day <= 0 || d.dow > 6 || d.dow != dayDow(d.day) || (d.flags & ~DF_HOLIDAY)) return false;
  for (int h = 0; h < DAY_NH; h++) {
    if (d.slots[h] > DAY_SLOTS_H) return false;
    if (d.obs[h] > (uint32_t)d.slots[h] * VN_MAX_ST) return false;
    if ((uint32_t)d.closed[h] + d.empty[h] + d.full[h] > d.obs[h]) return false;
  }
  return true;
}
static bool dayStSane(const DaySt & r) {
  const int maxSlots = DAY_NH * DAY_SLOTS_H;
  if (r.obs > maxSlots || r.closed + r.empty + r.full > r.obs) return false;
  const uint16_t mask = (uint16_t)((1u << DAY_NH) - 1);
  return !(r.seen & ~mask) && !(r.hitE & ~mask) && !(r.hitF & ~mask) && !(r.hitA & ~mask);
}

bool DayBook::load(const uint8_t * b, size_t len, const int16_t * map, int nStFile, int64_t now) {
  if (!net || !b || len < sizeof(DaysFileHdr)) return false;
  DaysFileHdr h;
  memcpy(&h, b, sizeof(h));
  if (h.magic != DAYS_MAGIC || h.ver != DAYS_VER || h.nSt != nStFile || h.nSt > VN_MAX_ST
      || h.netSz < 8 || h.netSz > 4096 || h.stSz < 4 || h.stSz > 256 || h.nDays > 1000) return false;
  size_t need = sizeof(h) + (size_t)h.nSt * 8 + (size_t)h.nDays * (h.netSz + (size_t)h.nSt * h.stSz) + (size_t)h.nSt * 5;
  if (len != need) return false;
  clear();
  const int32_t maxDay = now > 0 ? localDay(now) : (h.savedAt > 0 ? localDay(h.savedAt) : 0x7FFFFFFF);
  const int skip = h.nDays > VN_DAYS ? h.nDays - VN_DAYS : 0;          // trop de jours : on garde les plus récents
  size_t o = sizeof(h) + (size_t)h.nSt * 8 + (size_t)skip * (h.netSz + (size_t)h.nSt * h.stSz);
  int32_t last = 0;
  for (int k = skip; k < h.nDays; k++) {
    DayNet d;
    memset(&d, 0, sizeof(d));
    memcpy(&d, b + o, h.netSz < sizeof(d) ? h.netSz : sizeof(d));
    o += h.netSz;
    const uint8_t * rows = b + o;
    o += (size_t)h.nSt * h.stSz;
    if (!dayNetSane(d) || d.day <= last || d.day > maxDay) continue;
    head = (head + 1) % VN_DAYS;
    if (count < VN_DAYS) count++;
    net[head] = d;
    last = d.day;
    DaySt * row = st + (size_t)head * VN_MAX_ST;
    memset(row, 0, sizeof(DaySt) * VN_MAX_ST);
    for (int j = 0; j < h.nSt; j++) {
      if (map[j] < 0 || map[j] >= VN_MAX_ST) continue;
      DaySt r;
      memset(&r, 0, sizeof(r));
      memcpy(&r, rows + (size_t)j * h.stSz, h.stSz < sizeof(r) ? h.stSz : sizeof(r));
      if (dayStSane(r)) row[map[j]] = r;
    }
  }
  // Créneau en cours et tranche précédente : seulement s'ils prolongent le jour le plus récent relu
  bool cont = head >= 0 && h.prevT > 0 && localDay(h.prevT) == net[head].day && h.hcKey / 100 == net[head].day;
  if (cont) {
    const uint8_t * hcp = b + o;
    const uint8_t * pv = hcp + (size_t)h.nSt * 4;
    for (int j = 0; j < h.nSt; j++) {
      if (map[j] < 0 || map[j] >= VN_MAX_ST) continue;
      for (int q = 0; q < 4; q++) hc[(size_t)map[j] * 4 + q] = hcp[(size_t)j * 4 + q] <= DAY_SLOTS_H ? hcp[(size_t)j * 4 + q] : 0;
      prev[map[j]] = pv[j] < ST_COUNT ? pv[j] : 255;
    }
    hcKey = h.hcKey;
    prevT = h.prevT;
  }
  version++;
  return true;
}

} // namespace vn
