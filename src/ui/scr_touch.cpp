// ── Test et étalonnage du tactile (plein écran, au-dessus de tout) ────
// Mode test : une croix suit le doigt et affiche les coordonnées écran / brutes ;
//             si elle n'apparaît pas sous le doigt, lancer l'étalonnage.
// Étalonnage : 4 cibles dans les coins + 1 au centre pour contrôler la précision.
// Accès : Réglages → Système → Tactile, ou appui continu de 6 s n'importe où ;
// sur l'écran de test, un appui de 3 s lance l'étalonnage (utile si les boutons
// sont inatteignables parce que le tactile est décalé).
#include "ui_internal.h"
#include "../hal/touch_api.h"
#include <stdio.h>
#include <string.h>

namespace ui {

static const int16_t TX[5] = { 40, 440, 440, 40, 240 };
static const int16_t TY[5] = { 40, 40, 232, 232, 136 };

struct TT {
  lv_obj_t * root = nullptr, * hl, * vl, * ring, * coord, * title, * msg, * btns, * target, * info, * marks[4];
  int mode = 0;                     // 0 test, 1 étalonnage
  int step = 0;
  int16_t rx[5], ry[5];
  long accX = 0, accY = 0;
  int accN = 0;
  bool wasPressed = false;
  uint32_t lastSeq = 0, releasedAt = 0, pressStart = 0;
  lv_timer_t * timer = nullptr;
};
static TT T;

static void setMsg(const char * title, const char * msg, uint32_t col = HX_TEXT2) {
  lv_label_set_text(T.title, title);
  lv_label_set_text(T.msg, msg);
  lv_obj_set_style_text_color(T.msg, C(col), 0);
}

static void placeTarget(int i) {
  lv_obj_set_pos(T.target, TX[i] - 20, TY[i] - 20);
  lv_obj_clear_flag(T.target, LV_OBJ_FLAG_HIDDEN);
  char t[48];
  snprintf(t, sizeof(t), "\xC3\x89talonnage %d/5", i + 1);
  setMsg(t, i < 4 ? "Touchez pr\xC3\xA9" "cis\xC3\xA9ment le centre de la cible, puis rel\xC3\xA2" "chez"
                  : "Derni\xC3\xA8re cible : contr\xC3\xB4le de pr\xC3\xA9" "cision au centre", HX_TEXT2);
}

static void refreshInfo() {
  char info[80];
  snprintf(info, sizeof(info), "%s \xC2\xB7 %s", hal::touchName(),
           vn::g_set.tcal.valid ? "\xC3\xA9talonnage enregistr\xC3\xA9" : "r\xC3\xA9glage d'usine");
  lv_label_set_text(T.info, info);
}

static void setMode(int mode) {
  refreshInfo();
  T.mode = mode;
  T.step = 0;
  T.accN = 0; T.accX = T.accY = 0;
  T.wasPressed = false;
  T.releasedAt = lv_tick_get();
  if (mode == 1) {
    lv_obj_add_flag(T.btns, LV_OBJ_FLAG_HIDDEN);
    for (auto * m : T.marks) lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(T.hl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(T.vl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(T.ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(T.coord, LV_OBJ_FLAG_HIDDEN);
    placeTarget(0);
  } else {
    lv_obj_clear_flag(T.btns, LV_OBJ_FLAG_HIDDEN);
    for (auto * m : T.marks) lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(T.target, LV_OBJ_FLAG_HIDDEN);
  }
}

static void close() {
  if (T.timer) { lv_timer_del(T.timer); T.timer = nullptr; }
  if (T.root) { lv_obj_del_async(T.root); T.root = nullptr; }
}

static void finishCalibration() {
  vn::TouchCal cal;
  if (!hal::touchSolveCal(T.rx, T.ry, TX, TY, cal)) {
    setMode(1);
    setMsg("Mesure incoh\xC3\xA9rente", "Les cibles n'ont pas \xC3\xA9t\xC3\xA9 touch\xC3\xA9" "es correctement \xE2\x80\x94 on recommence", HX_WARN);
    return;
  }
  int err = hal::touchCalError(cal, T.rx[4], T.ry[4], TX[4], TY[4]);
  if (err > 20) {
    char m[96];
    snprintf(m, sizeof(m), "\xC3\x89" "cart de %d px au centre \xE2\x80\x94 on recommence", err);
    setMode(1);
    setMsg("Pr\xC3\xA9" "cision insuffisante", m, HX_WARN);
    return;
  }
  vn::g_set.tcal = cal;
  vn::settingsSave();
  setMode(0);
  char m[96];
  snprintf(m, sizeof(m), "Pr\xC3\xA9" "cision %d px \xC2\xB7 axes %s \xC2\xB7 enregistr\xC3\xA9", err, cal.swap ? "\xC3\xA9" "chang\xC3\xA9s" : "directs");
  setMsg("\xC3\x89talonnage termin\xC3\xA9", m, HX_GOOD);
}

static void poll(lv_timer_t *) {
  if (!T.root) return;
  hal::TouchSample s = hal::touchLast();
  uint32_t now = lv_tick_get();
  if (T.mode == 0) {
    if (s.pressed) {
      if (!T.wasPressed) T.pressStart = now;
      lv_obj_clear_flag(T.hl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(T.vl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(T.ring, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(T.coord, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_y(T.hl, s.y);
      lv_obj_set_x(T.vl, s.x);
      lv_obj_set_pos(T.ring, s.x - 14, s.y - 14);
      lv_label_set_text_fmt(T.coord, "x %d \xC2\xB7 y %d  (brut %d, %d)", s.x, s.y, s.rx, s.ry);
      lv_obj_set_pos(T.coord, s.x > 300 ? s.x - 170 : s.x + 18, s.y > 230 ? s.y - 26 : s.y + 12);
      if (now - T.pressStart > 3000) {           // appui long : étalonnage
        T.wasPressed = false;
        setMode(1);
        return;
      }
    }
    T.wasPressed = s.pressed;
    return;
  }
  // Étalonnage : moyenne des lectures pendant l'appui, validation au relâchement
  if (now - T.releasedAt < 300) { T.wasPressed = s.pressed && T.wasPressed; return; }
  if (s.pressed) {
    if (s.seq != T.lastSeq) { T.accX += s.rx; T.accY += s.ry; T.accN++; T.lastSeq = s.seq; }
    T.wasPressed = true;
    lv_obj_set_style_border_color(T.target, C(HX_GOOD), 0);
  } else if (T.wasPressed) {
    T.wasPressed = false;
    T.releasedAt = now;
    lv_obj_set_style_border_color(T.target, C(HX_ACCENT), 0);
    if (T.accN >= 2) {
      T.rx[T.step] = (int16_t)(T.accX / T.accN);
      T.ry[T.step] = (int16_t)(T.accY / T.accN);
      T.step++;
      if (T.step >= 5) { lv_obj_add_flag(T.target, LV_OBJ_FLAG_HIDDEN); finishCalibration(); }
      else placeTarget(T.step);
    }
    T.accN = 0; T.accX = T.accY = 0;
  }
}

static lv_obj_t * line(lv_obj_t * p, lv_coord_t w, lv_coord_t h) {
  lv_obj_t * o = box(p);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, C(HX_ACCENT), 0);
  lv_obj_set_style_bg_opa(o, 150, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return o;
}

void openTouchTest(bool calibrate) {
  if (T.root) { if (calibrate) setMode(1); return; }
  T = TT();
  T.root = box(lv_layer_sys());
  lv_obj_set_size(T.root, 480, 272);
  lv_obj_set_style_bg_color(T.root, C(HX_BG), 0);
  lv_obj_set_style_bg_opa(T.root, LV_OPA_COVER, 0);
  lv_obj_add_flag(T.root, LV_OBJ_FLAG_CLICKABLE);
  // repères de coin (cibles du test)
  for (int i = 0; i < 4; i++) {
    lv_obj_t * m = box(T.root);
    lv_obj_set_size(m, 16, 16);
    lv_obj_set_pos(m, TX[i] - 8, TY[i] - 8);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(m, C(HX_TEXT3), 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_t * d = dot(m, 4, C(HX_TEXT3));
    lv_obj_center(d);
    T.marks[i] = m;
  }
  T.title = label(T.root, &vn_inter_15b, HX_TEXT, "Test du tactile");
  lv_obj_align(T.title, LV_ALIGN_TOP_MID, 0, 74);
  T.msg = label(T.root, &vn_inter_12, HX_TEXT2, "");
  lv_obj_set_width(T.msg, 340);
  lv_obj_set_style_text_align(T.msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(T.msg, LV_LABEL_LONG_WRAP);
  lv_obj_align(T.msg, LV_ALIGN_TOP_MID, 0, 98);
  T.info = eyebrow(T.root, "");
  lv_obj_align(T.info, LV_ALIGN_TOP_MID, 0, 56);

  T.hl = line(T.root, 480, 1);
  T.vl = line(T.root, 1, 272);
  T.ring = box(T.root);
  lv_obj_set_size(T.ring, 28, 28);
  lv_obj_set_style_radius(T.ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(T.ring, C(HX_ACCENT), 0);
  lv_obj_set_style_border_width(T.ring, 2, 0);
  lv_obj_add_flag(T.ring, LV_OBJ_FLAG_HIDDEN);
  T.coord = label(T.root, &vn_mono_10, HX_ACCENT, "");
  lv_obj_add_flag(T.coord, LV_OBJ_FLAG_HIDDEN);

  // cible d'étalonnage : anneau + croix
  T.target = box(T.root);
  lv_obj_set_size(T.target, 40, 40);
  lv_obj_set_style_radius(T.target, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(T.target, C(HX_ACCENT), 0);
  lv_obj_set_style_border_width(T.target, 2, 0);
  lv_obj_set_style_shadow_color(T.target, C(HX_ACCENT), 0);
  lv_obj_set_style_shadow_width(T.target, 16, 0);
  lv_obj_set_style_shadow_opa(T.target, 90, 0);
  lv_obj_t * th = box(T.target);
  lv_obj_set_size(th, 24, 2);
  lv_obj_set_style_bg_color(th, C(HX_TEXT), 0);
  lv_obj_set_style_bg_opa(th, LV_OPA_COVER, 0);
  lv_obj_center(th);
  lv_obj_t * tv = box(T.target);
  lv_obj_set_size(tv, 2, 24);
  lv_obj_set_style_bg_color(tv, C(HX_TEXT), 0);
  lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
  lv_obj_center(tv);
  lv_obj_add_flag(T.target, LV_OBJ_FLAG_HIDDEN);

  T.btns = row(T.root, 8);
  lv_obj_align(T.btns, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_t * bc = button(T.btns, "\xC3\x89talonner", &ic_target_16, B_TONAL, true);
  lv_obj_t * bf = button(T.btns, "Usine", nullptr, B_GHOST, true);
  lv_obj_t * bd = button(T.btns, "Terminer", &ic_check_16, B_PRIMARY, true);
  lv_obj_add_event_cb(bc, [](lv_event_t *) { setMode(1); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(bf, [](lv_event_t *) {
    vn::g_set.tcal.valid = 0;
    vn::settingsSave();
    refreshInfo();
    setMsg("Test du tactile", "R\xC3\xA9glage d'usine r\xC3\xA9tabli \xE2\x80\x94 v\xC3\xA9rifiez que la croix suit le doigt", HX_TEXT2);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(bd, [](lv_event_t *) { close(); }, LV_EVENT_CLICKED, nullptr);

  setMode(calibrate ? 1 : 0);
  if (!calibrate)
    setMsg("Test du tactile", "Touchez l'\xC3\xA9" "cran : la croix doit appara\xC3\xAE" "tre sous votre doigt. "
                              "Sinon, \xC3\xA9talonnez (ou maintenez 3 s).", HX_TEXT2);
  T.timer = lv_timer_create(poll, 25, nullptr);
}

bool touchTestOpen() { return T.root != nullptr; }

} // namespace ui
