// ── Dalle RGB 480×272 (esp_lcd) + pilote d'affichage LVGL ────────────
#ifdef ARDUINO
#include "display.h"
#include "board.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

namespace hal {

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_draw_buf_t s_drawBuf;
static lv_disp_drv_t s_drv;
static const int BUF_LINES = 40;

#ifdef VN_QEMU
// Émulateur : copie de l'image dans un tampon dont l'adresse est publiée sur le port série,
// pour que le test de démarrage puisse en faire des captures (QEMU n'émule pas la dalle RGB).
static uint16_t * s_qemuFb = nullptr;
static void qemuMirror(const lv_area_t * a, const lv_color_t * px) {
  if (!s_qemuFb) {
    s_qemuFb = (uint16_t *)heap_caps_calloc(480 * 272, 2, MALLOC_CAP_SPIRAM);
    Serial.printf("[qemu] fb=%p\n", s_qemuFb);
  }
  if (!s_qemuFb) return;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) s_qemuFb[y * 480 + x] = (px++)->full;
}
#endif

static void flushCb(lv_disp_drv_t * drv, const lv_area_t * a, lv_color_t * px) {
#ifdef VN_QEMU
  qemuMirror(a, px);
#endif
  if (s_panel)
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
  lv_disp_flush_ready(drv);
}

static bool makePanel(bool bounce) {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = 480;
  cfg.timings.v_res = 272;
  cfg.timings.hsync_pulse_width = LCD_HSYNC_PW;
  cfg.timings.hsync_back_porch = LCD_HSYNC_BP;
  cfg.timings.hsync_front_porch = LCD_HSYNC_FP;
  cfg.timings.vsync_pulse_width = LCD_VSYNC_PW;
  cfg.timings.vsync_back_porch = LCD_VSYNC_BP;
  cfg.timings.vsync_front_porch = LCD_VSYNC_FP;
  cfg.timings.flags.hsync_idle_low = 1;
  cfg.timings.flags.vsync_idle_low = 1;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 16;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)
  cfg.sram_trans_align = 64;
  cfg.psram_trans_align = 64;
#endif
  cfg.hsync_gpio_num = LCD_PIN_HSYNC;
  cfg.vsync_gpio_num = LCD_PIN_VSYNC;
  cfg.de_gpio_num = LCD_PIN_DE;
  cfg.pclk_gpio_num = LCD_PIN_PCLK;
  const int pins[16] = LCD_DATA_PINS;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = pins[i];
  cfg.disp_gpio_num = -1;
  cfg.flags.fb_in_psram = 1;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = bounce ? 480 * 8 : 0;
#endif
  s_panel = nullptr;
  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
  if (err != ESP_OK || !s_panel) {
    Serial.printf("[lcd] new_rgb_panel a échoué (%s, rebond %d)\n", esp_err_to_name(err), bounce);
    s_panel = nullptr;
    return false;
  }
  err = esp_lcd_panel_reset(s_panel);
  if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
  if (err != ESP_OK) {
    Serial.printf("[lcd] init a échoué (%s)\n", esp_err_to_name(err));
    esp_lcd_panel_del(s_panel);
    s_panel = nullptr;
    return false;
  }
  esp_lcd_panel_disp_on_off(s_panel, true);
  return true;
}

lv_disp_t * displayBegin() {
  // Le tampon de rebond DMA plante (et coupait le rétroéclairage) sur certaines révisions.
  if (!makePanel(true) && !makePanel(false)) {
#ifndef VN_QEMU
    return nullptr;
#endif
  }

  size_t n = 480 * BUF_LINES;
  lv_color_t * buf = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) buf = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&s_drawBuf, buf, nullptr, n);

  lv_disp_drv_init(&s_drv);
  s_drv.hor_res = 480;
  s_drv.ver_res = 272;
  s_drv.flush_cb = flushCb;
  s_drv.draw_buf = &s_drawBuf;
#if VN_ROTATE_180
  s_drv.sw_rotate = 1;
  s_drv.rotated = LV_DISP_ROT_180;
#endif
  return lv_disp_drv_register(&s_drv);
}

} // namespace hal
#endif // ARDUINO
