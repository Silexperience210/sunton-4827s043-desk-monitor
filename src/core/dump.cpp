// ── Vidage série des données apprises (voir dump.h) ─────────────────
#include "dump.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>

namespace vn {

uint32_t crc32(const void * data, size_t n, uint32_t crc) {
  static uint32_t T[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      T[i] = c;
    }
    ready = true;
  }
  const uint8_t * p = (const uint8_t *)data;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = T[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

size_t base64Encode(const uint8_t * in, size_t n, char * out) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = A[(v >> 18) & 63];
    out[o++] = A[(v >> 12) & 63];
    out[o++] = i + 1 < n ? A[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? A[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

bool DumpWriter::add(const char * name, const uint8_t * data, size_t len) {
  if (nf >= DUMP_MAX_FILES || !name) return false;
  f[nf] = { name, data, data ? len : 0, crc32(data, data ? len : 0) };
  nf++;
  return true;
}

void DumpWriter::abort(const char * w) {
  if (phase == PH_DONE) return;
  snprintf(why, sizeof(why), "%s", w ? w : "");
  phase = PH_ABORT;
}

bool DumpWriter::next(char * buf, size_t cap, size_t & len) {
  len = 0;
  if (cap < DUMP_LINE_MAX) return false;
  int r = 0;
  for (;;) switch (phase) {
    case PH_DONE: return false;
    case PH_ABORT:
      r = snprintf(buf, cap, "VNDUMP ABANDON %s\n", why);
      phase = PH_DONE;
      len = (size_t)r;
      return true;
    case PH_BEGIN:
      r = snprintf(buf, cap, "VNDUMP DEBUT proto=%d version=%s t=%lld fichiers=%d\n", DUMP_PROTO, fw, (long long)at, nf);
      cur = 0; blk = 0;
      phase = nf ? PH_FILE : PH_TERM;
      len = (size_t)r;
      return true;
    case PH_FILE:
      r = snprintf(buf, cap, "VNDUMP FICHIER %d %s taille=%u blocs=%u crc=%08X\n", cur, f[cur].name, (unsigned)f[cur].len,
                   (unsigned)blocks(cur), (unsigned)f[cur].crc);
      blk = 0;
      phase = blocks(cur) ? PH_BLOCK : PH_END;
      len = (size_t)r;
      return true;
    case PH_BLOCK: {
      size_t o = blk * DUMP_BLOCK, n = f[cur].len - o;
      if (n > DUMP_BLOCK) n = DUMP_BLOCK;
      const uint8_t * p = f[cur].data + o;
      r = snprintf(buf, cap, "VNDUMP B %d %u ", cur, (unsigned)blk);
      r += (int)base64Encode(p, n, buf + r);
      r += snprintf(buf + r, cap - r, " %08X\n", (unsigned)crc32(p, n));
      if (++blk >= blocks(cur)) phase = PH_END;
      len = (size_t)r;
      return true;
    }
    case PH_END:
      r = snprintf(buf, cap, "VNDUMP FIN %d %s blocs=%u crc=%08X\n", cur, f[cur].name, (unsigned)blocks(cur), (unsigned)f[cur].crc);
      phase = ++cur < nf ? PH_FILE : PH_TERM;
      len = (size_t)r;
      return true;
    case PH_TERM:
      r = snprintf(buf, cap, "VNDUMP TERMINE fichiers=%d\n", nf);
      phase = PH_DONE;
      len = (size_t)r;
      return true;
  }
}

size_t stationsCsv(const Model & m, int64_t now, char * out, size_t cap) {
  size_t o = 0;
  int r = snprintf(out, cap, "# vn_stations v1 t=%lld\nsid;capacite;publiee;lat;lon;nom\n", (long long)now);
  if (r < 0 || (size_t)r >= cap) return 0;
  o = (size_t)r;
  for (int i = 0; i < m.n; i++) {
    const Station & s = m.st[i];
    char name[sizeof(s.name)];
    snprintf(name, sizeof(name), "%s", s.name);
    for (char * c = name; *c; c++) if (*c == ';' || *c == '\n' || *c == '\r') *c = ',';
    r = snprintf(out + o, cap - o, "%s;%u;%d;%.6f;%.6f;%s\n", s.sid, (unsigned)s.cap, s.active ? 1 : 0, s.lat, s.lon, name);
    if (r < 0 || (size_t)r >= cap - o) return 0;
    o += (size_t)r;
  }
  return o;
}

bool dumpSnapshot(const Model & m, DumpSnap & out) {
  out = DumpSnap();
  if (!m.serialize(out.blob)) return false;
  size_t cap = 64 + (size_t)m.n * 160;
  out.csv = (uint8_t *)plat::psAlloc(cap);
  if (out.csv) out.csvLen = stationsCsv(m, m.lastUpdate, (char *)out.csv, cap);
  return true;
}

void dumpFree(DumpSnap & s) {
  if (s.blob.prof) plat::psFree(s.blob.prof);
  if (s.blob.hist) plat::psFree(s.blob.hist);
  if (s.blob.days) plat::psFree(s.blob.days);
  if (s.csv) plat::psFree(s.csv);
  s = DumpSnap();
}

void dumpSetup(DumpWriter & w, const DumpSnap & s, int64_t now, const char * fw) {
  w = DumpWriter();
  w.at = now; w.fw = fw ? fw : "";
  w.add("/vn_hist.bin", s.blob.hist, s.blob.histLen);
  w.add("/vn_prof.bin", s.blob.prof, s.blob.profLen);
  if (s.blob.days) w.add("/vn_days.bin", s.blob.days, s.blob.daysLen);
  if (s.csv && s.csvLen) w.add("/vn_stations.csv", s.csv, s.csvLen);
}

const char * CmdLine::feed(char c) {
  if (c == '\r' || c == '\n') {
    bool bad = over;
    buf[n] = 0;
    int len = n;
    n = 0; over = false;
    if (bad || len == 0) return nullptr;
    // espaces de tête et de queue retirés
    char * s = buf;
    while (*s == ' ' || *s == '\t') s++;
    char * e = buf + len;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return *s ? s : nullptr;
  }
  if (n >= (int)sizeof(buf) - 1) { over = true; return nullptr; }
  buf[n++] = c;
  return nullptr;
}

} // namespace vn
