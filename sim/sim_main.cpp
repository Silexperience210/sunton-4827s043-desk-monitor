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
#include <string>
#include <time.h>

namespace ui { lv_obj_t * debugToast(); int debugTab(); }
namespace sim { extern int64_t g_epoch0; extern uint32_t g_ms; extern bool g_fetching; extern const char * g_fsDir;
                extern int g_wifiState; extern bool g_timeOk; extern const char * g_stage;
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
    for (int t = 0; t < 5; t++) { ui::sheetClose(); run(300); tap(30, 36 + t * 47 + 20); run(600); char n[32]; snprintf(n, sizeof n, "boot_vide_onglet%d", t); shot(n); }
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
  for (int t = 0; t < 5; t++) { tap(30, 36 + t * 47 + 20); run(600); char n[32]; snprintf(n, sizeof n, "boot_onglet%d", t); shot(n); }
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
  tap(30, 36 + 4 * 47 + 20);
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
  tap(30, 36 + 3 * 47 + 20);
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
  printf("Scénario ops : %d échec(s)\n", fails);
  return fails ? 4 : 0;
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
    return bootScenario(in, argv[3]);
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
  tap(30, 36 + 47 + 20);
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
  tap(30, 36 + 2 * 47 + 20);
  run(500);
  shot("stations");
  tap(60 + 8 + 3 * 75 + 30, 36 + 8 + 14);      // tri « Échéance »
  run(400);
  shot("stations_echeance");

  // Alertes
  tap(30, 36 + 3 * 47 + 20);
  run(500);
  shot("alertes");
  ui::openTransfers();
  run(500);
  shot("transferts");
  ui::sheetClose();
  run(300);

  // Réglages
  tap(30, 36 + 4 * 47 + 20);
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
  tap(30, 36 + 20);
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
  static const char * TABS[5] = { "reseau", "carte", "stations", "alertes", "reglages" };
  for (int t = 0; t < 5; t++) {
    tap(30, 36 + t * 47 + 20);
    run(500);
    audit(TABS[t], rep);
    run(100);
  }
  tap(30, 36 + 20);
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
  tap(30, 36 + 3 * 47 + 20);
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
