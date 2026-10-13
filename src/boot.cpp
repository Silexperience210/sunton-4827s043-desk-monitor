// ── Démarrage : écran d'étapes, cause du dernier redémarrage, mode sans échec ─
#ifdef ARDUINO
#include "boot.h"
#include "config.h"
#include "lvgl.h"
#include "ui/theme.h"
#include "platform/platform.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

using ui::C;

namespace boot {

// Mémoire RTC non initialisée : survit aux plantages et redémarrages logiciels
// (pas à une coupure d'alimentation, d'où le nombre magique).
static const uint32_t RTC_MAGIC = 0x56424F54;   // "VBOT"
RTC_NOINIT_ATTR static uint32_t r_magic;
RTC_NOINIT_ATTR static uint32_t r_stage;
RTC_NOINIT_ATTR static uint32_t r_crashes;
RTC_NOINIT_ATTR static char     r_note[96];

static esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
static Stage s_prevStage = S_NONE;
static uint32_t s_crashes = 0;
static bool s_incident = false, s_stableDone = false;
static lv_obj_t * s_root = nullptr, * s_text = nullptr, * s_bar = nullptr;
static uint32_t s_t0 = 0;

static const char * const STAGE_NAMES[S_COUNT] = {
  "-", "d\xC3\xA9marrage", "initialisation de l'\xC3\xA9" "cran", "ouverture du stockage", "lecture des r\xC3\xA9glages",
  "r\xC3\xA9servation de la m\xC3\xA9moire", "initialisation du tactile", "construction de l'interface",
  "premier affichage", "d\xC3\xA9marrage du r\xC3\xA9seau", "fonctionnement normal",
};

static const char * reasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_PANIC:    return "plantage du programme";
    case ESP_RST_INT_WDT:  return "blocage (chien de garde d'interruption)";
    case ESP_RST_TASK_WDT: return "blocage (chien de garde de t\xC3\xA2" "che)";
    case ESP_RST_WDT:      return "blocage (chien de garde)";
    case ESP_RST_BROWNOUT: return "chute de tension : alimentation USB trop faible";
    default:               return "red\xC3\xA9marrage inattendu";
  }
}

uint32_t bootMs() { return millis() - s_t0; }

void begin() {
  s_t0 = millis();
  s_reason = esp_reset_reason();
  if (r_magic != RTC_MAGIC) { r_magic = RTC_MAGIC; r_stage = S_NONE; r_crashes = 0; r_note[0] = 0; }
  r_note[sizeof(r_note) - 1] = 0;
  s_prevStage = r_stage < S_COUNT ? (Stage)r_stage : S_NONE;
  s_incident = s_reason == ESP_RST_PANIC || s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_TASK_WDT ||
               s_reason == ESP_RST_WDT || s_reason == ESP_RST_BROWNOUT;
  if (s_incident) r_crashes++;
  else { r_crashes = 0; r_note[0] = 0; }
  s_crashes = r_crashes;
  r_stage = S_START;
  Serial.printf("\n[boot] %s %s \xC2\xB7 cause du d\xC3\xA9marrage : %d\n", VN_NAME, VN_VERSION, (int)s_reason);
  if (s_incident)
    Serial.printf("[boot] INCIDENT pr\xC3\xA9" "c\xC3\xA9" "dent : %s pendant \xC2\xAB %s \xC2\xBB%s%s (n\xC2\xB0 %u)\n", reasonText(s_reason),
                  STAGE_NAMES[s_prevStage], r_note[0] ? " \xE2\x80\x94 " : "", r_note, (unsigned)s_crashes);
}

bool safeMode() { return s_crashes >= 3; }

static lv_obj_t * mkLabel(lv_obj_t * parent, const lv_font_t * f, uint32_t col, const char * txt) {
  lv_obj_t * l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C(col), 0);
  lv_label_set_text(l, txt);
  return l;
}

void screenOpen() {
  lv_obj_t * scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C(ui::HX_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  s_root = lv_obj_create(scr);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, 480, 272);
  lv_obj_set_style_bg_color(s_root, C(ui::HX_BG), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * logo = lv_img_create(s_root);            // même mise en page que l'écran d'accueil
  lv_img_set_src(logo, &img_logo_56);
  lv_obj_align(logo, LV_ALIGN_CENTER, 0, -44);
  lv_obj_t * brand = mkLabel(s_root, &vn_mono_20b, ui::HX_TEXT, "");
  lv_obj_set_style_text_letter_space(brand, 4, 0);
  lv_label_set_recolor(brand, true);
  lv_label_set_text(brand, "VELOH#F5820D NAV#");
  lv_obj_align(brand, LV_ALIGN_CENTER, 0, 6);

  lv_obj_t * track = lv_obj_create(s_root);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, 180, 4);
  lv_obj_set_style_radius(track, 2, 0);
  lv_obj_set_style_bg_color(track, C(ui::HX_S3), 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_align(track, LV_ALIGN_CENTER, 0, 48);
  s_bar = lv_obj_create(track);
  lv_obj_remove_style_all(s_bar);
  lv_obj_set_size(s_bar, 8, 4);
  lv_obj_set_style_radius(s_bar, 2, 0);
  lv_obj_set_style_bg_color(s_bar, C(ui::HX_ACCENT), 0);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);

  s_text = mkLabel(s_root, &vn_inter_12, ui::HX_TEXT2, "D\xC3\xA9marrage\xE2\x80\xA6");
  lv_obj_set_width(s_text, 440);
  lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_text, LV_ALIGN_CENTER, 0, 72);
  lv_obj_t * ver = mkLabel(s_root, &vn_mono_10, ui::HX_TEXT3, "v" VN_VERSION " \xC2\xB7 journal : port s\xC3\xA9rie 115200 bauds");
  lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_refr_now(nullptr);
}

