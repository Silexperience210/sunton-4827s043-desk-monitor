// ── Écran « Carte » : le réseau, maintenant ou à l'horizon de prévision ─
#include "ui_internal.h"
#include "widgets/vn_map.h"
#include <stdio.h>

using vn::g_model;

namespace ui {

static lv_obj_t * s_map, * s_chips[4], * s_seg, * s_legendNote;
static MapFilter s_filter = MF_ALL;
static int s_layer = 0;
static const int LAYERS[3] = { 0, 6, 12 };

static void updateCounts() {
  int c[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < g_model.n; i++) {
    const vn::Station & s = g_model.st[i];
    if (!s.active || !s.live) continue;
    uint8_t sts = statusAt(i, s_layer);
    c[0]++;
    if (filterMatch(sts, MF_EMPTY)) c[1]++;
    if (filterMatch(sts, MF_FULL)) c[2]++;
    if (filterMatch(sts, MF_TENSION)) c[3]++;
  }
  for (int k = 0; k < 4; k++) chipSet(s_chips[k], k == (int)s_filter, c[k]);
  if (s_layer) lv_label_set_text_fmt(s_legendNote, "PR\xC3\x89VISION +%d MIN", s_layer * 5);
  else lv_label_set_text(s_legendNote, "TEMPS R\xC3\x89" "EL");
  lv_obj_set_style_text_color(s_legendNote, C(s_layer ? HX_ACCENT : HX_GOOD), 0);
}

static void onTap(int st) { openStation(st); }

static void chipEvent(lv_event_t * e) {
  s_filter = (MapFilter)(intptr_t)lv_event_get_user_data(e);
  mapSetFilter(s_map, s_filter);
  updateCounts();
}

static void build(lv_obj_t * root) {
  s_map = mapCreate(root, CONTENT_W, CONTENT_H, onTap);
  // Filtres
  lv_obj_t * chips = row(root, 5);
  lv_obj_set_pos(chips, 8, 8);
  static const char * L[4] = { "Tout", "Vides", "Pleines", "Tension" };
  for (int k = 0; k < 4; k++) {
    s_chips[k] = chip(chips, L[k]);
    lv_obj_add_event_cb(s_chips[k], chipEvent, LV_EVENT_CLICKED, (void *)(intptr_t)k);
  }
  // Horizon : maintenant / +30 min / +1 h
  static const char * H[3] = { "Live", "+30 min", "+1 h" };
  s_seg = segmented(root, H, 3, 0, [](int i) {
    s_layer = LAYERS[i];
    mapSetLayer(s_map, s_layer);
    updateCounts();
  }, 30);
  lv_obj_set_width(s_seg, 176);
  lv_obj_set_pos(s_seg, 8, 41);
  segSet(s_seg, 0, false);

  // Légende (vn-legend)
  lv_obj_t * lg = row(root, 8);
  lv_obj_set_style_pad_hor(lg, 8, 0);
  lv_obj_set_height(lg, 22);
  lv_obj_set_style_radius(lg, 8, 0);
  lv_obj_set_style_bg_color(lg, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(lg, 230, 0);
  lv_obj_set_style_border_color(lg, lv_color_white(), 0);
  lv_obj_set_style_border_opa(lg, OPA_BORDER, 0);
  lv_obj_set_style_border_width(lg, 1, 0);
  lv_obj_align(lg, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  static const uint8_t ST[5] = { vn::ST_EMPTY, vn::ST_LOW, vn::ST_OK, vn::ST_HIGH, vn::ST_FULL };
  static const char * N[5] = { "VIDE", "FAIBLE", "\xC3\x89QUIL.", "CHARG\xC3\x89" "E", "PLEINE" };
  for (int k = 0; k < 5; k++) {
    lv_obj_t * it = row(lg, 4);
    dot(it, 7, statusColor(ST[k]));
    label(it, &vn_mono_10, HX_TEXT2, N[k]);
  }
  lv_obj_t * tr = row(lg, 4);
  lv_obj_t * line = box(tr);
  lv_obj_set_size(line, 12, 3);
  lv_obj_set_style_radius(line, 2, 0);
  lv_obj_set_style_bg_color(line, C(HX_TRANSIT), 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  label(tr, &vn_mono_10, HX_TEXT2, "T1");

  s_legendNote = eyebrow(root, "TEMPS R\xC3\x89" "EL");
  lv_obj_align(s_legendNote, LV_ALIGN_BOTTOM_LEFT, 12, -34);

  // Zoom
  lv_obj_t * zc = col(root, 6);
  lv_obj_align(zc, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_t * zp = iconButton(zc, &ic_plus_16, true, 32);
  lv_obj_t * zm = iconButton(zc, &ic_minus_16, true, 32);
  lv_obj_t * zf = iconButton(zc, &ic_target_16, true, 32);
  lv_obj_add_event_cb(zp, [](lv_event_t *) { mapZoom(s_map, 1.6f); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(zm, [](lv_event_t *) { mapZoom(s_map, 1 / 1.6f); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(zf, [](lv_event_t *) {
    static bool whole = true;
    mapFit(s_map, whole);
    toast(whole ? "Vue r\xC3\xA9seau complet" : "Vue centre-ville", whole ? "Toutes les stations Vel'OH!" : "Luxembourg-Ville",
          T_NEUTRAL, &ic_target_16);
    whole = !whole;
  }, LV_EVENT_CLICKED, nullptr);
}

static void refresh() { updateCounts(); mapRefresh(s_map); }

void mapScreenFocus(int st) {
  if (!s_map || st < 0 || st >= g_model.n) return;
  mapCenter(s_map, g_model.st[st].lat, g_model.st[st].lon, 16000.f);
  mapSelect(s_map, st);
}

const ScreenDef SCR_MAP = { "Carte", "CARTE DU R\xC3\x89SEAU", &ic_map_20, build, refresh, nullptr };

} // namespace ui
