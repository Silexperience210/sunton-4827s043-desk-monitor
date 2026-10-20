// ── Simulateur headless : rend l'UI réelle (même code que l'ESP32) ────
// 1. charge un instantané réel des flux GBFS Vel'OH! + Open-Meteo (fichiers JSON),
// 2. fabrique un historique SYNTHÉTIQUE plausible (24 h + profils) pour illustrer
//    graphiques et prévisions — sur l'appareil, ils s'apprennent en temps réel,
// 3. joue un scénario tactile et enregistre des captures 480×272 (PPM).
//
// Usage : sim <dossier_json> <dossier_sortie>
#include "lvgl.h"
#include "../src/ui/ui.h"
#include "../src/ui/ui_internal.h"
#include "../src/core/model.h"
#include "../src/core/settings.h"
#include "../src/core/gbfs.h"
#include "../src/core/weather.h"
#include "../src/platform/platform.h"
#include "../src/hal/touch_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <sys/stat.h>
#include <pthread.h>
#include "../src/net/net.h"
#include "../src/core/ops.h"
#include "../src/core/service.h"
#include <string>
#include <time.h>

namespace ui { lv_obj_t * debugToast(); int debugTab(); const char * debugDashText(int w);
               const char * debugDashPrio(int row, int field);
               int debugDashCurvePoints(); int debugDashCurveLastX(); int debugDashForecastPoints(); }
namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern bool g_fetching; extern const char * g_fsDir;
                extern int g_wifiState; extern bool g_timeOk; extern const char * g_stage; extern int g_wifiReason;
                extern vn::FeedStat g_feed[vn::FEED_COUNT]; extern int64_t g_timeSyncAt; }
using namespace vn;

static uint16_t s_fb[480 * 272];
static lv_color_t s_buf[480 * 272];
static int s_px = 0, s_py = 0;
static bool s_pressed = false;
static std::string s_out;
static int s_shot = 0;

static void flush(lv_disp_drv_t * d, const lv_area_t * a, lv_color_t * px) {
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) s_fb[y * 480 + x] = (px++)->full;
  lv_disp_flush_ready(d);
}
// ── Panneau tactile simulé ───────────────────────────────────────────
// Modèle physique : 0 = GT911 de la carte (brut = pixels), 1 = résistif typique
// (axes échangés, X en miroir, brut 12 bits avec marges) pour éprouver l'étalonnage.
static int s_phys = 0;
static bool s_resistive = false;
static hal::TouchSample s_ts = { 0, 0, 0, 0, false, 0 };
static void physRaw(int x, int y, int & rx, int & ry) {
  if (s_phys == 0) { rx = x; ry = y; return; }
  // écran x → brut Y (miroir), écran y → brut X ; ~7,5 brut/px, décalages
  ry = 3860 - x * 3600 / 479;
  rx = 230 + y * 3570 / 271;
}
namespace hal {
TouchSample touchLast() { return s_ts; }
const char * touchName() { return s_phys ? "XPT2046 (simul\xC3\xA9)" : "GT911 @0x5D 480x272"; }
bool touchIsResistive() { return s_resistive; }
void touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap) {
  if (!s_resistive) { x0 = 0; x1 = 479; y0 = 0; y1 = 271; swap = false; return; }
  x0 = 200; x1 = 3850; y0 = 240; y1 = 3800; swap = false;      // valeurs d'usine de board.h
}
}
static void readTouch(lv_indev_drv_t *, lv_indev_data_t * d) {
  if (s_pressed) {
    int rx, ry;
    physRaw(s_px, s_py, rx, ry);
    s_ts.rx = (int16_t)rx; s_ts.ry = (int16_t)ry;
    hal::touchToScreen(rx, ry, s_ts.x, s_ts.y);
    s_ts.seq++;
  }
  s_ts.pressed = s_pressed;
  d->point.x = s_ts.x; d->point.y = s_ts.y;
  d->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ── Audit tactile : chaque élément cliquable visible reçoit-il le tap à son endroit ?
static int s_auditOk = 0, s_auditFail = 0, s_auditSmall = 0;
static bool visibleArea(lv_obj_t * o, lv_area_t & a) {
  lv_obj_get_coords(o, &a);
  lv_area_t scr = { 0, 0, 479, 271 };
  if (!_lv_area_intersect(&a, &a, &scr)) return false;
  for (lv_obj_t * p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
    lv_area_t pa;
    lv_obj_get_coords(p, &pa);
    if (!_lv_area_intersect(&a, &a, &pa)) return false;
  }
  return true;
}
static void describe(lv_obj_t * o, char * out, size_t n) {
  // premier texte trouvé dans le sous-arbre
  if (lv_obj_check_type(o, &lv_label_class)) { snprintf(out, n, "%s", lv_label_get_text(o)); return; }
  uint32_t c = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < c; i++) {
    describe(lv_obj_get_child(o, i), out, n);
    if (out[0]) return;
  }
  if (lv_obj_check_type(o, &lv_switch_class)) snprintf(out, n, "[interrupteur]");
  else if (lv_obj_check_type(o, &lv_slider_class)) snprintf(out, n, "[curseur]");
  else if (lv_obj_check_type(o, &lv_keyboard_class)) snprintf(out, n, "[clavier]");
  else if (c && lv_obj_check_type(lv_obj_get_child(o, 0), &lv_img_class)) snprintf(out, n, "[ic\xC3\xB4ne]");
}
static lv_obj_t * hitAt(lv_point_t p) {
  lv_obj_t * f = lv_indev_search_obj(lv_layer_sys(), &p);
  if (!f) f = lv_indev_search_obj(lv_layer_top(), &p);
  if (!f) f = lv_indev_search_obj(lv_scr_act(), &p);
  return f;
}
static bool isAncestor(lv_obj_t * anc, lv_obj_t * o) {
  for (; o; o = lv_obj_get_parent(o)) if (o == anc) return true;
  return false;
}
static void auditTree(lv_obj_t * o, const char * where, FILE * rep) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  bool clickable = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && o != lv_scr_act() && o != lv_layer_top() && o != lv_layer_sys();
  if (clickable) {
    // amener l'élément à l'écran s'il est dans une zone défilante
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    lv_obj_update_layout(lv_scr_act());
    lv_area_t a;
    if (visibleArea(o, a)) {
      lv_coord_t ext = o->spec_attr ? o->spec_attr->ext_click_pad : 0;
      lv_coord_t w = lv_area_get_width(&a) + 2 * ext, h = lv_area_get_height(&a) + 2 * ext;
      lv_point_t pts[5] = { { (lv_coord_t)((a.x1 + a.x2) / 2), (lv_coord_t)((a.y1 + a.y2) / 2) },
                            { (lv_coord_t)(a.x1 + 3), (lv_coord_t)(a.y1 + 3) }, { (lv_coord_t)(a.x2 - 3), (lv_coord_t)(a.y1 + 3) },
                            { (lv_coord_t)(a.x1 + 3), (lv_coord_t)(a.y2 - 3) }, { (lv_coord_t)(a.x2 - 3), (lv_coord_t)(a.y2 - 3) } };
      int hit = -1;
      lv_obj_t * blocker = nullptr;
      for (int k = 0; k < 5 && hit < 0; k++) {
        lv_obj_t * f = hitAt(pts[k]);
        if (f && isAncestor(o, f)) hit = k; else if (!blocker) blocker = f;
      }
      char d[64] = "";
      describe(o, d, sizeof(d));
      bool small = w < 24 || h < 24;
      if (hit >= 0) { s_auditOk++; if (small) s_auditSmall++; }
      else s_auditFail++;
      char bd[64] = "";
      if (hit < 0 && blocker) describe(blocker, bd, sizeof(bd));
      fprintf(rep, "%-10s %-4s %3dx%-3d @(%3d,%3d)  %s%s%s%s\n", where, hit >= 0 ? (hit == 0 ? "OK" : "OK*") : "KO",
              w, h, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, d, small ? "  [petite cible]" : "",
              hit < 0 ? "  \xE2\x86\x90 masqu\xC3\xA9 par : " : "", hit < 0 ? bd : "");
    }
  }
  uint32_t c = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < c; i++) auditTree(lv_obj_get_child(o, i), where, rep);
}
static lv_obj_t * findClickableWithText(lv_obj_t * o, const char * txt) {
  if (lv_obj_check_type(o, &lv_label_class) && strcmp(lv_label_get_text(o), txt) == 0) {
    for (lv_obj_t * p = o; p; p = lv_obj_get_parent(p)) if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) return p;
  }
  uint32_t c = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < c; i++) { lv_obj_t * f = findClickableWithText(lv_obj_get_child(o, i), txt); if (f) return f; }
  return nullptr;
}
static void audit(const char * where, FILE * rep) {
  // seule la couche active la plus haute reçoit les taps : on audite celle-ci
  if (lv_obj_get_child_cnt(lv_layer_sys())) auditTree(lv_layer_sys(), where, rep);
  else if (lv_obj_get_child_cnt(lv_layer_top()) && ui::sheetIsOpen()) auditTree(lv_layer_top(), where, rep);
  else auditTree(lv_scr_act(), where, rep);
}

static void run(int ms) {
  for (int t = 0; t < ms; t += 16) {
    sim::g_ms += 16;
    plat::modelLock();
    lv_timer_handler();
    plat::modelUnlock();
  }
}
static bool s_namedShots = false;            // scénario « ops » : le nom est le nom de fichier
static void shot(const char * name) {
  run(40);
  char f[512];
  if (s_namedShots) snprintf(f, sizeof(f), "%s/%s.ppm", s_out.c_str(), name);
  else snprintf(f, sizeof(f), "%s/%02d_%s.ppm", s_out.c_str(), s_shot++, name);
  FILE * fp = fopen(f, "wb");
  if (!fp) { fprintf(stderr, "impossible d'écrire %s\n", f); exit(1); }
  fprintf(fp, "P6\n480 272\n255\n");
  for (int i = 0; i < 480 * 272; i++) {
    uint16_t c = s_fb[i];
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    uint8_t px[3] = { (uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)), (uint8_t)((b << 3) | (b >> 2)) };
    fwrite(px, 1, 3, fp);
  }
  fclose(fp);
  printf("capture %s\n", f);
}
// Centre vertical de l'onglet t du rail (six onglets de 39 px sous l'en-tête de 36 px)
static int tabY(int t) { return 36 + t * (236 / ui::TAB_COUNT) + 236 / ui::TAB_COUNT / 2; }
static void tap(int x, int y, int holdMs = 60) {
  s_px = x; s_py = y; s_pressed = true; run(holdMs);
  s_pressed = false; run(50);
}
static void drag(int x1, int y1, int x2, int y2, int steps = 12) {
  s_px = x1; s_py = y1; s_pressed = true; run(48);
  for (int i = 1; i <= steps; i++) { s_px = x1 + (x2 - x1) * i / steps; s_py = y1 + (y2 - y1) * i / steps; run(16); }
  run(32);
  s_pressed = false; run(400);
}