void screenClose() {
  if (s_root) lv_obj_del(s_root);
  s_root = s_text = s_bar = nullptr;
}

void stage(Stage s, const char * text) {
  r_stage = s;
  Serial.printf("[boot] %5u ms \xC2\xB7 %s\n", (unsigned)bootMs(), text);
  if (s_text) {
    char b[128];
    snprintf(b, sizeof(b), "%s\xE2\x80\xA6", text);
    lv_label_set_text(s_text, b);
    lv_obj_set_width(s_bar, (lv_coord_t)(8 + 172 * s / (S_COUNT - 1)));
    lv_refr_now(nullptr);
  }
}

bool showIncident() {
  if (!s_incident || !s_root) return false;
  lv_obj_t * box = lv_obj_create(s_root);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 440, 102);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 46);
  lv_obj_set_style_bg_color(box, C(0x2A1414), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, C(ui::HX_BAD), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 10, 0);
  lv_obj_set_style_pad_all(box, 10, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 3, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  char b[200];
  snprintf(b, sizeof(b), "Red\xC3\xA9marrage apr\xC3\xA8s un incident%s", s_crashes > 1 ? " (r\xC3\xA9p\xC3\xA9t\xC3\xA9)" : "");
  mkLabel(box, &vn_inter_13s, ui::HX_BAD, b);
  snprintf(b, sizeof(b), "Cause : %s", reasonText(s_reason));
  lv_obj_t * l1 = mkLabel(box, &vn_inter_12, ui::HX_TEXT, b);
  lv_obj_set_width(l1, 416);
  snprintf(b, sizeof(b), "Pendant : %s%s%s", STAGE_NAMES[s_prevStage], r_note[0] ? " \xC2\xB7 " : "", r_note);
  lv_obj_t * l2 = mkLabel(box, &vn_inter_12, ui::HX_TEXT2, b);
  lv_obj_set_width(l2, 416);
  lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
  if (safeMode()) snprintf(b, sizeof(b), "Mode sans \xC3\xA9" "chec : r\xC3\xA9seau d\xC3\xA9sactiv\xC3\xA9 jusqu'au prochain d\xC3\xA9" "branchement.");
  else if (s_reason == ESP_RST_BROWNOUT) snprintf(b, sizeof(b), "Essayer un autre c\xC3\xA2" "ble ou un chargeur USB 5 V / 2 A.");
  else snprintf(b, sizeof(b), "Le d\xC3\xA9marrage continue.");
  lv_obj_t * l3 = mkLabel(box, &vn_inter_12, ui::HX_WARN, b);
  lv_obj_set_width(l3, 416);
  if (s_text) lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(nullptr);
  delay(6000);
  lv_obj_del(box);
  if (s_text) lv_obj_clear_flag(s_text, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(nullptr);
  return true;
}

void fatal(const char * title, const char * detail) {
  Serial.printf("[boot] ERREUR FATALE : %s \xE2\x80\x94 %s\n", title, detail);
  if (!s_root) screenOpen();
  if (s_text) {
    char b[200];
    snprintf(b, sizeof(b), "%s\n%s", title, detail);
    lv_label_set_text(s_text, b);
    lv_obj_set_style_text_color(s_text, C(ui::HX_BAD), 0);
    lv_obj_align(s_text, LV_ALIGN_CENTER, 0, 78);
    lv_refr_now(nullptr);
  }
  plat::setBacklight(80);
  for (;;) delay(1000);
}

void tick() {
  // Fonctionnement stable depuis 2 min : le compteur d'incidents repart de zéro
  if (!s_stableDone && millis() - s_t0 > 120000) { s_stableDone = true; r_crashes = 0; r_note[0] = 0; }
}

} // namespace boot

// Assertion LVGL : au lieu de figer l'écran (while(1)), on note l'endroit et on redémarre ;
// l'incident est affiché au démarrage suivant.
extern "C" void vn_lv_assert_fail(const char * file, int line) {
  const char * f = strrchr(file, '/');
  snprintf(boot::r_note, sizeof(boot::r_note), "LVGL %s:%d", f ? f + 1 : file, line);
  Serial.printf("[lvgl] assertion %s:%d\n", file, line);
  Serial.flush();
  abort();
}
#endif // ARDUINO
