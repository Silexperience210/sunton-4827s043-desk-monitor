// ── VelohNav — design tokens & briques UI (LVGL 8.3) ─────────────────
// Valeurs reprises de src/ui/tokens.js / ui.css (VelohNav v4) :
// noir profond, surfaces s1–s3, orange #F5820D, vert/ambre/rouge d'état,
// bleu élec, violet transport, typo sans (Inter) + mono (JetBrains Mono).
// Écran 480×272 : échelle typographique réduite d'un cran, cibles ≥ 32 px.
#pragma once
#include "lvgl.h"
#include "icons/vn_icons.h"
#include <stdint.h>

LV_FONT_DECLARE(vn_inter_11)
LV_FONT_DECLARE(vn_inter_12)
LV_FONT_DECLARE(vn_inter_13s)
LV_FONT_DECLARE(vn_inter_15b)
LV_FONT_DECLARE(vn_inter_20b)
LV_FONT_DECLARE(vn_inter_26x)
LV_FONT_DECLARE(vn_mono_10)
LV_FONT_DECLARE(vn_mono_12b)
LV_FONT_DECLARE(vn_mono_20b)

namespace ui {

// ── Couleurs (tokens.js) ─────────────────────────────────────────────
enum : uint32_t {
  HX_BG = 0x07090B, HX_S1 = 0x0D1014, HX_S2 = 0x13171C, HX_S3 = 0x1B2027,
  HX_TEXT = 0xECEFF4, HX_TEXT2 = 0xA9B1BD, HX_TEXT3 = 0x808A99,
  HX_ACCENT = 0xF5820D, HX_ACCENT_HOT = 0xFF4A1C, HX_ACCENT_INK = 0x140A02,
  HX_GOOD = 0x2ECC8F, HX_WARN = 0xF2B33D, HX_BAD = 0xF0524A, HX_CLOSED = 0x5C6573,
  HX_ELEC = 0x5AA9FF, HX_TRANSIT = 0xA78BFA, HX_SATS = 0xFFC53D, HX_MECA = 0xC9D1DC,
  HX_MAPBG = 0x0B0D10,
  HX_WARN_BG = 0x2A2214,     // fond des bandeaux d'avertissement (hors ligne, fraîcheur)
};
inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Bordures : rgba(255,255,255,0.08) / 0.14
static const lv_opa_t OPA_BORDER = 20, OPA_BORDER_STRONG = 36;

enum Tone : uint8_t { T_NEUTRAL, T_GOOD, T_WARN, T_BAD, T_ACCENT, T_ELEC, T_TRANSIT, T_CLOSED };
lv_color_t toneColor(Tone t);
lv_color_t statusColor(uint8_t st);     // vn::StStatus → couleur (axe de remplissage)
Tone       statusTone(uint8_t st);

// ── Mise en forme ────────────────────────────────────────────────────
void fmtInt(char * out, size_t n, int v, bool narrowSep = true);   // 1 522 (espace fine)
void fmtSigned(char * out, size_t n, int v);                        // +12 / −4
void fmtAgo(char * out, size_t n, int64_t ts, int64_t now);         // à l'instant / il y a 3 min
void fmtDur(char * out, size_t n, int minutes);                     // 45 min / 2 h 05
void fmtDist(char * out, size_t n, float m);                        // 380 m / 1,2 km
void fmtClock(char * out, size_t n, int64_t t);                     // 14:05
void fmtFloat1(char * out, size_t n, float v);                      // 8,6

// ── Briques ──────────────────────────────────────────────────────────
lv_obj_t * box(lv_obj_t * parent);                         // conteneur nu (aucun style)
lv_obj_t * card(lv_obj_t * parent, lv_coord_t pad = 8);    // .vn-card
lv_obj_t * label(lv_obj_t * parent, const lv_font_t * f, uint32_t hex, const char * txt = "");
lv_obj_t * eyebrow(lv_obj_t * parent, const char * txt);   // mono 10, capitales, espacé, text3
lv_obj_t * icon(lv_obj_t * parent, const lv_img_dsc_t * img, uint32_t hex);
void       oneLine(lv_obj_t * lbl, lv_coord_t w);              // une ligne, « … » si trop long
void       iconColor(lv_obj_t * ic, lv_color_t c);
lv_obj_t * row(lv_obj_t * parent, lv_coord_t gap = 6);     // flex ligne, centré verticalement
lv_obj_t * col(lv_obj_t * parent, lv_coord_t gap = 2);     // flex colonne

lv_obj_t * badge(lv_obj_t * parent, const char * txt, Tone t);      // .vn-badge
void       badgeSet(lv_obj_t * b, const char * txt, Tone t);
lv_obj_t * pill(lv_obj_t * parent);                                // .vn-pill (conteneur)
lv_obj_t * dot(lv_obj_t * parent, lv_coord_t d, lv_color_t c);

enum BtnVariant : uint8_t { B_PRIMARY, B_SECONDARY, B_TONAL, B_GHOST, B_DANGER };
lv_obj_t * button(lv_obj_t * parent, const char * txt, const lv_img_dsc_t * ic, BtnVariant v, bool small = false);
lv_obj_t * iconButton(lv_obj_t * parent, const lv_img_dsc_t * ic, bool filled = false, lv_coord_t size = 32);
void       buttonText(lv_obj_t * btn, const char * txt);

// Chip filtrant avec compteur (.vn-chip)
lv_obj_t * chip(lv_obj_t * parent, const char * txt, const lv_img_dsc_t * ic = nullptr);
void       chipSet(lv_obj_t * chip, bool pressed, int count);

// Contrôle segmenté (.vn-seg) — pouce animé ; cb(index)
typedef void (*SegCb)(int index);
lv_obj_t * segmented(lv_obj_t * parent, const char * const * labels, int n, int value, SegCb cb, lv_coord_t h = 28);
void       segSet(lv_obj_t * seg, int value, bool anim = true);
void       segLabel(lv_obj_t * seg, int index, const char * txt);

// Barre composée (.vn-meter) : segments {valeur, couleur}
lv_obj_t * meter(lv_obj_t * parent, lv_coord_t w, lv_coord_t h);
void       meterSet(lv_obj_t * m, const int * values, const lv_color_t * colors, int n);

// Interrupteur (.vn-switch)
lv_obj_t * toggle(lv_obj_t * parent, bool on, lv_event_cb_t cb, void * ud);

// Animations utilitaires
void fadeIn(lv_obj_t * o, uint32_t ms = 260, int dy = 6);
void pulse(lv_obj_t * dotObj, lv_color_t c);                 // .vn-dot--live

void themeInit(lv_disp_t * disp);

} // namespace ui
