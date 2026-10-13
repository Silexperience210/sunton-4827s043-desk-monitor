// ── Écran « Alertes » : ruptures, prévisions, maintenance, régulation ─
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

using vn::g_model;

namespace ui {

void alertWhy(const vn::Alert & a, char * out, size_t n) {
  char d[24];
  const vn::Station & s = g_model.st[a.st];
  switch (a.type) {
    case vn::AL_EMPTY:
      if (a.minutes >= 0) { fmtDur(d, sizeof(d), a.minutes); snprintf(out, n, "Vide depuis %s", d); }
      else snprintf(out, n, "Plus aucun v\xC3\xA9lo");
      break;
    case vn::AL_FULL:
      if (a.minutes >= 0) { fmtDur(d, sizeof(d), a.minutes); snprintf(out, n, "Pleine depuis %s", d); }
      else snprintf(out, n, "Plus aucune borne libre");
      break;
    case vn::AL_SOON_EMPTY:
      fmtDur(d, sizeof(d), a.minutes);
      snprintf(out, n, "Rupture dans ~%s \xC2\xB7 %d v\xC3\xA9lo%s", d, a.value, a.value > 1 ? "s" : "");
      break;
    case vn::AL_SOON_FULL:
      fmtDur(d, sizeof(d), a.minutes);
      snprintf(out, n, "Saturation dans ~%s \xC2\xB7 %d borne%s", d, a.value, a.value > 1 ? "s" : "");
      break;
    case vn::AL_MAINT_BIKES: snprintf(out, n, "%d v\xC3\xA9los hors service", a.value); break;
    case vn::AL_MAINT_DOCKS: snprintf(out, n, "%d bornes hors service", a.value); break;
    case vn::AL_FROZEN:
      fmtDur(d, sizeof(d), a.minutes);
      snprintf(out, n, "Aucun mouvement depuis %s \xE2\x80\x94 v\xC3\xA9rifier la station", d);
      break;
    case vn::AL_UNUSUAL:      // a.value = écart signé au moment du calcul ; attendu et tolérance : la station
      snprintf(out, n, "Inhabituel : %d v\xC3\xA9lo%s contre %d attendu%s (\xC2\xB1 %d)",
               s.bikes, s.bikes > 1 ? "s" : "", s.anomExp, s.anomExp > 1 ? "s" : "", s.anomTol);
      break;
    case vn::AL_CLOSED:
      snprintf(out, n, "%s", a.value == 1 ? "Location suspendue" : a.value == 2 ? "Retours suspendus"
                                          : (s.installed ? "Station hors service" : "Station non install\xC3\xA9" "e"));
      break;
    default: out[0] = 0;
  }
}

const lv_img_dsc_t * alertIcon16(uint8_t t) {
  switch (t) {
    case vn::AL_EMPTY: return &ic_alert_16;
    case vn::AL_FULL: return &ic_dock_16;
    case vn::AL_SOON_EMPTY: return &ic_trend_down_16;
    case vn::AL_SOON_FULL: return &ic_trend_up_16;
    case vn::AL_MAINT_BIKES: case vn::AL_MAINT_DOCKS: return &ic_wrench_16;
    case vn::AL_FROZEN: return &ic_clock_16;
    case vn::AL_UNUSUAL: return &ic_activity_16;
    default: return &ic_ban_16;
  }
}

Tone alertTone(const vn::Alert & a) {
  switch (a.type) {
    case vn::AL_EMPTY: return T_BAD;
    case vn::AL_FULL: return T_TRANSIT;
    case vn::AL_SOON_EMPTY: return T_WARN;
    case vn::AL_SOON_FULL: return T_ELEC;
    case vn::AL_MAINT_BIKES: case vn::AL_MAINT_DOCKS: case vn::AL_FROZEN: return T_WARN;
    case vn::AL_UNUSUAL: return T_ELEC;
    default: return T_CLOSED;
  }
}

void needText(int need, char * out, size_t n) {
  if (need > 0) snprintf(out, n, "+%d v\xC3\xA9los", need);
  else snprintf(out, n, "\xE2\x88\x92%d v\xC3\xA9los", -need);
}

// ─────────────────────────────────────────────────────────────────────
struct ARow { lv_obj_t * obj, * flash, * ic, * name, * why, * need; };
static ARow s_rows[VN_MAX_ALERTS];
static int s_nRows = 0;
static lv_obj_t * s_list, * s_seg, * s_planSub, * s_empty;
static int s_filter = 0;
static const int NF = 5;
static const char * FILTERS[NF] = { "Toutes", "Ruptures", "Pr\xC3\xA9visions", "Maint.", "Inhabituel" };

static bool match(const vn::Alert & a) {
  switch (s_filter) {
    case 1: return a.type == vn::AL_EMPTY || a.type == vn::AL_FULL;
    case 2: return a.type == vn::AL_SOON_EMPTY || a.type == vn::AL_SOON_FULL;
    case 3: return a.type >= vn::AL_MAINT_BIKES && a.type <= vn::AL_CLOSED;
    case 4: return a.type == vn::AL_UNUSUAL;
    default: return true;
  }
}

static void rowClick(lv_event_t * e) {
  int st = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (st >= 0) openStation(st);
}

static ARow & makeRow(int i) {
  ARow & r = s_rows[i];
  r.obj = row(s_list, 10);
  lv_obj_set_size(r.obj, LV_PCT(100), 44);
  lv_obj_set_style_pad_hor(r.obj, 10, 0);
  lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r.obj, C(HX_S2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_STATE_PRESSED);
  if (i > 0) {
    lv_obj_set_style_border_side(r.obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(r.obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(r.obj, OPA_BORDER, 0);
    lv_obj_set_style_border_width(r.obj, 1, 0);
  }
  lv_obj_add_event_cb(r.obj, rowClick, LV_EVENT_CLICKED, nullptr);
  r.flash = box(r.obj);
  lv_obj_set_size(r.flash, 28, 28);
  lv_obj_set_style_radius(r.flash, 8, 0);
  lv_obj_set_style_bg_opa(r.flash, 36, 0);
  r.ic = lv_img_create(r.flash);
  lv_obj_center(r.ic);
  lv_obj_t * c = col(r.obj, 1);
  lv_obj_set_flex_grow(c, 1);
  r.name = label(c, &vn_inter_13s, HX_TEXT, "");
  oneLine(r.name, 250);
  r.why = label(c, &vn_inter_12, HX_TEXT2, "");
  oneLine(r.why, 250);
  r.need = badge(r.obj, "", T_GOOD);
  return r;
}

static void refresh() {
  int cnt[NF] = { 0, 0, 0, 0, 0 };
  for (int k = 0; k < g_model.nAlerts; k++) {
    const vn::Alert & a = g_model.alerts[k];
    cnt[0]++;
    if (a.type == vn::AL_EMPTY || a.type == vn::AL_FULL) cnt[1]++;
    else if (a.type == vn::AL_SOON_EMPTY || a.type == vn::AL_SOON_FULL) cnt[2]++;
    else if (a.type == vn::AL_UNUSUAL) cnt[4]++;
    else cnt[3]++;
  }
  char b[96];
  for (int k = 0; k < NF; k++) { snprintf(b, sizeof(b), "%s %d", FILTERS[k], cnt[k]); segLabel(s_seg, k, b); }

  int shown = 0;
  for (int k = 0; k < g_model.nAlerts; k++) {
    const vn::Alert & a = g_model.alerts[k];
    if (!match(a)) continue;
    if (shown >= s_nRows) makeRow(s_nRows++);
    ARow & r = s_rows[shown++];
    lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(r.obj, (void *)(intptr_t)a.st);
    Tone t = alertTone(a);
    lv_color_t tc = toneColor(t);
    lv_obj_set_style_bg_color(r.flash, t == T_CLOSED ? C(HX_CLOSED) : tc, 0);
    lv_img_set_src(r.ic, alertIcon16(a.type));
    iconColor(r.ic, tc);
    const vn::Station & s = g_model.st[a.st];
    snprintf(b, sizeof(b), "%s", s.name);
    lv_label_set_text(r.name, b);
    alertWhy(a, b, sizeof(b));
    lv_label_set_text(r.why, b);
    bool showNeed = s.need && (a.type <= vn::AL_SOON_FULL);
    if (showNeed) {
      char nd[24];
      needText(s.need, nd, sizeof(nd));
      badgeSet(r.need, nd, s.need > 0 ? T_GOOD : T_TRANSIT);
      lv_obj_clear_flag(r.need, LV_OBJ_FLAG_HIDDEN);
    } else if (a.sev >= 3) {
      badgeSet(r.need, "URGENT", T_BAD);
      lv_obj_clear_flag(r.need, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(r.need, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = shown; i < s_nRows; i++) lv_obj_add_flag(s_rows[i].obj, LV_OBJ_FLAG_HIDDEN);
  if (shown) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

  char n1[16], n2[16];
  fmtInt(n1, sizeof(n1), g_model.tot.need);
  fmtInt(n2, sizeof(n2), g_model.tot.surplus);
  if (!g_model.lastUpdate) snprintf(b, sizeof(b), "En attente des premi\xC3\xA8res donn\xC3\xA9" "es");
  else if (!g_model.tot.need && !g_model.tot.surplus) snprintf(b, sizeof(b), "R\xC3\xA9seau \xC3\xA9quilibr\xC3\xA9 \xC2\xB7 aucune action");
  else snprintf(b, sizeof(b), "+%s \xC3\xA0 livrer \xC2\xB7 \xE2\x88\x92%s \xC3\xA0 retirer \xC2\xB7 %d transfert%s",
                n1, n2, g_model.nTr, g_model.nTr > 1 ? "s" : "");
  lv_label_set_text(s_planSub, b);
}

static void build(lv_obj_t * root) {
  // Plan de régulation
  lv_obj_t * plan = card(root, 8);
  lv_obj_set_pos(plan, 8, 8);
  lv_obj_set_size(plan, CONTENT_W - 16, 48);
  lv_obj_set_flex_flow(plan, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(plan, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(plan, 10, 0);
  lv_obj_set_style_bg_color(plan, C(0x1A120A), 0);
  lv_obj_set_style_border_color(plan, C(HX_ACCENT), 0);
  lv_obj_set_style_border_opa(plan, 80, 0);
  lv_obj_t * fl = box(plan);
  lv_obj_set_size(fl, 30, 30);
  lv_obj_set_style_radius(fl, 9, 0);
  lv_obj_set_style_bg_color(fl, C(HX_ACCENT), 0);
  lv_obj_set_style_bg_opa(fl, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(fl, C(HX_ACCENT), 0);
  lv_obj_set_style_shadow_spread(fl, 3, 0);
  lv_obj_set_style_shadow_opa(fl, 40, 0);
  lv_obj_t * fi = icon(fl, &ic_truck_16, HX_ACCENT_INK);
  lv_obj_center(fi);
  lv_obj_t * c = col(plan, 1);
  lv_obj_set_flex_grow(c, 1);
  label(c, &vn_inter_13s, HX_TEXT, "Plan de r\xC3\xA9gulation");
  s_planSub = label(c, &vn_inter_11, HX_TEXT2, "");
  oneLine(s_planSub, 250);
  lv_obj_t * btn = button(plan, "Transferts", &ic_route_16, B_TONAL, true);
  lv_obj_add_event_cb(btn, [](lv_event_t *) { openTransfers(); }, LV_EVENT_CLICKED, nullptr);

  s_seg = segmented(root, FILTERS, NF, 0, [](int i) {
    s_filter = i;
    refresh();
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
  }, 32);
  lv_obj_set_pos(s_seg, 8, 60);
  lv_obj_set_width(s_seg, CONTENT_W - 16);
  segSet(s_seg, 0, false);

  lv_obj_t * holder = box(root);
  lv_obj_set_pos(holder, 8, 98);
  lv_obj_set_size(holder, CONTENT_W - 16, CONTENT_H - 98);
  s_list = card(holder, 0);
  lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(s_list, C(HX_TEXT3), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(s_list, 120, LV_PART_SCROLLBAR);
  // État vide (.vn-empty)
  s_empty = col(s_list, 4);
  lv_obj_set_width(s_empty, LV_PCT(100));
  lv_obj_set_style_pad_ver(s_empty, 18, 0);
  lv_obj_set_flex_align(s_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t * ei = box(s_empty);
  lv_obj_set_size(ei, 40, 40);
  lv_obj_set_style_radius(ei, 12, 0);
  lv_obj_set_style_bg_color(ei, C(HX_S2), 0);
  lv_obj_set_style_bg_opa(ei, LV_OPA_COVER, 0);
  lv_obj_t * eic = icon(ei, &ic_check_20, HX_GOOD);
  lv_obj_center(eic);
  label(s_empty, &vn_inter_13s, HX_TEXT, "Rien \xC3\xA0 signaler");
  label(s_empty, &vn_inter_12, HX_TEXT2, "Aucune alerte dans cette cat\xC3\xA9gorie");
  lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
}

const ScreenDef SCR_ALERTS = { "Alertes", "ALERTES & R\xC3\x89GULATION", &ic_bell_20, build, refresh, nullptr };

} // namespace ui
