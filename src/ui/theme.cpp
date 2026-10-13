#include "theme.h"
#include "../core/model.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace ui {

lv_color_t toneColor(Tone t) {
  switch (t) {
    case T_GOOD: return C(HX_GOOD);
    case T_WARN: return C(HX_WARN);
    case T_BAD: return C(HX_BAD);
    case T_ACCENT: return C(HX_ACCENT);
    case T_ELEC: return C(HX_ELEC);
    case T_TRANSIT: return C(HX_TRANSIT);
    case T_CLOSED: return C(0xA3ABB8);
    default: return C(HX_TEXT2);
  }
}

lv_color_t statusColor(uint8_t st) {
  switch (st) {
    case vn::ST_EMPTY: return C(HX_BAD);
    case vn::ST_LOW:   return C(HX_WARN);
    case vn::ST_OK:    return C(HX_GOOD);
    case vn::ST_HIGH:  return C(HX_ELEC);
    case vn::ST_FULL:  return C(HX_TRANSIT);
    default:           return C(HX_CLOSED);
  }
}
Tone statusTone(uint8_t st) {
  switch (st) {
    case vn::ST_EMPTY: return T_BAD;
    case vn::ST_LOW:   return T_WARN;
    case vn::ST_OK:    return T_GOOD;
    case vn::ST_HIGH:  return T_ELEC;
    case vn::ST_FULL:  return T_TRANSIT;
    default:           return T_CLOSED;
  }
}

// ── Formats (fr-FR) ──────────────────────────────────────────────────
void fmtInt(char * out, size_t n, int v, bool narrow) {
  char digits[16];
  bool neg = v < 0;
  snprintf(digits, sizeof(digits), "%d", neg ? -v : v);
  const char * sep = narrow ? "\xE2\x80\xAF" : "\xC2\xA0";
  size_t sl = strlen(sep), L = strlen(digits), o = 0;
  if (neg && o + 3 < n) { memcpy(out + o, "\xE2\x88\x92", 3); o += 3; }
  for (size_t i = 0; i < L && o + 1 < n; i++) {
    out[o++] = digits[i];
    size_t rem = L - i - 1;
    if (rem && rem % 3 == 0 && o + sl < n) { memcpy(out + o, sep, sl); o += sl; }
  }
  out[o] = 0;
}

void fmtSigned(char * out, size_t n, int v) {
  if (v > 0) snprintf(out, n, "+%d", v);
  else if (v < 0) snprintf(out, n, "\xE2\x88\x92%d", -v);
  else snprintf(out, n, "0");
}

void fmtAgo(char * out, size_t n, int64_t ts, int64_t now) {
  if (!ts || !now) { snprintf(out, n, "jamais"); return; }
  int64_t s = now - ts;
  if (s < 0) s = 0;
  if (s < 45) snprintf(out, n, "\xC3\xA0 l'instant");
  else if (s < 3600) snprintf(out, n, "il y a %d min", (int)((s + 30) / 60));
  else if (s < 86400) snprintf(out, n, "il y a %d h", (int)((s + 1800) / 3600));
  else snprintf(out, n, "il y a %d j", (int)(s / 86400));
}

void fmtDur(char * out, size_t n, int m) {
  if (m < 0) { snprintf(out, n, "\xE2\x80\x94"); return; }
  if (m < 60) snprintf(out, n, "%d min", m);
  else if (m < 48 * 60) snprintf(out, n, "%d h %02d", m / 60, m % 60);
  else snprintf(out, n, "%d j", m / 1440);
}

void fmtDist(char * out, size_t n, float m) {
  if (!(m >= 0)) { snprintf(out, n, "\xE2\x80\x94"); return; }
  if (m < 1000) snprintf(out, n, "%d m", (int)(lroundf(m / 10) * 10));
  else {
    char b[16];
    snprintf(b, sizeof(b), m < 10000 ? "%.1f" : "%.0f", m / 1000);
    for (char * p = b; *p; p++) if (*p == '.') *p = ',';
    snprintf(out, n, "%s km", b);
  }
}