static std::vector<char> readFile(const std::string & p) {
  std::vector<char> v;
  FILE * f = fopen(p.c_str(), "rb");
  if (!f) { fprintf(stderr, "introuvable : %s\n", p.c_str()); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  v.resize(n);
  if (fread(v.data(), 1, n, f) != (size_t)n) exit(1);
  fclose(f);
  return v;
}

// ── Historique synthétique (illustration) ────────────────────────────
static float hrand(uint32_t x) {                 // bruit déterministe 0…1
  x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
  return (x & 0xFFFFFF) / 16777215.f;
}
static float S(float h, float c, float w) { return 1.f / (1.f + expf(-(h - c) / w)); }
static float pattern(int type, int dow, float h) {
  float f;
  if (type == 0) f = 0.24f + 0.50f * (1 - S(h, 7.9f, 0.6f) + S(h, 18.2f, 0.9f));        // résidentiel
  else if (type == 1) f = 0.16f + 0.62f * (S(h, 8.6f, 0.6f) - S(h, 17.5f, 0.8f));       // bureaux / gare
  else f = 0.5f + 0.13f * sinf(6.2832f * (h - 9.f) / 24.f);                             // mixte
  if (dow == 0 || dow == 6) f = 0.5f + (f - 0.5f) * 0.35f - 0.12f * expf(-powf((h - 15.f) / 2.5f, 2));
  return fminf(0.97f, fmaxf(0.02f, f));
}

static void synthHistory(Model & m, int64_t now, const StatusRec * recs, int nrec) {
  struct Syn { float capE, live, off; int type; bool ok; };
  static Syn syn[VN_MAX_ST];
  for (int i = 0; i < m.n; i++) syn[i].ok = false;
  for (int k = 0; k < nrec; k++) {
    int i = m.find(recs[k].sid);
    if (i < 0 || !m.st[i].active) continue;
    Syn & s = syn[i];
    s.capE = recs[k].bikes + recs[k].docks;
    if (s.capE < 1 || !recs[k].renting) continue;
    s.live = recs[k].bikes;
    uint32_t hsh = (uint32_t)atoi(recs[k].sid) * 2654435761u;
    float lat = m.st[i].lat, lon = m.st[i].lon;
    bool kirch = lat > 49.618f && lon > 6.14f, gare = lat < 49.603f && lat > 49.597f && lon > 6.125f && lon < 6.14f;
    s.type = (kirch || gare) ? 1 : (hrand(hsh) < 0.6f ? 0 : 2);
    struct tm lt; plat::localTime(now, &lt);
    float h = lt.tm_hour + lt.tm_min / 60.f;
    s.off = s.live / s.capE - pattern(s.type, lt.tm_wday, h);
    s.ok = true;
  }
  auto value = [&](int i, int64_t t, int salt) {
    const Syn & s = syn[i];
    struct tm lt; plat::localTime(t, &lt);
    float h = lt.tm_hour + lt.tm_min / 60.f;
    float decay = expf(-(float)(now - t) / (3.5f * 3600.f));
    float f = pattern(s.type, lt.tm_wday, h) + s.off * decay + 0.05f * (hrand((uint32_t)(i * 7919 + salt / 6)) - 0.5f);
    return fminf(s.capE, fmaxf(0.f, roundf(f * s.capE)));
  };
  // Profils (≈ 3 semaines apprises)
  for (int i = 0; i < m.n; i++) {
    if (!syn[i].ok) continue;
    for (int d = 0; d < 7; d++)
      for (int q = 0; q < 96; q++) {
        float f = pattern(syn[i].type, d, q / 4.f + 0.125f) + 0.25f * syn[i].off;
        float v = fminf(syn[i].capE, fmaxf(0.f, f * syn[i].capE));
        ProfCell & c = m.prof[(size_t)i * 7 * 96 + d * 96 + q];
        c.a = (uint16_t)(v * 256); c.n = (uint8_t)(7 + (int)(hrand(i * 131 + d * 97 + q) * 6));
      }
  }
  for (int c = 0; c < 7 * 96; c++) m.cellN[c] = 9;
  m.learnSince = now - 23 * 86400;
  m.learnSamples = 23 * 288;
  // 24 h au pas de 5 min (jusqu'à la tranche précédente)
  int64_t slotNow = now - now % VN_RING_STEP_S;
  int slots = VN_RING_SLOTS - 1;
  m.head = -1; m.count = 0;
  static float prev[VN_MAX_ST];
  for (int k = slots; k >= 1; k--) {
    int64_t t = slotNow - (int64_t)k * VN_RING_STEP_S;
    m.head = (m.head + 1) % VN_RING_SLOTS;
    m.count++;
    m.ringT[m.head] = t;
    uint8_t * row = m.ring + (size_t)m.head * VN_MAX_ST;
    NetSample & ns = m.net[m.head];
    memset(&ns, 0, sizeof(ns));
    ns.t = t;
    netWxUnknown(ns);                              // historique synthétique : météo non reconstituée
    int av = 0, dk = 0, dis = 0, raw = 0, mv = 0;
    for (int i = 0; i < VN_MAX_ST; i++) {
      if (i >= m.n || !syn[i].ok) { row[i] = 255; continue; }
      float v = value(i, t, k);
      row[i] = (uint8_t)v;
      av += (int)v; dk += (int)(syn[i].capE - v); raw += (int)v; dis += 1 + (i % 3);
      if (k < slots) mv += (int)fabsf(v - prev[i]);
      prev[i] = v;
    }
    ns.avail = (uint16_t)av; ns.docks = (uint16_t)dk; ns.dis = (uint16_t)dis; ns.bikesRaw = (uint16_t)raw;
    struct tm lt; plat::localTime(t, &lt);
    float act = 0.35f + 0.65f * (expf(-powf((lt.tm_hour + lt.tm_min / 60.f - 8.3f) / 1.4f, 2)) + expf(-powf((lt.tm_hour + lt.tm_min / 60.f - 17.8f) / 1.8f, 2)) + 0.45f * expf(-powf((lt.tm_hour - 12.5f) / 1.6f, 2)));
    if (lt.tm_hour < 6) act *= 0.25f;
    int moves = (int)(mv * 0.12f + act * 46);
    ns.dep = (uint16_t)(moves / 2 + (int)(hrand(k) * 4)); ns.ret = (uint16_t)(moves - moves / 2);
  }
  // 30 dernières minutes au pas de 1 min (tendance)
  m.rHead = -1; m.rCount = 0;
  for (int k = 30; k >= 1; k--) {
    int64_t t = now - k * 60;
    m.rHead = (m.rHead + 1) % VN_RECENT_SLOTS;
    m.rCount++;
    m.recT[m.rHead] = t;
    uint8_t * row = m.rec + (size_t)m.rHead * VN_MAX_ST;
    for (int i = 0; i < VN_MAX_ST; i++) row[i] = (i < m.n && syn[i].ok) ? (uint8_t)value(i, t, 5000 + k) : 255;
  }
  // Dernières variations (deux stations « figées » pour illustrer l'alerte)
  int frozen = 0;
  for (int i = 0; i < m.n; i++) {
    m.st[i].lastChange = now - (int64_t)(hrand(i * 31) * 1800);
    if (syn[i].ok && frozen < 2 && hrand(i * 977) < 0.03f) { m.st[i].lastChange = now - 5 * 3600 - 600 * frozen; frozen++; }
  }
}


// ── Compteurs d'ingestion du simulateur : ce qu'il a RÉELLEMENT lu ────
// Pas de réseau ici : « octets » = taille du fichier JSON, « ms » = durée mesurée
// de la lecture + analyse par le même parseur que la carte (horloge monotone).
static uint32_t msSince(const timespec & a) {
  timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
  return (uint32_t)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
}
static void simFeeds(const std::string & in, int64_t now) {
  static const char * FILES[vn::FEED_COUNT] = { "station_status.json", "station_information.json", "vehicle_types.json", "weather.json" };
  static StatusRec sr[VN_MAX_ST];
  static InfoRec ir[VN_MAX_ST];
  for (int f = 0; f < vn::FEED_COUNT; f++) {
    timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    auto v = readFile(in + "/" + FILES[f]);
    bool ok = false;
    ElecTypes et; Weather w; int64_t lu;
    switch (f) {
      case vn::FEED_STATUS: ok = gbfsParseStatus(v.data(), v.size(), sr, VN_MAX_ST, et, &lu) > 0; break;
      case vn::FEED_INFO: ok = gbfsParseInfo(v.data(), v.size(), ir, VN_MAX_ST) > 0; break;
      case vn::FEED_TYPES: ok = gbfsParseTypes(v.data(), v.size(), et); break;
      default: ok = wxParse(v.data(), v.size(), w); break;
    }
    uint32_t ms = msSince(t0);
    if (ok) vn::ingestOk(sim::g_feed[f], now, (uint32_t)v.size(), ms);
    else vn::ingestErr(sim::g_feed[f], now, vn::ING_E_PARSE, (uint32_t)v.size(), ms);
  }
}

// ── Scénario « totaux » : les chiffres du tableau de bord sur un instantané ──
// Sert à confronter, chiffre par chiffre, ce que le firmware calcule et ce que
// le flux brut contient réellement (audit des chiffres affichés).
// Aucun historique n'est fabriqué : c'est l'état d'une carte fraîchement flashée.
static int totauxScenario(const std::string & in) {
  auto info = readFile(in + "/station_information.json");
  auto status = readFile(in + "/station_status.json");
  auto types = readFile(in + "/vehicle_types.json");
  static StatusRec recs[VN_MAX_ST];
  static InfoRec irecs[VN_MAX_ST];
  ElecTypes et;
  gbfsParseTypes(types.data(), types.size(), et);
  int64_t lastUpd = 0;
  int ns = gbfsParseStatus(status.data(), status.size(), recs, VN_MAX_ST, et, &lastUpd);
  int ni = gbfsParseInfo(info.data(), info.size(), irecs, VN_MAX_ST);
  if (ni <= 0 || ns <= 0) { fprintf(stderr, "flux illisible (info=%d status=%d)\n", ni, ns); return 1; }

  sim::g_epoch0 = lastUpd ? lastUpd : 1791894641;
  sim::g_timeOk = true;
  settingsDefaults(g_set);
  if (!g_model.init()) { fprintf(stderr, "init modèle\n"); return 1; }

  int64_t now = plat::now();
  g_model.applyInfo(irecs, ni, now);
  g_model.applyStatus(recs, ns, now);           // aucun historique synthétique
  const vn::Totals & t = g_model.tot;

  printf("FLUX        info=%d publiques · status=%d relevés · last_updated=%lld · types elec=%d\n",
         ni, ns, (long long)lastUpd, et.n);
  printf("STATIONS    %d (ouvertes %d · fermées %d)\n", t.stations, t.open, t.closed);
  printf("VELOS       %d  (elec %d)\n", t.bikes, t.elec);
  printf("BORNES      %d\n", t.docks);
  printf("HS          velos %d · bornes %d\n", t.bikesDis, t.docksDis);
  printf("CAPACITE    %d · velos bruts %d\n", t.cap, t.bikesRaw);
  printf("REMPLISSAGE %.1f %%\n", t.fillPct);
  printf("HISTORIQUE  %d echantillon(s) · en-trajet valide=%s · variation valide=%s · activite valide=%s\n",
         g_model.count, t.inUseValid ? "oui" : "non", t.deltaValid ? "oui" : "non", t.actValid ? "oui" : "non");
  printf("PARC        %d (estimation)\n", t.fleet);
  printf("EN-TRAJET   %d (estimation)\n", t.inUse);
  printf("VAR 1 H     %+d · retraits %d · retours %d\n", t.delta1h, t.dep1h, t.ret1h);
  int nbElecMoins = 0, minB = 1 << 30, maxB = 0;
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (s.elec < s.bikes) nbElecMoins++;
    minB = s.bikes < minB ? s.bikes : minB;
    maxB = s.bikes > maxB ? s.bikes : maxB;
  }
  printf("PAR STATION elec<velos dans %d station(s) · velos de %d a %d\n", nbElecMoins, minB, maxB);
  return 0;
}

