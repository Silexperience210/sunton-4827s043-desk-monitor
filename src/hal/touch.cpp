// ── Tactile : GT911 (capacitif, version « C ») ou XPT2046 (résistif, « R ») ─
// Le pilote lit les coordonnées brutes du contrôleur puis applique l'étalonnage
// (g_set.tcal, réglable à l'écran : Réglages → Système → Tactile). Sans
// étalonnage enregistré, il applique la correspondance d'usine de la carte.
#ifdef ARDUINO
#include "display.h"
#include "touch_api.h"
#include "board.h"
#include "../core/settings.h"
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

#if !defined(VN_TOUCH_GT911) && !defined(VN_TOUCH_XPT2046)
#define VN_TOUCH_GT911 1
#endif

namespace hal {

static lv_indev_drv_t s_indev;
static TouchSample s_last = { 0, 0, 0, 0, false, 0 };
static char s_name[24] = "?";

TouchSample touchLast() { return s_last; }
const char * touchName() { return s_name; }

static void report(bool pressed, int rx, int ry, lv_indev_data_t * d) {
  if (pressed) {
    s_last.rx = (int16_t)rx;
    s_last.ry = (int16_t)ry;
    touchToScreen(rx, ry, s_last.x, s_last.y);
    s_last.seq++;
  }
  s_last.pressed = pressed;
  d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  d->point.x = s_last.x;
  d->point.y = s_last.y;
}

#if defined(VN_TOUCH_GT911)
// ── GT911 (I2C). INT n'est relié à GPIO 18 qu'après une modification de la
// carte (pont sur R17) : on fonctionne donc en scrutation, et on accepte les
// deux adresses possibles (0x5D / 0x14) puisque l'état d'INT au reset n'est
// pas garanti.
static uint8_t s_addr = 0;
static uint16_t s_resX = 480, s_resY = 272;

bool touchIsResistive() { return false; }
void touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap) {
  // Correspondance directe (cf. rzeldent/esp32-smartdisplay : swap_xy = mirror_x = mirror_y = false)
  x0 = 0; x1 = (int16_t)(s_resX - 1); y0 = 0; y1 = (int16_t)(s_resY - 1); swap = false;
}

static bool gtRead(uint16_t reg, uint8_t * buf, uint8_t len) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}
static void gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}
static bool probe(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static void readCb(lv_indev_drv_t *, lv_indev_data_t * d) {
  uint8_t st = 0;
  if (!s_addr || !gtRead(0x814E, &st, 1)) { report(false, 0, 0, d); return; }
  if (!(st & 0x80)) {                       // pas de nouvelle trame : on garde l'état précédent
    report(s_last.pressed, s_last.rx, s_last.ry, d);
    return;
  }
  uint8_t n = st & 0x0F;
  bool pressed = false;
  int rx = 0, ry = 0;
  if (n > 0 && n <= 5) {
    uint8_t p[6];                           // 0x814F : id, X(lo,hi), Y(lo,hi), taille
    if (gtRead(0x814F, p, 6)) { rx = p[1] | (p[2] << 8); ry = p[3] | (p[4] << 8); pressed = true; }
  }
  gtWrite(0x814E, 0);                       // acquittement de la trame
  report(pressed, rx, ry, d);
}

void touchBegin() {
  // Reset Goodix : INT bas au front montant de RST (→ 0x5D si INT est câblé)
  pinMode(GT911_INT, OUTPUT);
  pinMode(GT911_RST, OUTPUT);
  digitalWrite(GT911_INT, LOW);
  digitalWrite(GT911_RST, LOW);
  delay(10);
  digitalWrite(GT911_RST, HIGH);
  delay(10);
  pinMode(GT911_INT, INPUT);
  delay(60);
  Wire.begin(GT911_SDA, GT911_SCL, 400000);
  if (probe(0x5D)) s_addr = 0x5D;
  else if (probe(0x14)) s_addr = 0x14;
  if (s_addr) {
    uint8_t id[4] = { 0 }, res[4] = { 0 };
    gtRead(0x8140, id, 4);
    if (gtRead(0x8048, res, 4)) {           // résolution configurée dans le contrôleur
      uint16_t rx = res[0] | (res[1] << 8), ry = res[2] | (res[3] << 8);
      if (rx > 100 && rx < 4096 && ry > 100 && ry < 4096) { s_resX = rx; s_resY = ry; }
    }
    snprintf(s_name, sizeof(s_name), "GT911 @0x%02X %ux%u", s_addr, s_resX, s_resY);
    Serial.printf("[touch] %s id=%c%c%c%c\n", s_name, id[0], id[1], id[2], id[3]);
  } else {
    snprintf(s_name, sizeof(s_name), "GT911 absent");
    Serial.println("[touch] GT911 introuvable (I2C 19/20)");
  }
  lv_indev_drv_init(&s_indev);
  s_indev.type = LV_INDEV_TYPE_POINTER;
  s_indev.read_cb = readCb;
  lv_indev_drv_register(&s_indev);
}

#else  // ── XPT2046 (SPI partagé avec la microSD) ────────────────────────
static SPIClass s_spi(HSPI);
static const SPISettings XPT_SPI(2000000, MSBFIRST, SPI_MODE0);

bool touchIsResistive() { return true; }
void touchDefaultCal(int16_t & x0, int16_t & x1, int16_t & y0, int16_t & y1, bool & swap) {
  swap = XPT_SWAP_XY;
  x0 = XPT_INVERT_X ? XPT_X_MAX : XPT_X_MIN; x1 = XPT_INVERT_X ? XPT_X_MIN : XPT_X_MAX;
  y0 = XPT_INVERT_Y ? XPT_Y_MAX : XPT_Y_MIN; y1 = XPT_INVERT_Y ? XPT_Y_MIN : XPT_Y_MAX;
}

static uint16_t xptCmd(uint8_t cmd) {
  s_spi.transfer(cmd);
  return s_spi.transfer16(0) >> 3;
}

static void readCb(lv_indev_drv_t *, lv_indev_data_t * d) {
  s_spi.beginTransaction(XPT_SPI);
  digitalWrite(XPT_CS, LOW);
  int z1 = xptCmd(0xB1), z2 = xptCmd(0xC1);
  int z = z1 + 4095 - z2;
  bool pressed = z > XPT_Z_THRESHOLD;
  int rx = 0, ry = 0;
  if (pressed) {
    xptCmd(0xD1);                           // première conversion jetée (bruit)
    for (int i = 0; i < 4; i++) { rx += xptCmd(0xD1); ry += xptCmd(0x91); }
    rx /= 4; ry /= 4;
  }
  xptCmd(0x90);                             // retour en veille, IRQ actif
  digitalWrite(XPT_CS, HIGH);
  s_spi.endTransaction();
  report(pressed, rx, ry, d);
}

void touchBegin() {
  pinMode(XPT_CS, OUTPUT);
  digitalWrite(XPT_CS, HIGH);
  pinMode(XPT_IRQ, INPUT);
  s_spi.begin(XPT_SCLK, XPT_MISO, XPT_MOSI, XPT_CS);
  snprintf(s_name, sizeof(s_name), "XPT2046 (r\xC3\xA9sistif)");
  Serial.println("[touch] XPT2046");
  lv_indev_drv_init(&s_indev);
  s_indev.type = LV_INDEV_TYPE_POINTER;
  s_indev.read_cb = readCb;
  lv_indev_drv_register(&s_indev);
}
#endif

} // namespace hal
#endif // ARDUINO