void fmtClock(char * out, size_t n, int64_t t) {
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  plat::localTime(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void fmtFloat1(char * out, size_t n, float v) {
  snprintf(out, n, "%.1f", v);
  for (char * p = out; *p; p++) if (*p == '.') *p = ',';
}

// ── Briques ──────────────────────────────────────────────────────────
lv_obj_t * box(lv_obj_t * parent) {
  lv_obj_t * o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  return o;
}

lv_obj_t * card(lv_obj_t * parent, lv_coord_t pad) {
  lv_obj_t * o = box(parent);
  lv_obj_set_style_bg_color(o, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_white(), 0);
  lv_obj_set_style_border_opa(o, OPA_BORDER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 10, 0);
  lv_obj_set_style_pad_all(o, pad, 0);
  return o;
}

lv_obj_t * label(lv_obj_t * parent, const lv_font_t * f, uint32_t hex, const char * txt) {
  lv_obj_t * l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C(hex), 0);
  lv_label_set_text(l, txt ? txt : "");
  return l;
}

lv_obj_t * eyebrow(lv_obj_t * parent, const char * txt) {
  lv_obj_t * l = label(parent, &vn_mono_10, HX_TEXT3, txt);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  return l;
}

lv_obj_t * icon(lv_obj_t * parent, const lv_img_dsc_t * img, uint32_t hex) {
  lv_obj_t * o = lv_img_create(parent);
  lv_img_set_src(o, img);
  iconColor(o, C(hex));
  return o;
}
void oneLine(lv_obj_t * l, lv_coord_t w) {
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, w);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

void iconColor(lv_obj_t * ic, lv_color_t c) {
  lv_obj_set_style_img_recolor(ic, c, 0);
  lv_obj_set_style_img_recolor_opa(ic, LV_OPA_COVER, 0);
}

lv_obj_t * row(lv_obj_t * parent, lv_coord_t gap) {
  lv_obj_t * o = box(parent);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(o, gap, 0);
  return o;
}
lv_obj_t * col(lv_obj_t * parent, lv_coord_t gap) {
  lv_obj_t * o = box(parent);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(o, gap, 0);
  return o;
}

static void badgeStyle(lv_obj_t * b, Tone t) {
  lv_color_t c = toneColor(t);
  if (t == T_NEUTRAL) { lv_obj_set_style_bg_color(b, lv_color_white(), 0); lv_obj_set_style_bg_opa(b, 15, 0); }
  else if (t == T_CLOSED) { lv_obj_set_style_bg_color(b, C(HX_CLOSED), 0); lv_obj_set_style_bg_opa(b, 51, 0); }
  else { lv_obj_set_style_bg_color(b, c, 0); lv_obj_set_style_bg_opa(b, 31, 0); }
  lv_obj_t * l = lv_obj_get_child(b, 0);
  if (l) lv_obj_set_style_text_color(l, c, 0);
}
lv_obj_t * badge(lv_obj_t * parent, const char * txt, Tone t) {
  lv_obj_t * b = box(parent);
  lv_obj_set_height(b, 18);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_pad_hor(b, 6, 0);
  lv_obj_t * l = label(b, &vn_mono_10, HX_TEXT2, txt);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  lv_obj_center(l);
  badgeStyle(b, t);
  return b;
}
void badgeSet(lv_obj_t * b, const char * txt, Tone t) {
  lv_obj_t * l = lv_obj_get_child(b, 0);
  if (l && txt) lv_label_set_text(l, txt);
  badgeStyle(b, t);
}

lv_obj_t * pill(lv_obj_t * parent) {
  lv_obj_t * o = row(parent, 5);
  lv_obj_set_height(o, 24);
  lv_obj_set_style_pad_hor(o, 8, 0);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_white(), 0);
  lv_obj_set_style_border_opa(o, OPA_BORDER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  return o;
}

lv_obj_t * dot(lv_obj_t * parent, lv_coord_t d, lv_color_t c) {
  lv_obj_t * o = box(parent);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

static lv_color_t btnTextColor(BtnVariant v) {
  switch (v) {
    case B_PRIMARY: return C(HX_ACCENT_INK);
    case B_TONAL: return C(HX_ACCENT);
    case B_GHOST: return C(HX_TEXT2);
    case B_DANGER: return C(HX_BAD);
    default: return C(HX_TEXT);
  }
}

lv_obj_t * button(lv_obj_t * parent, const char * txt, const lv_img_dsc_t * ic, BtnVariant v, bool small) {
  lv_obj_t * b = row(parent, 6);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_height(b, small ? 30 : 36);
  lv_obj_set_style_pad_hor(b, small ? 10 : 14, 0);
  lv_obj_set_style_radius(b, small ? 6 : 8, 0);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  switch (v) {
    case B_PRIMARY:
      lv_obj_set_style_bg_color(b, C(HX_ACCENT), 0);
      lv_obj_set_style_bg_color(b, C(0xE07508), LV_STATE_PRESSED);
      break;
    case B_SECONDARY:
      lv_obj_set_style_bg_color(b, C(HX_S2), 0);
      lv_obj_set_style_bg_color(b, C(HX_S3), LV_STATE_PRESSED);
      lv_obj_set_style_border_color(b, lv_color_white(), 0);
      lv_obj_set_style_border_opa(b, OPA_BORDER_STRONG, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      break;
    case B_TONAL:
      lv_obj_set_style_bg_color(b, C(HX_ACCENT), 0);
      lv_obj_set_style_bg_opa(b, 36, 0);
      lv_obj_set_style_bg_opa(b, 64, LV_STATE_PRESSED);
      lv_obj_set_style_border_color(b, C(HX_ACCENT), 0);
      lv_obj_set_style_border_opa(b, 90, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      break;
    case B_GHOST:
      lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
      lv_obj_set_style_bg_color(b, C(HX_S2), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
      break;
    case B_DANGER:
      lv_obj_set_style_bg_color(b, C(HX_BAD), 0);
      lv_obj_set_style_bg_opa(b, 31, 0);
      lv_obj_set_style_bg_opa(b, 60, LV_STATE_PRESSED);
      lv_obj_set_style_border_color(b, C(HX_BAD), 0);
      lv_obj_set_style_border_opa(b, 102, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      break;
  }
  lv_color_t tc = btnTextColor(v);
  if (ic) { lv_obj_t * i = lv_img_create(b); lv_img_set_src(i, ic); iconColor(i, tc); }
  if (txt) {
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &vn_inter_13s, 0);
    lv_obj_set_style_text_color(l, tc, 0);
    lv_label_set_text(l, txt);
  }
  return b;
}

void buttonText(lv_obj_t * btn, const char * txt) {
  uint32_t n = lv_obj_get_child_cnt(btn);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t * c = lv_obj_get_child(btn, i);
    if (lv_obj_check_type(c, &lv_label_class)) { lv_label_set_text(c, txt); return; }
  }
}

lv_obj_t * iconButton(lv_obj_t * parent, const lv_img_dsc_t * ic, bool filled, lv_coord_t size) {
  lv_obj_t * b = box(parent);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_bg_color(b, C(HX_S3), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
  if (filled) {
    lv_obj_set_style_bg_color(b, C(HX_S1), 0);
    lv_obj_set_style_bg_opa(b, 240, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, OPA_BORDER_STRONG, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 14, 0);
    lv_obj_set_style_shadow_ofs_y(b, 4, 0);
    lv_obj_set_style_shadow_color(b, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(b, 110, 0);
  }
  lv_obj_t * i = lv_img_create(b);
  lv_img_set_src(i, ic);
  iconColor(i, C(filled ? HX_TEXT : HX_TEXT2));
  lv_obj_center(i);
  return b;
}

// ── Chip ─────────────────────────────────────────────────────────────
lv_obj_t * chip(lv_obj_t * parent, const char * txt, const lv_img_dsc_t * ic) {
  lv_obj_t * c = row(parent, 5);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_height(c, 26);
  lv_obj_set_style_pad_left(c, ic ? 8 : 10, 0);
  lv_obj_set_style_pad_right(c, 5, 0);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(c, C(HX_S1), 0);
  lv_obj_set_style_bg_opa(c, 235, 0);
  lv_obj_set_style_border_color(c, lv_color_white(), 0);
  lv_obj_set_style_border_opa(c, OPA_BORDER_STRONG, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  if (ic) { lv_obj_t * i = lv_img_create(c); lv_img_set_src(i, ic); iconColor(i, C(HX_TEXT2)); }
  label(c, &vn_inter_12, HX_TEXT2, txt);
  lv_obj_t * cnt = box(c);
  lv_obj_set_style_min_width(cnt, 18, 0);
  lv_obj_set_height(cnt, 16);
  lv_obj_set_style_pad_hor(cnt, 4, 0);
  lv_obj_set_style_radius(cnt, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(cnt, LV_OPA_COVER, 0);
  lv_obj_t * cl = label(cnt, &vn_mono_10, HX_TEXT2, "0");
  lv_obj_center(cl);
  chipSet(c, false, 0);
  return c;
}

void chipSet(lv_obj_t * c, bool on, int count) {
  uint32_t n = lv_obj_get_child_cnt(c);
  lv_obj_t * cnt = lv_obj_get_child(c, n - 1);
  lv_obj_t * txt = lv_obj_get_child(c, n - 2);
  lv_obj_t * ic = n >= 3 ? lv_obj_get_child(c, 0) : nullptr;
  lv_obj_set_style_bg_color(c, on ? C(HX_ACCENT) : C(HX_S1), 0);
  lv_obj_set_style_bg_opa(c, on ? 40 : 235, 0);
  lv_obj_set_style_border_color(c, on ? C(HX_ACCENT) : lv_color_white(), 0);
  lv_obj_set_style_border_opa(c, on ? 140 : OPA_BORDER_STRONG, 0);
  lv_obj_set_style_text_color(txt, C(on ? HX_TEXT : HX_TEXT2), 0);
  if (ic) iconColor(ic, C(on ? HX_ACCENT : HX_TEXT2));
  lv_obj_set_style_bg_color(cnt, on ? C(HX_ACCENT) : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(cnt, on ? 56 : 15, 0);
  lv_obj_t * cl = lv_obj_get_child(cnt, 0);
  lv_obj_set_style_text_color(cl, C(on ? HX_ACCENT : HX_TEXT2), 0);
  if (count >= 0) lv_label_set_text_fmt(cl, "%d", count);
}

// ── Contrôle segmenté ────────────────────────────────────────────────
struct SegData { int n, value; SegCb cb; lv_obj_t * thumb; lv_obj_t * opts[6]; };

static void segLayout(lv_obj_t * seg, bool anim) {
  SegData * d = (SegData *)lv_obj_get_user_data(seg);
  lv_obj_t * o = d->opts[d->value];
  lv_obj_update_layout(seg);
  lv_coord_t x = lv_obj_get_x(o), w = lv_obj_get_width(o);
  lv_obj_set_width(d->thumb, w);
  for (int i = 0; i < d->n; i++)
    lv_obj_set_style_text_color(lv_obj_get_child(d->opts[i], 0), C(i == d->value ? HX_TEXT : HX_TEXT3), 0);
  if (!anim) { lv_obj_set_x(d->thumb, x); return; }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, d->thumb);
  lv_anim_set_values(&a, lv_obj_get_x(d->thumb), x);
  lv_anim_set_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void * v, int32_t x) { lv_obj_set_x((lv_obj_t *)v, (lv_coord_t)x); });
  lv_anim_start(&a);
}

static void segEvent(lv_event_t * e) {
  lv_obj_t * opt = lv_event_get_current_target(e);
  lv_obj_t * seg = lv_obj_get_parent(opt);
  SegData * d = (SegData *)lv_obj_get_user_data(seg);
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx == d->value) return;
  d->value = idx;
  segLayout(seg, true);
  if (d->cb) d->cb(idx);
}

static void segDelete(lv_event_t * e) {
  lv_obj_t * seg = lv_event_get_target(e);
  void * d = lv_obj_get_user_data(seg);
  if (d) lv_mem_free(d);
}

lv_obj_t * segmented(lv_obj_t * parent, const char * const * labels, int n, int value, SegCb cb, lv_coord_t h) {
  lv_obj_t * s = row(parent, 0);
  lv_obj_set_height(s, h);
  lv_obj_set_style_pad_all(s, 2, 0);
  lv_obj_set_style_radius(s, 8, 0);
  lv_obj_set_style_bg_color(s, C(HX_S2), 0);
  lv_obj_set_style_bg_opa(s, 240, 0);
  lv_obj_set_style_border_color(s, lv_color_white(), 0);
  lv_obj_set_style_border_opa(s, OPA_BORDER, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  SegData * d = (SegData *)lv_mem_alloc(sizeof(SegData));
  memset(d, 0, sizeof(*d));
  d->n = n > 6 ? 6 : n; d->value = value; d->cb = cb;
  lv_obj_set_user_data(s, d);
  lv_obj_add_event_cb(s, segDelete, LV_EVENT_DELETE, nullptr);
  d->thumb = box(s);
  lv_obj_add_flag(d->thumb, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_radius(d->thumb, 6, 0);
  lv_obj_set_style_bg_color(d->thumb, C(HX_S3), 0);
  lv_obj_set_style_bg_opa(d->thumb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(d->thumb, lv_color_white(), 0);
  lv_obj_set_style_border_opa(d->thumb, OPA_BORDER_STRONG, 0);
  lv_obj_set_style_border_width(d->thumb, 1, 0);
  lv_obj_set_y(d->thumb, 0);
  lv_obj_set_height(d->thumb, h - 6);
  for (int i = 0; i < d->n; i++) {
    lv_obj_t * o = box(s);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(o, LV_PCT(100));
    lv_obj_set_flex_grow(o, 1);
    lv_obj_set_style_pad_hor(o, 6, 0);
    lv_obj_set_ext_click_area(o, 3);
    lv_obj_t * l = label(o, &vn_inter_12, HX_TEXT3, labels[i]);
    lv_obj_center(l);
    d->opts[i] = o;
    lv_obj_add_event_cb(o, segEvent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  segLayout(s, false);
  return s;
}

void segSet(lv_obj_t * seg, int value, bool anim) {
  SegData * d = (SegData *)lv_obj_get_user_data(seg);
  if (!d || value < 0 || value >= d->n) return;
  d->value = value;
  segLayout(seg, anim);
}

void segLabel(lv_obj_t * seg, int index, const char * txt) {
  SegData * d = (SegData *)lv_obj_get_user_data(seg);
  if (!d || index < 0 || index >= d->n) return;
  lv_label_set_text(lv_obj_get_child(d->opts[index], 0), txt);
}

// ── Barre composée ───────────────────────────────────────────────────
lv_obj_t * meter(lv_obj_t * parent, lv_coord_t w, lv_coord_t h) {
  lv_obj_t * m = row(parent, 2);
  lv_obj_set_size(m, w, h);
  lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(m, true, 0);
  lv_obj_set_style_bg_color(m, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(m, 15, 0);
  for (int i = 0; i < 4; i++) {
    lv_obj_t * s = box(m);
    lv_obj_set_height(s, LV_PCT(100));
    lv_obj_set_width(s, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  }
  return m;
}

void meterSet(lv_obj_t * m, const int * values, const lv_color_t * colors, int n) {
  for (int i = 0; i < 4; i++) {
    lv_obj_t * s = lv_obj_get_child(m, i);
    if (i < n && values[i] > 0) {
      lv_obj_clear_flag(s, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_flex_grow(s, (uint8_t)(values[i] > 255 ? 255 : values[i]));
      lv_obj_set_style_bg_color(s, colors[i], 0);
    } else {
      lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

lv_obj_t * toggle(lv_obj_t * parent, bool on, lv_event_cb_t cb, void * ud) {
  lv_obj_t * s = lv_switch_create(parent);
  lv_obj_set_size(s, 40, 22);
  lv_obj_set_style_bg_color(s, C(HX_S3), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(s, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_opa(s, OPA_BORDER_STRONG, LV_PART_MAIN);
  lv_obj_set_style_border_width(s, 1, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, C(HX_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(s, C(HX_TEXT2), LV_PART_KNOB);
  lv_obj_set_style_bg_color(s, C(HX_ACCENT_INK), LV_PART_KNOB | LV_STATE_CHECKED);
  lv_obj_set_style_pad_all(s, -3, LV_PART_KNOB);
  if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
  lv_obj_set_ext_click_area(s, 10);
  if (cb) lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
  return s;
}

// ── Animations ───────────────────────────────────────────────────────
static void animOpa(void * o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void animTy(void * o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, (lv_coord_t)v, 0); }

void fadeIn(lv_obj_t * o, uint32_t ms, int dy) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_exec_cb(&a, animOpa);
  lv_anim_start(&a);
  if (dy) {
    lv_anim_set_values(&a, dy, 0);
    lv_anim_set_exec_cb(&a, animTy);
    lv_anim_start(&a);
  }
}

static void pulseExec(void * o, int32_t v) {
  lv_obj_t * d = (lv_obj_t *)o;
  lv_obj_set_style_shadow_spread(d, (lv_coord_t)(v * 5 / 100), 0);
  lv_obj_set_style_shadow_opa(d, (lv_opa_t)(150 * (100 - v) / 100), 0);
}
void pulse(lv_obj_t * d, lv_color_t c) {
  lv_obj_set_style_shadow_color(d, c, 0);
  lv_obj_set_style_shadow_width(d, 2, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, d);
  lv_anim_set_values(&a, 0, 100);
  lv_anim_set_time(&a, 1700);
  lv_anim_set_repeat_delay(&a, 700);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, pulseExec);
  lv_anim_start(&a);
}

void themeInit(lv_disp_t * disp) {
  lv_theme_t * th = lv_theme_default_init(disp, C(HX_ACCENT), C(HX_ELEC), true, &vn_inter_12);
  lv_disp_set_theme(disp, th);
  lv_obj_t * scr = lv_disp_get_scr_act(disp);
  lv_obj_set_style_bg_color(scr, C(HX_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scr, C(HX_TEXT), 0);
  lv_obj_set_style_text_font(scr, &vn_inter_12, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

} // namespace ui