// ── Scénario « premier démarrage » : comme sur la carte ─────────────
// Heure inconnue (avant NTP), aucun Wi-Fi enregistré, aucune donnée, aucun historique.
static int bootScenario(const std::string & in, const char * variant) {
  bool savedSsid = variant && strcmp(variant, "boot-wifi-ko") == 0;
  auto info = readFile(in + "/station_information.json");
  auto status = readFile(in + "/station_status.json");
  auto types = readFile(in + "/vehicle_types.json");
  auto wx = readFile(in + "/weather.json");
  static StatusRec recs[VN_MAX_ST];
  static InfoRec irecs[VN_MAX_ST];
  ElecTypes et;
  gbfsParseTypes(types.data(), types.size(), et);
  int64_t lastUpd = 0;
  int ns = gbfsParseStatus(status.data(), status.size(), recs, VN_MAX_ST, et, &lastUpd);
  int ni = gbfsParseInfo(info.data(), info.size(), irecs, VN_MAX_ST);

  sim::g_epoch0 = 0;                    // heure inconnue
  sim::g_timeOk = false;
  settingsDefaults(g_set);
  if (savedSsid) {                      // réseau enregistré mais injoignable (mauvais mot de passe…)
    strcpy(g_set.ssid, "BUREAU");
    sim::g_wifiState = net::W_CONNECTING; sim::g_stage = "Connexion Wi-Fi\xE2\x80\xA6";
  } else {
    sim::g_wifiState = net::W_NONE; sim::g_stage = "Wi-Fi \xC3\xA0 configurer";
  }
  if (!g_model.init()) { fprintf(stderr, "init modèle\n"); return 1; }
  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 40);      // comme sur la carte : 40 lignes
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);
  ui::init(disp);
  run(16);
  shot("boot_premier_rendu");
  run(800);
  shot("boot_accueil");
  if (savedSsid) {
    run(15000);
    sim::g_wifiState = net::W_FAILED; sim::g_stage = "Wi-Fi injoignable";
    run(1500);
    shot("boot_wifi_echec");
    bool stuck = lv_obj_get_child_cnt(lv_layer_top()) && !ui::sheetIsOpen();
    printf("Wi-Fi en échec : %s\n", stuck ? "BLOQUÉ sur l'écran d'accueil" : "écran d'accueil fermé ✓");
    // parcours des onglets sans aucune donnée ni heure
    for (int t = 0; t < ui::TAB_COUNT; t++) { ui::sheetClose(); run(300); tap(30, tabY(t)); run(600); char n[32]; snprintf(n, sizeof n, "boot_vide_onglet%d", t); shot(n); }
    return stuck ? 3 : 0;
  }
  run(2000);
  shot("boot_wifi");
  bool wifiOk = ui::sheetIsOpen();
  printf("Premier démarrage : feuille Wi-Fi %s\n", wifiOk ? "ouverte ✓" : "ABSENTE");
  // l'utilisateur choisit un réseau
  strcpy(g_set.ssid, "VELOH-OPS");
  ui::sheetClose();
  sim::g_wifiState = net::W_CONNECTING; sim::g_stage = "Connexion Wi-Fi\xE2\x80\xA6";
  run(1500);
  shot("boot_connexion");
  sim::g_wifiState = net::W_CONNECTED; sim::g_stage = "Synchronisation de l'heure\xE2\x80\xA6";
  run(2000);
  sim::g_epoch0 = lastUpd ? lastUpd : 1791894641; sim::g_timeOk = true;
  sim::g_stage = "Chargement des stations Vel'OH!\xE2\x80\xA6";
  run(1500);
  shot("boot_heure_ok");
  // premières données, sans aucun historique
  int64_t now = plat::now();
  { plat::LockGuard g; g_model.applyInfo(irecs, ni, now); }
  run(500);
  { plat::LockGuard g; g_model.elec = et; g_model.applyStatus(recs, ns, now); g_model.feedUpdated = lastUpd; }
  run(1500);
  shot("boot_donnees");
  bool dashOk = ui::debugTab() == ui::TAB_DASH && !lv_obj_get_child_cnt(lv_layer_top());
  printf("Premières données : tableau de bord %s\n", dashOk ? "affiché ✓" : "ABSENT");
  { Weather w; if (wxParse(wx.data(), wx.size(), w)) { w.fetchedAt = now; plat::LockGuard g; g_model.wx = w; g_model.recompute(now); } }
  run(800);
  for (int t = 0; t < ui::TAB_COUNT; t++) { tap(30, tabY(t)); run(600); char n[32]; snprintf(n, sizeof n, "boot_onglet%d", t); shot(n); }
  ui::openStation(0); run(600); shot("boot_fiche"); ui::sheetClose(); run(300);
  // deuxième relevé une minute plus tard
  sim::g_ms += 60000;
  { plat::LockGuard g; g_model.applyStatus(recs, ns, plat::now()); }
  run(1000);
  shot("boot_releve2");
  return (wifiOk && dashOk) ? 0 : 3;
}

// ── Scénario « ops » : captures des écrans d'exploitation (lot 1) ────
// Noms de fichiers explicites ; chaque étape vérifie aussi ce qu'elle montre.
static lv_obj_t * clickableText(const char * txt) {
  lv_obj_t * o = findClickableWithText(lv_layer_top(), txt);
  return o ? o : findClickableWithText(lv_scr_act(), txt);
}
static void tapObj(lv_obj_t * o) {
  if (!o) { printf("  (cible introuvable)\n"); return; }
  lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
  run(50);
  lv_area_t a; lv_obj_get_coords(o, &a);
  tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}
