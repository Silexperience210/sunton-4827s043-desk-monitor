#include "settings.h"
#include "../platform/platform.h"
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"      // facultatif : VN_WIFI_SSID / VN_WIFI_PASS par défaut
#endif

namespace vn {

static const uint32_t SET_MAGIC = 0x564E4F50;   // "VNOP"
static const uint16_t SET_VER   = 1;
static const char *   SET_PATH  = "/settings.bin";

Settings g_set;

void settingsDefaults(Settings & s) {
  memset(&s, 0, sizeof(s));
  s.magic = SET_MAGIC;
  s.version = SET_VER;
#ifdef VN_WIFI_SSID
  strncpy(s.ssid, VN_WIFI_SSID, sizeof(s.ssid) - 1);
#endif
#ifdef VN_WIFI_PASS
  strncpy(s.pass, VN_WIFI_PASS, sizeof(s.pass) - 1);
#endif
  s.refreshSec  = 60;
  s.lowBikes    = 2;
  s.lowDocks    = 2;
  s.maintBikes  = 4;
  s.maintDocks  = 2;
  s.frozenHours = 4;
  s.brightness  = 100;
  s.nightDim    = 1;
  s.alertToasts = 1;
}

bool settingsLoad() {
  Settings tmp;
  if (plat::fsSize(SET_PATH) == (long)sizeof(Settings) && plat::fsRead(SET_PATH, &tmp, sizeof(tmp))
      && tmp.magic == SET_MAGIC && tmp.version == SET_VER) {
    tmp.ssid[sizeof(tmp.ssid) - 1] = 0;
    tmp.pass[sizeof(tmp.pass) - 1] = 0;
    if (tmp.refreshSec < 30) tmp.refreshSec = 30;
    if (tmp.brightness < 5) tmp.brightness = 5;
    g_set = tmp;
    return true;
  }
  settingsDefaults(g_set);
  return false;
}

uint8_t effectiveBrightness(int64_t now) {
  uint8_t b = g_set.brightness < 5 ? 5 : g_set.brightness;
  if (g_set.nightDim && now > 0) {
    struct tm lt;
    plat::localTime(now, &lt);
    if ((lt.tm_hour >= 22 || lt.tm_hour < 6) && b > 20) b = 20;
  }
  return b;
}

bool settingsSave() {
  return plat::fsWrite(SET_PATH, &g_set, sizeof(g_set));
}

} // namespace vn
