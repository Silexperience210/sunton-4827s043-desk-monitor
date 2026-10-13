// ── Sunton ESP32-4827S043 (C = tactile capacitif GT911, R = résistif XPT2046) ─
// Brochage vérifié sur les définitions de carte rzeldent/platformio-espressif32-sunton
// et les pilotes esp32-smartdisplay. Dalle 4,3" 480×272 RGB565 parallèle (ST7262).
#pragma once

// Dalle RGB
#define LCD_PIN_DE      40
#define LCD_PIN_VSYNC   41
#define LCD_PIN_HSYNC   39
#define LCD_PIN_PCLK    42
// D0…D15 dans l'ordre attendu par esp_lcd (B0…B4, G0…G5, R0…R4)
#define LCD_DATA_PINS   { 8, 3, 46, 9, 1,   5, 6, 7, 15, 16, 4,   45, 48, 47, 21, 14 }
#define LCD_PCLK_HZ     (8 * 1000 * 1000)
#define LCD_HSYNC_PW    4
#define LCD_HSYNC_BP    43
#define LCD_HSYNC_FP    8
#define LCD_VSYNC_PW    4
#define LCD_VSYNC_BP    12
#define LCD_VSYNC_FP    8
#define VN_PIN_BACKLIGHT 2

// Tactile capacitif GT911 (I2C)
#define GT911_SDA       19
#define GT911_SCL       20
#define GT911_INT       18
#define GT911_RST       38

// Tactile résistif XPT2046 (SPI, partagé avec la carte microSD)
#define XPT_SCLK        12
#define XPT_MISO        13
#define XPT_MOSI        11
#define XPT_CS          38
#define XPT_IRQ         18
// Étalonnage brut → pixels (à ajuster si besoin : voir README)
#ifndef XPT_X_MIN
#define XPT_X_MIN       200
#define XPT_X_MAX       3850
#define XPT_Y_MIN       240
#define XPT_Y_MAX       3800
#define XPT_SWAP_XY     0
#define XPT_INVERT_X    0
#define XPT_INVERT_Y    0
#define XPT_Z_THRESHOLD 400
#endif

// Rotation 180° (montage tête en bas) : -DVN_ROTATE_180=1
#ifndef VN_ROTATE_180
#define VN_ROTATE_180   0
#endif