static bool textVisible(lv_obj_t * o, const char * prefix) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  if (lv_obj_check_type(o, &lv_label_class) && strncmp(lv_label_get_text(o), prefix, strlen(prefix)) == 0) return true;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) if (textVisible(lv_obj_get_child(o, i), prefix)) return true;
  return false;
}
static int opsScenario(const std::string & in, StatusRec * recs, int ns) {
  s_namedShots = true;
  int fails = 0;
  auto expect = [&](bool c, const char * what) { printf("  %s %s\n", c ? "✓" : "ÉCHEC", what); if (!c) fails++; };
  run(900);
  // 1. Fraîcheur : 12 minutes sans relevé réussi (seuil auto : 3 min)
  sim::g_ms += 12 * 60 * 1000;
  run(600);
  shot("01_fraicheur");
  expect(textVisible(lv_scr_act(), "Données vieilles de 12 min"), "bandeau « Données vieilles de 12 min » affiché");
  // un relevé frais arrive : le bandeau disparaît
  { plat::LockGuard g; g_model.applyStatus(recs, ns, plat::now()); g_model.feedUpdated = plat::now() - 20; }
  simFeeds(in, plat::now());
  run(600);
  expect(!textVisible(lv_scr_act(), "Données vieilles"), "bandeau retiré après un relevé frais");
  shot("01b_fraicheur_retablie");
  // 2. Provenance : tap réel sur les tuiles « Vélos dispo » et « En trajet »
  tap(60 + 8 + 48, 36 + 8 + 37);
  run(500);
  expect(ui::sheetIsOpen() && textVisible(lv_layer_top(), "Provenance · vélos disponibles"), "tap sur VÉLOS DISPO → provenance");
  shot("02_provenance_velos");
  drag(300, 240, 300, 90, 10);
  shot("02b_provenance_velos_suite");
  ui::sheetClose(); run(300);
  tap(60 + 8 + 48 + 102, 36 + 8 + 37);
  run(500);
  expect(textVisible(lv_layer_top(), "Provenance · vélos en trajet"), "tap sur EN TRAJET → provenance");
  shot("02c_provenance_en_trajet");
  ui::sheetClose(); run(300);
  // 3. Tableau d'ingestion depuis Réglages
  tap(30, tabY(ui::TAB_SETTINGS));
  run(500);
  {
    lv_obj_t * ing = clickableText("Ingestion des flux");
    if (ing) { lv_obj_scroll_to_view_recursive(ing, LV_ANIM_OFF); run(100); }
    shot("03_reglages_fraicheur_ingestion");
    tapObj(ing);
  }
  run(500);
  expect(textVisible(lv_layer_top(), "Ingestion des flux"), "Réglages → Ingestion des flux");
  shot("03b_ingestion");
  drag(300, 240, 300, 60, 10);
  shot("03c_ingestion_suite");
  ui::sheetClose(); run(300);
  // 4. Prévision 24 h depuis la fiche station (bouton « 24 h »)
  int st = g_model.nAlerts ? g_model.alerts[0].st : 0;
  ui::openStation(st); run(500);
  tapObj(clickableText("24 h"));
  run(600);
  expect(textVisible(lv_layer_top(), "Prévision 24 h"), "fiche → 24 h");
  shot("04_prevision_24h");
  drag(300, 240, 300, 80, 10);
  shot("04b_prevision_24h_suite");
  ui::sheetClose(); run(300);
  // Profil jeune (illustration) : profil de cette station réduit à 1 relevé par créneau
  // au-delà des 10 prochaines heures — cette partie de la courbe doit passer en pointillés.
  {
    plat::LockGuard g;
    struct tm lt; plat::localTime(plat::now(), &lt);
    int dow0 = lt.tm_wday, q0 = (lt.tm_hour * 60 + lt.tm_min) / 15;
    for (int k = 40; k < 7 * 96 - 8; k++) {
      int q = q0 + k, d = (dow0 + q / 96) % 7;
      g_model.prof[(size_t)st * 7 * 96 + d * 96 + q % 96].n = 1;
    }
  }
  ui::openForecast24(st); run(600);
  shot("04c_prevision_24h_profil_jeune");
  ui::sheetClose(); run(300);
  // 5. Alertes, filtre « Inhabituel » (le libellé porte le compteur)
  tap(30, tabY(ui::TAB_ALERTS));
  run(500);
  {
    char b[24];
    snprintf(b, sizeof(b), "Inhabituel %d", g_model.alertCount[AL_UNUSUAL]);
    tapObj(clickableText(b));
  }
  run(500);
  expect(g_model.alertCount[AL_UNUSUAL] > 0, "au moins une alerte « inhabituel » sur ces données");
  shot("05_alertes_inhabituel");
  // 6. Fiche d'une station inhabituelle : la ligne d'écart
  int an = -1;
  for (int k = 0; k < g_model.nAlerts && an < 0; k++) if (g_model.alerts[k].type == AL_UNUSUAL) an = g_model.alerts[k].st;
  if (an >= 0) {
    ui::openStation(an); run(500);
    drag(300, 240, 300, 40, 10);
    expect(textVisible(lv_layer_top(), "Inhabituel : "), "fiche : ligne « Inhabituel : N vélos contre P attendus (± T) »");
    shot("06_fiche_ecart");
    ui::sheetClose(); run(300);
  }
  // 7. Météo apprise (étapes 3a / 4) : état d'un coefficient appris injecté — α = 0,55 ± 0,12 sur
  // 184 fenêtres pluvieuses —, il pleut maintenant, accalmie annoncée dans 5 h.
  int wst = -1;
  {
    plat::LockGuard g;
    WxStats & w = g_model.wxG;
    memset(&w, 0, sizeof(w));
    w.sdd = 1000; w.szz = 10; w.szr = -5.5f; w.sgg = 1.44f; w.sn = 50000; w.nRain = 184;
    for (int h = 0; h < 24; h++) { g_model.wxClim.sx[h] = 0.6f; g_model.wxClim.sw[h] = 10; }
    Weather & wx = g_model.wx;
    wx.rain = 1.4f; wx.code = 63; wx.fetchedAt = plat::now();
    for (int i = 0; i < wx.nfc; i++) {
      bool r = i < 5;
      wx.fc[i].rain = r ? 1.2f : 0.f; wx.fc[i].prob = r ? 85 : 10; wx.fc[i].code = r ? 63 : 2;
    }
    wxScore(wx);
    g_model.recompute(plat::now());
    float best = 0;
    for (int i = 0; i < g_model.n; i++) {
      const Station & s = g_model.st[i];
      if (s.active && s.status != ST_CLOSED && fabsf(s.fcWx[24]) > best) { best = fabsf(s.fcWx[24]); wst = i; }
    }
    printf("  météo : α %.2f ± %.2f, station %d, part +2 h %+.1f\n", g_model.wxAlpha, g_model.wxAlphaSE, wst, wst >= 0 ? g_model.st[wst].fcWx[24] : 0.f);
  }
  expect(g_model.wxSrc == WXC_LEARNED && wst >= 0 && fabsf(g_model.st[wst].fcWx[24]) >= 0.5f, "coefficient appris et part météo notable sur une station");
  if (wst >= 0) {
    ui::openStation(wst); run(500);
    drag(300, 240, 300, 40, 10);
    drag(300, 200, 300, 120, 10);
    expect(textVisible(lv_layer_top(), "M\xC3\xA9t\xC3\xA9o : dont "), "fiche : « Météo : dont N vélos à HH:MM liés à la météo actuelle »");
    shot("07_fiche_meteo");
    ui::sheetClose(); run(300);
    ui::openForecast24(wst); run(600);
    drag(300, 240, 300, 40, 10);
    expect(textVisible(lv_layer_top(), "dont "), "24 h : ligne MÉTÉO avec la part la plus marquée");
    shot("07b_prevision_24h_meteo");
    ui::sheetClose(); run(300);
  }
  ui::openProvenance(ui::PG_WEATHER); run(500);
  drag(300, 240, 300, 40, 10);
  drag(300, 240, 300, 40, 10);
  drag(300, 240, 300, 110, 10);
  expect(textVisible(lv_layer_top(), "Confiance "), "provenance +1 h : état de la météo dans le détail");
  shot("07c_provenance_prevision_meteo");
  ui::sheetClose(); run(300);
  // 8. Météo indisponible : même fiche, l'écran le dit, prévision sans part météo
  { plat::LockGuard g; g_model.wx = Weather(); g_model.recompute(plat::now()); }
  if (wst >= 0) {
    expect(g_model.st[wst].fcWx[24] == 0, "météo indisponible : aucune part météo");
    ui::openStation(wst); run(500);
    drag(300, 240, 300, 40, 10);
    drag(300, 200, 300, 120, 10);
    expect(textVisible(lv_layer_top(), "M\xC3\xA9t\xC3\xA9o : m\xC3\xA9t\xC3\xA9o indisponible"), "fiche : « Météo : météo indisponible : prévision sans météo »");
    shot("07d_fiche_meteo_indisponible");
    ui::sheetClose(); run(300);
  }
  // 9. Jour férié (étape 5a-ii) : la veille de Noël au soir, la courbe 24 h couvre le 25/12, prévu
  // avec le profil du dimanche, et le dit ; le 25/12 en journée, la ligne d'écart le dit aussi.
  auto textAny = [&](lv_obj_t * o, const char * sub, auto && self) -> bool {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), sub)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) if (self(lv_obj_get_child(o, i), sub, self)) return true;
    return false;
  };
  {
    struct tm e = {}; e.tm_year = 126; e.tm_mon = 11; e.tm_mday = 24; e.tm_hour = 20; e.tm_isdst = -1;
    sim::g_epoch0 += (int64_t)mktime(&e) - plat::now();
    plat::LockGuard g;
    g_model.applyStatus(recs, ns, plat::now()); g_model.feedUpdated = plat::now() - 20;
  }
  simFeeds(in, plat::now());
  run(600);
  ui::openForecast24(st); run(600);
  drag(300, 240, 300, 40, 10);
  expect(textAny(lv_layer_top(), "Le 25/12 est f\xC3\xA9ri\xC3\xA9 (No\xC3\xABl) : profil du dimanche", textAny), "24 h : « Le 25/12 est férié (Noël) : profil du dimanche »");
  shot("09_prevision_24h_ferie");
  ui::sheetClose(); run(300);
  {
    sim::g_epoch0 += 14 * 3600 + 30 * 60 - 20 * 3600 + 24 * 3600;   // 25/12 14:30
    plat::LockGuard g;
    g_model.applyStatus(recs, ns, plat::now()); g_model.feedUpdated = plat::now() - 20;
  }
  simFeeds(in, plat::now());
  run(600);
  int hs = -1;
  for (int i = 0; i < g_model.n && hs < 0; i++) if (g_model.st[i].anom == AN_NORMAL || g_model.st[i].anom == AN_LOW || g_model.st[i].anom == AN_HIGH) hs = i;
  expect(hs >= 0 && g_model.holidayProfile(plat::now()), "25/12 14:30 : écart au profil évalué, jour férié reconnu");
  if (hs >= 0) {
    ui::openStation(hs); run(500);
    drag(300, 240, 300, 40, 10);
    expect(textAny(lv_layer_top(), "jour f\xC3\xA9ri\xC3\xA9 (No\xC3\xABl) : profil du dimanche", textAny), "fiche : « … · jour férié (Noël) : profil du dimanche »");
    shot("09b_fiche_ferie");
    ui::sheetClose(); run(300);
  }
  // 10. Écart habituel (docs/mesures-prevision.md, « alerte calme ») : dispersion apprise INJECTÉE (le jeu de données réel
  // n'a pas d'historique) — écart habituel ≈ 12 % de la capacité, mesuré depuis 34 jours —, lundi 28/12 14:30.
  {
    sim::g_epoch0 += 3 * 86400;
    plat::LockGuard g;
    g_model.applyStatus(recs, ns, plat::now()); g_model.feedUpdated = plat::now() - 20;
    for (int i = 0; i < g_model.n; i++) {
      float d = fmaxf(0.8f, 0.12f * g_model.st[i].capEff());
      for (int c = 0; c < 7 * 96; c++) { ProfCell & pc = g_model.prof[(size_t)i * 7 * 96 + c]; pc.dn = 7; pc.d = (uint8_t)lroundf(d * 16); }
      g_model.dispSince[i] = plat::now() - 34 * 86400;
    }
    g_model.recompute(plat::now());
  }
  simFeeds(in, plat::now());
  run(600);
  // Réglage livré (formule fixe) : l'écart habituel mesuré est donné pour information
  int ca = -1;
  for (int k = 0; k < g_model.nAlerts && ca < 0; k++) if (g_model.alerts[k].type == AL_UNUSUAL) ca = g_model.alerts[k].st;
  expect(ca >= 0, "une alerte « inhabituel » à ouvrir");
  if (ca >= 0) {
    ui::openStation(ca); run(500);
    drag(300, 240, 300, 40, 10);
    expect(textAny(lv_layer_top(), "(pour information)", textAny),
           "fiche (formule fixe) : « · écart habituel à cette heure ± X, mesuré sur 34 jours (pour information) »");
    shot("10_fiche_ecart_info");
    ui::sheetClose(); run(300);
  }

  printf("Scénario ops : %d échec(s)\n", fails);
  return fails ? 4 : 0;
}

// ── Scénario « service » : quatre semaines du monde à vérité connue (sim/service_world.h) sur les
// vraies stations, puis la vue Service. Rôles : une station qui dérive (vide 8 h–9 h depuis le 08/12),
// une saturée le week-end, un incident isolé, une quasi-seuil, une hors service ; le reste sain,
// bruité ou « pendulaire au hasard ». La vérité est imprimée, les captures montrent ce que dit l'écran.
#include "service_world.h"
namespace ui { void servicePane(int i); lv_obj_t * servicePaneObj(int i); }
static int64_t luxT(int y, int mo, int d, int h, int mi) {
  struct tm lt0; plat::localTime(0, &lt0);                   // fixe TZ (Luxembourg)
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  return (int64_t)mktime(&t);
}
static int serviceScenario(const std::string & in) {
  s_namedShots = true;
  auto info = readFile(in + "/station_information.json");
  auto wx = readFile(in + "/weather.json");
  static InfoRec irecs[VN_MAX_ST];
  static StatusRec sr[VN_MAX_ST];
  int ni = gbfsParseInfo(info.data(), info.size(), irecs, VN_MAX_ST);
  settingsDefaults(g_set);
  strcpy(g_set.ssid, "VELOH-OPS");
  if (!g_model.init()) return 1;
  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 272);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);
  ui::init(disp);

  using namespace world;
  static Role roles[VN_MAX_ST];
  for (int i = 0; i < ni; i++) {
    int k = i % 10;
    roles[i] = k <= 5 ? W_HEALTHY : k == 8 ? W_COMMUTE : W_NOISE;
  }
  const int DRIFT = 5, WEEKEND = 17, ISOL = 33, NEAR = 41, CLOSED = 60;
  roles[DRIFT] = W_DRIFT; roles[WEEKEND] = W_WEEKEND; roles[ISOL] = W_ISOLATED; roles[NEAR] = W_NEAR; roles[CLOSED] = W_CLOSED;
  g_model.applyInfo(irecs, ni, luxT(2026, 11, 30, 0, 0));
  auto play = [&](int fromDay, int toDay, int lastMin) {
    for (int dw = fromDay; dw <= toDay; dw++) {
      if (dw == OFF_DAY) continue;
      const int64_t d0 = luxT(2026, monthOf(dw), dateOf(dw), 0, 0);
      for (int mn = 6 * 60; mn <= 22 * 60 + 30 && (dw < toDay || mn <= lastMin); mn += 5) {
        for (int i = 0; i < ni; i++) {
          bool closed;
          int v = bikes(roles[i], i, dw, mn, closed);
          int cap = irecs[i].cap ? irecs[i].cap : CAP;
          int b = v == 0 ? 0 : v >= CAP ? cap : (int)lroundf((float)v * cap / CAP);
          if (v > 0 && v < CAP) b = b < 1 ? 1 : b > cap - 1 ? cap - 1 : b;
          sr[i] = StatusRec(); memset(&sr[i], 0, sizeof(sr[i]));
          strcpy(sr[i].sid, irecs[i].sid);
          sr[i].bikes = sr[i].elec = (uint16_t)b; sr[i].docks = (uint16_t)(cap - b);
          sr[i].installed = !closed; sr[i].renting = sr[i].returning = 1;
          sr[i].lastReported = d0 + mn * 60 - 40;
        }
        g_model.applyStatus(sr, ni, d0 + mn * 60);
      }
    }
  };
  auto settle = [&](int64_t t) {
    sim::g_epoch0 = t - sim::g_ms / 1000;
    g_model.lastUpdate = t - 30; g_model.feedUpdated = t - 50;
    g_model.wx.fetchedAt = t - 6 * 60;
    wxParse(wx.data(), wx.size(), g_model.wx);
    g_model.nEv = 0;                                          // pas de toast de rupture par-dessus les captures
    g_model.version++;
  };
  // 1. Début : trois jours seulement — l'écran dit qu'il est trop tôt
  play(0, 2, 22 * 60 + 30);
  int64_t t = luxT(2026, 12, 3, 10, 2);
  play(3, 3, 10 * 60);
  settle(t);
  run(600);
  tap(30, tabY(ui::TAB_SERVICE)); run(600);
  ui::servicePane(1); run(400);
  shot("11_service_recurrents_debut");
  ui::servicePane(0); run(300);
  // 2. Quatre semaines : jusqu'au lundi 28/12 10:00
  play(4, 28, 10 * 60);
  t = luxT(2026, 12, 28, 10, 2);
  settle(t);
  run(800);
  shot("12_service_synthese");
  drag(260, 230, 260, 60, 10); run(300);
  shot("12b_service_synthese_suite");
  drag(260, 230, 260, 40, 10); run(300);
  shot("12c_service_comparaisons");
  ui::servicePane(1); run(400);
  shot("13_service_recurrents");
  drag(260, 230, 260, 80, 10); run(300);
  shot("13b_service_recurrents_suite");
  ui::servicePane(2); run(400);
  shot("14_service_classement");
  drag(260, 230, 260, 40, 10); run(300);
  shot("14b_service_classement_suite");
  ui::servicePane(3); run(400);
  shot("15_service_la_carte");
  drag(260, 230, 260, 60, 10); run(300);
  shot("15b_service_la_carte_suite");
  // Vérité contre écran : le détecteur doit trouver la dérive et la saturation du week-end, et se taire sur le reste
  static Recur rec[64];
  int n = svcRecurrent(g_model.days, g_model, localDay(t), rec, 64);
  int found = 0, fpNamed = 0, fpNoise = 0, fpCommute = 0;
  for (int k = 0; k < n; k++) {
    int st = rec[k].st;
    char x[160]; svcRecurText(rec[k], x, sizeof(x));
    printf("  récurrent : [%s] %s — %s\n", roles[st] == W_DRIFT ? "dérive" : roles[st] == W_WEEKEND ? "week-end" : roles[st] == W_NOISE ? "bruit" :
           roles[st] == W_COMMUTE ? "pendulaire" : roles[st] == W_HEALTHY ? "saine" : "autre", g_model.st[st].name, x);
    if ((st == DRIFT && rec[k].kind == RK_EMPTY) || (st == WEEKEND && rec[k].kind == RK_FULL && rec[k].weekend)) found++;
    else if (roles[st] == W_NOISE) fpNoise++;
    else if (roles[st] == W_COMMUTE) fpCommute++;
    else fpNamed++;
  }
  int nNoise = 0, nCommute = 0;
  for (int i = 0; i < ni; i++) { nNoise += roles[i] == W_NOISE; nCommute += roles[i] == W_COMMUTE; }
  printf("Scénario service : %d stations, %d problème(s) ; vrais trouvés %d/2 ; faux positifs : %d (rôles nommés et sains), "
         "%d sur %d stations de bruit, %d sur %d pendulaires au hasard\n", ni, n, found, fpNamed, fpNoise, nNoise, fpCommute, nCommute);
  // La ligne de la dérive ouvre la fiche de la station
  ui::servicePane(1); run(300);
  ui::openStation(DRIFT); run(500);
  shot("16_service_fiche_derive");
  ui::sheetClose(); run(300);
  // Audit tactile de l'écran
  std::string repPath = s_out + "/audit_service.txt";
  FILE * rep = fopen(repPath.c_str(), "w");
  for (int p = 0; p < 4 && rep; p++) { ui::servicePane(p); run(300); char w[16]; snprintf(w, sizeof w, "service%d", p); audit(w, rep); }
  if (rep) { fprintf(rep, "\nBilan : %d OK, %d KO, %d petites cibles\n", s_auditOk, s_auditFail, s_auditSmall); fclose(rep); }
  printf("Audit tactile (service) : %d OK, %d KO, %d petites cibles\n", s_auditOk, s_auditFail, s_auditSmall);
  return (found == 2 && fpNamed == 0 && fpNoise == 0 && !s_auditFail) ? 0 : 2;
}

// ── Scénario « hors-ligne » : la carte n'a rien reçu (constat v1.0.8) ─────
// Heure connue (comme sur la photo), réseau enregistré mais association refusée,
// puis NTP absent, flux en échec, premier relevé, perte du Wi-Fi, flux en retard,
// horodatage de station invalide, retour à la normale. Chaque étape vérifie les
// TEXTES réellement affichés ; code de sortie ≠ 0 au premier écart.
// Labels visibles valant exactement « 0 », « 0% »… sous un objet (les axes des
// graphiques sont dessinés, pas des labels : ils ne comptent pas).
static int zeroLabels(lv_obj_t * o, std::string & which) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
  int n = 0;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char * t = lv_label_get_text(o);
    if (!strcmp(t, "0") || !strcmp(t, "0%") || !strcmp(t, "0 %") || !strcmp(t, "rempli \xC3\xA0 0 %") || !strcmp(t, "0 borne HS")) { n++; which += " \xC2\xAB"; which += t; which += "\xC2\xBB"; }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) n += zeroLabels(lv_obj_get_child(o, i), which);
  return n;
}
static lv_obj_t * labelWithPrefix(lv_obj_t * o, const char * prefix) {
  if (lv_obj_check_type(o, &lv_label_class) && strncmp(lv_label_get_text(o), prefix, strlen(prefix)) == 0) return o;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) { lv_obj_t * r = labelWithPrefix(lv_obj_get_child(o, i), prefix); if (r) return r; }
  return nullptr;
}
static int horsLigneScenario(const std::string & in) {
  s_namedShots = true;
  int fails = 0;
  auto expect = [&](bool c, const char * what, const char * got = nullptr) {
    printf("  %s %s%s%s%s\n", c ? "\xE2\x9C\x93" : "\xC3\x89" "CHEC", what, got ? " \xE2\x80\x94 lu : \xC2\xAB" : "", got ? got : "", got ? "\xC2\xBB" : "");
    if (!c) fails++;
  };
  auto D = [](int w) { return ui::debugDashText(w); };
  const char * DASH = "\xE2\x80\x94";
  auto info = readFile(in + "/station_information.json");
  auto status = readFile(in + "/station_status.json");
  auto types = readFile(in + "/vehicle_types.json");
  auto wx = readFile(in + "/weather.json");
  static StatusRec recs[VN_MAX_ST];
  static InfoRec irecs[VN_MAX_ST];
  ElecTypes et;
  gbfsParseTypes(types.data(), types.size(), et);
  int64_t lastUpd = 0;
  int bad = 0;
  int ns = gbfsParseStatus(status.data(), status.size(), recs, VN_MAX_ST, et, &lastUpd, &bad);
  int ni = gbfsParseInfo(info.data(), info.size(), irecs, VN_MAX_ST);

  sim::g_epoch0 = lastUpd ? lastUpd : 1791894641;   // heure connue, comme sur la photo de la carte
  sim::g_timeOk = true;
  settingsDefaults(g_set);
  strcpy(g_set.ssid, "BUREAU");
  sim::g_wifiState = net::W_CONNECTING; sim::g_stage = "Connexion Wi-Fi\xE2\x80\xA6";
  if (!g_model.init()) { fprintf(stderr, "init mod\xC3\xA8le\n"); return 1; }
  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 40);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);
  ui::init(disp);
  run(2000);

  // 1. Association refusée (mot de passe : code 15), aucun relevé — le cas de la photo
  printf("1. association en \xC3\xA9" "chec, aucun relev\xC3\xA9\n");
  sim::g_wifiReason = 15;
  run(18000);
  sim::g_wifiState = net::W_FAILED; sim::g_stage = "Wi-Fi injoignable";
  run(1500);
  ui::sheetClose(); run(400);
  ui::showTab(ui::TAB_DASH); run(6000);                // les toasts s'effacent
  shot("hl_01_aucune_donnee_wifi_echec");
  for (int k = 0; k < 4; k++) expect(!strcmp(D(k), DASH), "tuile : \xC2\xAB \xE2\x80\x94 \xC2\xBB, pas 0", D(k));
  expect(!strcmp(D(4), DASH), "jauge d'\xC3\xA9quilibre : \xC2\xAB \xE2\x80\x94 \xC2\xBB", D(4));
  expect(!strcmp(D(8), DASH), "compteurs d'\xC3\xA9quilibre : \xC2\xAB \xE2\x80\x94 \xC2\xBB", D(8));
  expect(!strcmp(D(5), DASH), "pr\xC3\xA9vision +1 h : \xC2\xAB \xE2\x80\x94 \xC2\xBB", D(5));
  expect(ui::debugDashForecastPoints() == 0, "aucun point de pr\xC3\xA9vision dessin\xC3\xA9 (pas de pointill\xC3\xA9s \xC3\xA0 0)");
  expect(!strcmp(D(6), "Aucune donn\xC3\xA9" "e"), "bandeau \xC2\xAB Aucune donn\xC3\xA9" "e \xC2\xBB", D(6));
  expect(strstr(D(7), "mot de passe refus\xC3\xA9") && strstr(D(7), "code 15"), "bandeau : cause de l'\xC3\xA9" "chec d'association", D(7));
  expect(!strcmp(D(9), "Aucune donn\xC3\xA9" "e"), "priorit\xC3\xA9s : pas de \xC2\xAB R\xC3\xA9seau \xC3\xA9quilibr\xC3\xA9 \xC2\xBB sans relev\xC3\xA9", D(9));
  { std::string w; int z = zeroLabels(lv_scr_act(), w); expect(z == 0, "aucun z\xC3\xA9ro affich\xC3\xA9 \xC3\xA0 l'\xC3\xA9" "cran", w.c_str()); }

  // 2. Associé, heure NTP absente
  printf("2. associ\xC3\xA9, NTP non synchronis\xC3\xA9\n");
  sim::g_wifiState = net::W_CONNECTED; sim::g_wifiReason = 0; sim::g_timeOk = false;
  run(1200);
  shot("hl_02_aucune_donnee_ntp");
  expect(!strcmp(D(6), "Aucune donn\xC3\xA9" "e") && strstr(D(7), "NTP"), "bandeau : NTP nomm\xC3\xA9", D(7));
  expect(!strcmp(D(0), DASH), "tuile toujours \xC2\xAB \xE2\x80\x94 \xC2\xBB", D(0));

  // 3. Associé, heure OK, station_status en échec (aucun flux reçu)
  printf("3. aucun flux re\xC3\xA7u (station_status en \xC3\xA9" "chec)\n");
  sim::g_timeOk = true;
  vn::ingestErr(sim::g_feed[vn::FEED_STATUS], plat::now(), 503, 0, 900);
  vn::ingestErr(sim::g_feed[vn::FEED_STATUS], plat::now(), 503, 0, 900);
  run(1200);
  shot("hl_03_aucun_flux");
  expect(strstr(D(7), "station_status") && strstr(D(7), "503"), "bandeau : flux et code d'\xC3\xA9" "chec", D(7));
  { std::string w; int z = zeroLabels(lv_scr_act(), w); expect(z == 0, "toujours aucun z\xC3\xA9ro", w.c_str()); }

  // 4. Premier relevé : en direct
  printf("4. relev\xC3\xA9 re\xC3\xA7u : en direct\n");
  int64_t t4 = plat::now();
  {
    plat::LockGuard g;
    g_model.applyInfo(irecs, ni, t4);
    synthHistory(g_model, t4, recs, ns);
    g_model.applyStatus(recs, ns, t4);
    wxParse(wx.data(), wx.size(), g_model.wx);
    g_model.wx.fetchedAt = t4;
    g_model.lastUpdate = t4;
    g_model.feedUpdated = lastUpd;
    g_model.recompute(t4);
    g_model.nEv = 0;
    g_model.version++;
  }
  vn::ingestOk(sim::g_feed[vn::FEED_STATUS], t4, (uint32_t)status.size(), 800);
  run(1500);
  shot("hl_04_en_direct");
  char exp[32]; ui::fmtInt(exp, sizeof(exp), g_model.tot.bikes);
  expect(!strcmp(D(0), exp), "v\xC3\xA9los dispo = total du flux", D(0));
  expect(!strcmp(D(6), ""), "aucun bandeau en direct", D(6));
  expect(strcmp(D(5), DASH) != 0 && ui::debugDashForecastPoints() == VN_FC_STEPS, "pr\xC3\xA9vision affich\xC3\xA9" "e", D(5));
  expect(ui::debugDashCurveLastX() == VN_RING_SLOTS - 1, "courbe jusqu'\xC3\xA0 maintenant");
  std::string k0 = D(0), k2 = D(2), k3 = D(3), arc = D(4);
  char hm[8]; ui::fmtClock(hm, sizeof(hm), t4);

  // 5. Wi-Fi perdu (borne introuvable), 10 min plus tard : dernières valeurs + « hors ligne depuis »
  printf("5. Wi-Fi perdu depuis 10 min\n");
  sim::g_wifiState = net::W_FAILED; sim::g_wifiReason = 201;
  sim::g_ms += 10 * 60 * 1000;
  run(1500);
  shot("hl_05_hors_ligne");
  expect(k0 == D(0) && k2 == D(2) && k3 == D(3), "tuiles : derni\xC3\xA8res valeurs gard\xC3\xA9" "es (ni 0 ni \xE2\x80\x94)", D(0));
  expect(arc == D(4), "jauge : derni\xC3\xA8re valeur gard\xC3\xA9" "e", D(4));
  std::string want = std::string("Hors ligne depuis ") + hm;
  expect(want == D(6), "bandeau \xC2\xAB Hors ligne depuis HH:MM \xC2\xBB (heure du dernier relev\xC3\xA9)", D(6));
  expect(strstr(D(7), "introuvable") != nullptr, "bandeau : cause (r\xC3\xA9seau introuvable)", D(7));
  expect(!strcmp(D(5), DASH) && !strcmp(D(11), "hors ligne") && ui::debugDashForecastPoints() == 0, "pr\xC3\xA9vision : \xC2\xAB \xE2\x80\x94 hors ligne \xC2\xBB, sans pointill\xC3\xA9s", D(5));
  expect(ui::debugDashCurveLastX() == VN_RING_SLOTS - 1 - 2, "courbe : dernier point \xC3\xA0 SON heure (2 tranches en arri\xC3\xA8re), pas \xC3\xA0 maintenant");
  // autre onglet : le bandeau global le dit aussi
  ui::showTab(ui::TAB_LIST); run(600);
  shot("hl_05b_hors_ligne_stations");
  expect(textVisible(lv_scr_act(), "depuis ") , "onglet Stations : \xC2\xAB Hors ligne \xC2\xB7 depuis HH:MM \xC2\xBB");
  ui::showTab(ui::TAB_DASH); run(600);

  // 6. Wi-Fi revenu mais flux en retard (20 min, seuil 3 min)
  printf("6. flux en retard\n");
  sim::g_wifiState = net::W_CONNECTED; sim::g_wifiReason = 0;
  sim::g_ms += 10 * 60 * 1000;
  run(1500);
  shot("hl_06_flux_en_retard");
  expect(strstr(D(6), "vieilles de 20 min") != nullptr, "bandeau \xC2\xAB Donn\xC3\xA9" "es vieilles de 20 min \xC2\xBB", D(6));
  expect(k0 == D(0), "tuiles : derni\xC3\xA8res valeurs", D(0));
  expect(!strcmp(D(5), DASH) && !strcmp(D(11), "en retard"), "pr\xC3\xA9vision : \xC2\xAB \xE2\x80\x94 en retard \xC2\xBB", D(11));

  // 7. Horodatage de station invalide (« 0001-01-01 » publié par la source)
  printf("7. horodatage de station invalide\n");
  expect(bad == 2, "instantan\xC3\xA9 : 2 horodatages invalides rep\xC3\xA9r\xC3\xA9s");
  // 148 et 149 publient l'an 1 mais n'ont pas de nom : écartées comme fantômes, jamais affichées.
  // Pour éprouver l'affichage, on injecte la même valeur sur la première station publique du flux.
  expect(g_model.find("148") < 0 && g_model.find("149") < 0, "148 et 149 (sans nom) : hors du mod\xC3\xA8le");
  std::string js(status.begin(), status.end());
  size_t q = js.find("\"last_reported\":\"");
  size_t q2 = q == std::string::npos ? q : js.find('"', q + 17);
  if (q2 != std::string::npos) js.replace(q + 17, q2 - (q + 17), "0001-01-01T00:00:00Z");
  static StatusRec r2[VN_MAX_ST];
  int bad2 = 0;
  int n2 = gbfsParseStatus(js.data(), js.size(), r2, VN_MAX_ST, et, &lastUpd, &bad2);
  int victim = -1;
  for (int k = 0; k < n2 && victim < 0; k++) if (r2[k].lastReported == 0) victim = g_model.find(r2[k].sid);
  { plat::LockGuard g; g_model.applyStatus(r2, n2, plat::now()); g_model.lastUpdate = plat::now(); g_model.version++; }
  run(600);
  expect(bad2 == 3 && victim >= 0 && g_model.st[victim].lastReported == 0, "station publique \xC3\xA0 l'an 1 : horodatage inconnu (0)");
  if (victim >= 0) {
    ui::openStation(victim); run(600);
    lv_obj_t * l = labelWithPrefix(lv_layer_top(), "Dernier rapport de la borne");
    if (l) { lv_obj_scroll_to_view_recursive(l, LV_ANIM_OFF); run(300); }
    shot("hl_07_station_horodatage_invalide");
    expect(l && strstr(lv_label_get_text(l), "inconnu"), "fiche : \xC2\xAB inconnu (horodatage invalide) \xC2\xBB", l ? lv_label_get_text(l) : "absent");
    ui::sheetClose(); run(400);
  }

  // 8. Retour à la normale : nouveau relevé
  printf("8. retour \xC3\xA0 la normale\n");
  { plat::LockGuard g; g_model.applyStatus(recs, ns, plat::now()); g_model.lastUpdate = plat::now(); g_model.version++; }
  run(1500);
  shot("hl_08_retour");
  expect(!strcmp(D(6), "") && strcmp(D(5), DASH) != 0, "bandeau effac\xC3\xA9, pr\xC3\xA9vision revenue", D(6));

  printf("Sc\xC3\xA9nario hors-ligne : %d \xC3\xA9" "cart(s)\n", fails);
  return fails ? 3 : 0;
}

// ── Scénario « priorités » : une alerte réseau dans les cinq lignes du tableau de bord ──
// Elle y entre dès que moins de cinq stations sont plus graves qu'elle — une journée calme, donc.
// La ligne lisait alors `g_model.st[-1]`, 408 octets AVANT le tableau (l'en-tête du tas PSRAM) :
// nom illisible et badge pris au hasard, sans plantage. ASan le voyait à chaque rafraîchissement.
// Ce scénario fige la règle : la ligne existe, elle est nommée « Réseau » et ne porte pas de badge.
static int prioScenario() {
  settingsDefaults(g_set);
  strcpy(g_set.ssid, "VELOH-OPS");
  const int64_t T = 1791842400 + 12 * 3600;              // 13/10/2026 12:00, heures actives
  if (!g_model.init()) { fprintf(stderr, "init modèle\n"); return 1; }
  static InfoRec ir[40];
  for (int i = 0; i < 40; i++) {
    memset(&ir[i], 0, sizeof ir[i]);
    snprintf(ir[i].sid, sizeof ir[i].sid, "%d", i + 1);
    snprintf(ir[i].name, sizeof ir[i].name, "Station %d", i + 1);
    ir[i].lat = 49.60f + i * 0.001f; ir[i].lon = 6.13f; ir[i].cap = 20;
  }
  g_model.applyInfo(ir, 40, T - 3600);
  static StatusRec sr[40];
  for (int i = 0; i < 40; i++) {
    memset(&sr[i], 0, sizeof sr[i]);
    strcpy(sr[i].sid, ir[i].sid);
    sr[i].bikes = 6; sr[i].elec = 6; sr[i].docks = 14;
    sr[i].installed = sr[i].renting = sr[i].returning = 1;
    sr[i].bikesDis = (i < 25) ? 2 : 0;                   // 50 vélos HS sur 290 présents ≈ 17 %
  }
  for (int i = 0; i < 4; i++) { sr[i].bikes = 0; sr[i].docks = 20; }   // quatre ruptures, pas plus
  sim::g_epoch0 = T; sim::g_timeOk = true;
  g_model.applyStatus(sr, 40, T - 1800);
  // Quelques stations bougent dans les vingt minutes : le flux n'est pas figé, donc l'alerte
  // réseau testée ici est celle du parc (part de vélos hors service), pas celle du flux.
  for (int i = 10; i < 15; i++) { sr[i].bikes = 7; sr[i].docks = 13; }
  g_model.applyStatus(sr, 40, T - 600);
  g_model.applyStatus(sr, 40, T);
  g_model.recompute(T);

  int fails = 0;
  auto check = [&](bool c, const char * what) { printf("  %s %s\n", c ? "✓" : "ÉCHEC", what); if (!c) fails++; };

  // Précondition : l'alerte réseau est bien dans les cinq premières lignes de priorité
  int netRank = -1, sevAbove = 0;
  for (int k = 0; k < g_model.nAlerts && netRank < 0; k++) {
    const Alert & a = g_model.alerts[k];
    if (a.sev < 1) continue;
    if (a.st < 0) netRank = sevAbove; else sevAbove++;
  }
  printf("Priorités : %d alerte(s) plus graves avant l'alerte réseau (rang %d)\n", sevAbove, netRank);
  check(netRank >= 0 && netRank < 5, "l'alerte réseau entre dans les cinq lignes de priorité");

  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 40);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);
  ui::init(disp);
  run(800);
  shot("prio_reseau");

  bool netNamed = false, netBadge = false, netWhy = false;
  for (int k = 0; k < 5; k++) {
    printf("  ligne %d : « %s » · « %s » · badge « %s »\n", k, ui::debugDashPrio(k, 0),
           ui::debugDashPrio(k, 1), ui::debugDashPrio(k, 2));
    if (strcmp(ui::debugDashPrio(k, 0), "R\xC3\xA9" "seau") == 0) {
      netNamed = true;
      netWhy = ui::debugDashPrio(k, 1)[0] != 0;
      netBadge = ui::debugDashPrio(k, 2)[0] != 0;
    }
  }
  check(netNamed, "la ligne de l'alerte réseau est nommée « Réseau » (pas un nom lu hors du tableau)");
  check(netWhy, "elle porte un motif lisible");
  check(!netBadge, "elle ne porte pas de besoin de livraison");
  printf("Scénario priorités : %d échec(s)\n", fails);
  return fails ? 3 : 0;
}

static int realMain(int argc, char ** argv) {
  if (argc < 3) { fprintf(stderr, "usage : sim <json_dir> <out_dir>\n"); return 1; }
  std::string in = argv[1];
  s_out = argv[2];
  mkdir(s_out.c_str(), 0755);              // dossier de sortie créé au besoin
  sim::g_fsDir = "/tmp/vn_sim_fs";
  if (system("rm -rf /tmp/vn_sim_fs")) {}
  bool ops = argc > 3 && strcmp(argv[3], "ops") == 0;
  if (argc > 3 && !ops) {
    if (strcmp(argv[3], "totaux") == 0) return totauxScenario(in);
    if (strcmp(argv[3], "service") == 0) return serviceScenario(in);
    if (strcmp(argv[3], "hors-ligne") == 0) return horsLigneScenario(in);
    if (strcmp(argv[3], "boot") == 0 || strcmp(argv[3], "boot-wifi-ko") == 0) return bootScenario(in, argv[3]);
    if (strcmp(argv[3], "priorites") == 0) return prioScenario();
    // un nom mal tapé ne doit pas « passer » en jouant le démarrage (code 0) : la CI le verrait vert
    fprintf(stderr, "sc\xC3\xA9nario inconnu : %s (ops, service, hors-ligne, totaux, boot, boot-wifi-ko, priorites)\n", argv[3]);
    return 64;
  }

  auto info = readFile(in + "/station_information.json");
  auto status = readFile(in + "/station_status.json");
  auto types = readFile(in + "/vehicle_types.json");
  auto wx = readFile(in + "/weather.json");

  static StatusRec recs[VN_MAX_ST];
  static InfoRec irecs[VN_MAX_ST];
  ElecTypes et;
  gbfsParseTypes(types.data(), types.size(), et);
  int64_t lastUpd = 0;
  int ns = gbfsParseStatus(status.data(), status.size(), recs, VN_MAX_ST, et, &lastUpd);
  int ni = gbfsParseInfo(info.data(), info.size(), irecs, VN_MAX_ST);
  printf("GBFS : %d stations publiques, %d statuts, last_updated=%lld, types élec=%d (%s)\n",
         ni, ns, (long long)lastUpd, et.n, et.n ? et.id[0] : "-");
  sim::g_epoch0 = lastUpd ? lastUpd : 1791894641;

  settingsDefaults(g_set);
  strcpy(g_set.ssid, "VELOH-OPS");
  if (!g_model.init()) { fprintf(stderr, "init modèle\n"); return 1; }

  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, s_buf, nullptr, 480 * 272);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480; dd.ver_res = 272; dd.flush_cb = flush; dd.draw_buf = &db;
  lv_disp_t * disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = readTouch;
  lv_indev_drv_register(&id);

  ui::init(disp);
  run(500);
  shot("accueil");

  // Données : info + historique synthétique + statut réel
  int64_t now = plat::now();
  g_model.applyInfo(irecs, ni, now);
  synthHistory(g_model, now, recs, ns);
  g_model.applyStatus(recs, ns, now);
  // Ruptures en cours : durée reconstituée depuis l'historique
  for (int i = 0; i < g_model.n; i++) {
    Station & s = g_model.st[i];
    if (s.status != ST_EMPTY && s.status != ST_FULL) continue;
    int back = 1;
    for (; back < g_model.count; back++) {
      int idx = g_model.ringIndex(back);
      uint8_t v = g_model.ring[(size_t)idx * VN_MAX_ST + i];
      bool same = s.status == ST_EMPTY ? v == 0 : (v != 255 && v >= s.capEff());
      if (!same) break;
    }
    int64_t since = now - (int64_t)back * VN_RING_STEP_S + 120;
    if (s.status == ST_EMPTY) s.emptySince = since; else s.fullSince = since;
  }
  g_model.wx.fetchedAt = now;
  wxParse(wx.data(), wx.size(), g_model.wx);
  g_model.lastUpdate = now;
  g_model.feedUpdated = lastUpd;
  g_model.fetchMs = 1240;
  g_model.recompute(now);
  g_model.nEv = 0;
  sim::g_timeSyncAt = now - 2 * 3600;          // heure « synchronisée » au démarrage simulé
  simFeeds(in, now);
  if (ops) return opsScenario(in, recs, ns);
  printf("Totaux : %d vélos dispo, %d bornes, %d HS, %d stations ouvertes, équilibre %.0f %%, alertes %d, transferts %d\n",
         g_model.tot.bikes, g_model.tot.docks, g_model.tot.bikesDis, g_model.tot.open, g_model.tot.balancePct,
         g_model.nAlerts, g_model.nTr);

  run(900);
  shot("reseau");
  drag(260, 230, 260, 60, 10);
  run(300);
  shot("reseau_activite");
  drag(260, 230, 260, 40, 10);
  run(300);
  shot("reseau_priorites");

  // Carte
  tap(30, tabY(ui::TAB_MAP));
  run(500);
  shot("carte");
  tap(460, 36 + 236 - 8 - 32 - 6 - 32 - 6 - 16);   // zoom +
  run(200);
  tap(460, 36 + 236 - 8 - 32 - 6 - 32 - 6 - 16);
  run(300);
  shot("carte_zoom");
  // Couche « +1 h »
  tap(60 + 8 + 170 - 28, 36 + 40 + 13);
  run(400);
  shot("carte_prevision_1h");
  tap(60 + 8 + 28, 36 + 40 + 13);              // retour Live
  run(300);
  tap(60 + 8 + 30 + 60, 36 + 8 + 13);          // filtre « Vides »
  run(300);
  shot("carte_filtre_vides");
  tap(60 + 8 + 20, 36 + 8 + 13);               // « Tout »
  run(200);

  // Fiche station : la station la plus critique
  int crit = g_model.nAlerts ? g_model.alerts[0].st : 0;
  ui::openStation(crit);
  run(500);
  shot("fiche_station");
  drag(300, 240, 300, 80, 10);
  run(300);
  shot("fiche_station_suite");
  ui::sheetClose();
  run(300);

  // Fiche d'une station équilibrée avec prévision intéressante
  int pick = -1;
  for (int i = 0; i < g_model.n; i++) if (g_model.st[i].active && g_model.st[i].etaEmpty > 15 && g_model.st[i].etaEmpty < 90) { pick = i; break; }
  if (pick >= 0) { ui::openStation(pick); run(500); shot("fiche_station_prevision"); ui::sheetClose(); run(300); }

  // Stations
  tap(30, tabY(ui::TAB_LIST));
  run(500);
  shot("stations");
  tap(60 + 8 + 3 * 75 + 30, 36 + 8 + 14);      // tri « Échéance »
  run(400);
  shot("stations_echeance");

  // Alertes
  tap(30, tabY(ui::TAB_ALERTS));
  run(500);
  shot("alertes");
  ui::openTransfers();
  run(500);
  shot("transferts");
  ui::sheetClose();
  run(300);

  // Réglages
  tap(30, tabY(ui::TAB_SETTINGS));
  run(500);
  shot("reglages");
  drag(260, 230, 260, 40, 10);
  drag(260, 230, 260, 40, 10);
  run(300);
  shot("reglages_suite");

  // Wi-Fi
  ui::openWifi();
  run(900);
  shot("wifi");
  tap(240, 36 + 14 + 70);                     // 1er réseau
  run(500);
  shot("wifi_clavier");
  ui::sheetClose();
  run(300);

  // Nouvelle rupture → toast
  tap(30, tabY(ui::TAB_DASH));
  run(400);
  for (int k = 0; k < ns; k++) {
    int i = g_model.find(recs[k].sid);
    if (i >= 0 && g_model.st[i].status == ST_OK && recs[k].bikes >= 1 && recs[k].bikes <= 4) {
      recs[k].docks += recs[k].bikes; recs[k].bikes = 0; recs[k].elec = 0; break;
    }
  }
  sim::g_fetching = true;
  run(300);
  g_model.applyStatus(recs, ns, plat::now());
  sim::g_fetching = false;
  run(600);
  shot("toast_rupture");
  run(1500);
  shot("toast_rupture_tard");
  run(4000);   // laisser le toast disparaître

  // ── Audit des zones tactiles, écran par écran ───────────────────────
  std::string repPath = s_out + "/audit_tactile.txt";
  FILE * rep = fopen(repPath.c_str(), "w");
  if (!rep) { fprintf(stderr, "impossible d'écrire %s\n", repPath.c_str()); return 1; }
  fprintf(rep, "Audit tactile — tap simulé au centre (ou dans un coin*) de chaque élément cliquable visible\n");
  fprintf(rep, "%-10s %-4s %-7s %-11s %s\n", "écran", "res.", "taille", "centre", "élément");
  static const char * TABS[ui::TAB_COUNT] = { "reseau", "carte", "stations", "alertes", "service", "reglages" };
  for (int t = 0; t < ui::TAB_COUNT; t++) {
    tap(30, tabY(t));
    run(500);
    audit(TABS[t], rep);
    run(100);
  }
  tap(30, tabY(ui::TAB_DASH));
  run(300);
  ui::openStation(g_model.alerts[0].st); run(500); audit("fiche", rep); ui::sheetClose(); run(300);
  ui::openTransfers(); run(500); audit("transferts", rep); ui::sheetClose(); run(300);
  ui::openProvenance(ui::PG_BIKES); run(500); audit("provenance", rep); ui::sheetClose(); run(300);
  ui::openIngest(); run(500); audit("ingestion", rep); ui::sheetClose(); run(300);
  ui::openForecast24(g_model.alerts[0].st); run(500); audit("prev24", rep); ui::sheetClose(); run(300);
  ui::openWifi(); run(900); audit("wifi", rep);
  tap(240, 36 + 14 + 70); run(500); audit("wifi-clav", rep); ui::sheetClose(); run(300);
  ui::openTouchTest(false); run(300); audit("tactile", rep);
  fprintf(rep, "\nBilan : %d éléments joignables, %d injoignables, %d petites cibles (< 24 px)\n", s_auditOk, s_auditFail, s_auditSmall);
  fclose(rep);
  printf("Audit tactile : %d OK, %d KO, %d petites cibles → %s\n", s_auditOk, s_auditFail, s_auditSmall, repPath.c_str());

  // ── Test du tactile : la croix suit le doigt ───────────────────────
  s_px = 330; s_py = 190; s_pressed = true; run(200);
  shot("tactile_test");
  s_pressed = false; run(300);

  // ── Étalonnage sur un panneau résistif simulé (axes échangés + miroir) ─
  s_phys = 1; s_resistive = true; g_set.tcal.valid = 0;
  static const int TX[5] = { 40, 440, 440, 40, 240 }, TY[5] = { 40, 40, 232, 232, 136 };
  // sans étalonnage : où atterrit un tap au centre ?
  { int rx, ry; int16_t x, y; physRaw(240, 136, rx, ry); hal::touchToScreen(rx, ry, x, y);
    printf("Résistif non étalonné : tap réel (240,136) → écran (%d,%d)\n", x, y); }
  ui::openTouchTest(true);
  run(400);
  shot("tactile_etalonnage");
  for (int i = 0; i < 5; i++) {
    s_px = TX[i] + (i % 2 ? 2 : -2); s_py = TY[i] + (i % 3 ? -1 : 2);   // doigt légèrement imprécis
    s_pressed = true; run(150);
    s_pressed = false; run(450);
  }
  run(300);
  shot("tactile_etalonne");
  int worst = 0;
  for (int y = 0; y < 272; y += 17) for (int x = 0; x < 480; x += 20) {
    int rx, ry; int16_t sx, sy;
    physRaw(x, y, rx, ry);
    hal::touchToScreen(rx, ry, sx, sy);
    int e = (int)lroundf(sqrtf((float)((sx - x) * (sx - x) + (sy - y) * (sy - y))));
    if (e > worst) worst = e;
  }
  printf("Étalonnage : valid=%d swap=%d x0=%d x1=%d y0=%d y1=%d → erreur max sur grille %d px\n",
         g_set.tcal.valid, g_set.tcal.swap, g_set.tcal.x0, g_set.tcal.x1, g_set.tcal.y0, g_set.tcal.y1, worst);
  // après étalonnage, un tap réel sur l'onglet « Alertes » ouvre bien Alertes
  {                // tap réel sur le bouton « Terminer » (position lue dans l'arbre d'objets)
    lv_obj_t * btn = findClickableWithText(lv_layer_sys(), "Terminer");
    lv_area_t a; lv_obj_get_coords(btn, &a);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    printf("Bouton « Terminer » à (%d,%d) → écran de test %s\n", (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2,
           ui::touchTestOpen() ? "toujours ouvert (INCORRECT)" : "fermé ✓");
  }
  run(300);
  tap(30, tabY(ui::TAB_ALERTS));
  run(500);
  printf("Tap réel sur « Alertes » après étalonnage → onglet %s\n", ui::debugTab() == ui::TAB_ALERTS ? "Alertes ✓" : "INCORRECT");
  return (s_auditFail || worst > 4 || ui::debugTab() != ui::TAB_ALERTS) ? 2 : 0;
}

// ── Exécution sur une pile peinte : mesure de la profondeur de pile maximale de l'UI ──
static int s_argc, s_ret;
static char ** s_argv;
static uint8_t * s_stk;
static uintptr_t s_top;
static void * uiThread(void *) {
  volatile uint8_t mark = 0;
  s_top = (uintptr_t)&mark;
  s_ret = realMain(s_argc, s_argv);
  return nullptr;
}
int main(int argc, char ** argv) {
  s_argc = argc; s_argv = argv;
  const size_t SZ = 4u << 20;
  s_stk = (uint8_t *)aligned_alloc(4096, SZ);
  memset(s_stk, 0xA5, SZ);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstack(&at, s_stk, SZ);
  pthread_t t;
  if (pthread_create(&t, &at, uiThread, nullptr)) { perror("pthread"); return 1; }
  pthread_join(t, nullptr);
  size_t i = 0;
  while (i < SZ && s_stk[i] == 0xA5) i++;
  printf("Pile de la boucle UI : %lu octets au maximum (x86-64)\n", (unsigned long)(s_top - (uintptr_t)(s_stk + i)));
  return s_ret;
}
