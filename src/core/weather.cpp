#include "weather.h"
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace vn {

struct WxGroup { int codes[6]; WxIcon icon; const char * label; };
static const WxGroup GROUPS[] = {
  { { 0, -1 },                    WX_SUN,    "Ciel clair" },
  { { 1, 2, -1 },                 WX_PARTLY, "Peu nuageux" },
  { { 3, -1 },                    WX_CLOUD,  "Couvert" },
  { { 45, 48, -1 },               WX_FOG,    "Brouillard" },
  { { 51, 53, 55, -1 },           WX_RAIN,   "Bruine" },
  { { 61, 63, 80, 81, -1 },       WX_RAIN,   "Pluie" },
  { { 65, 82, -1 },               WX_RAIN,   "Forte pluie" },
  { { 71, 73, 75, 77, 85, 86 },   WX_SNOW,   "Neige" },
  { { 95, 96, 99, -1 },           WX_STORM,  "Orage" },
};

static const WxGroup * group(int code) {
  for (const auto & g : GROUPS)
    for (int c : g.codes) { if (c < 0) break; if (c == code) return &g; }
  return nullptr;
}
const char * wxLabel(int code) { const WxGroup * g = group(code); return g ? g->label : "Météo"; }
WxIcon wxIcon(int code) { const WxGroup * g = group(code); return g ? g->icon : WX_CLOUD; }

bool wxParse(const char * buf, size_t len, Weather & w) {
  JsonDocument doc;
  if (deserializeJson(doc, buf, len)) return false;
  JsonObjectConst c = doc["current"];
  if (c.isNull()) return false;
  w.temp    = c["temperature_2m"] | 0.0f;
  w.rain    = c["precipitation"] | 0.0f;
  w.wind    = c["wind_speed_10m"] | 0.0f;
  w.windDir = (int)lroundf(c["wind_direction_10m"] | 0.0f);
  w.code    = c["weather_code"] | 0;
  JsonObjectConst h = doc["hourly"];
  JsonArrayConst t = h["temperature_2m"], pr = h["precipitation_probability"], rr = h["precipitation"],
                 wc = h["weather_code"], ws = h["wind_speed_10m"];
  w.nfc = 0;
  for (int i = 0; i < 6 && i < (int)t.size(); i++) {
    WxHour & f = w.fc[w.nfc++];
    f.h    = (int8_t)i;           // index 0 = heure en cours
    f.temp = t[i] | 0.0f;
    f.prob = pr[i].is<int>() ? (int16_t)pr[i].as<int>() : (int16_t)-1;
    f.rain = rr[i] | 0.0f;
    f.code = (int16_t)(wc[i] | 0);
    f.wind = ws[i] | 0.0f;
  }
  w.valid = true;
  wxScore(w);
  return true;
}

// ── Note 0–10 (cf. bikeScoreDetail) ──────────────────────────────────
static float rainPenalty(float mm) { return mm > 0 ? fminf(5.f, 2.f + 3.f * mm) : 0.f; }
static float windPenalty(float kmh) { return kmh > 20 ? fminf(3.f, (kmh - 20.f) / 10.f) : 0.f; }
static float fcWeight(int h) { return h == 1 ? 1.f : h == 2 ? 0.6f : h == 3 ? 0.35f : 0.f; }
static const float EASING_SHARE = 0.3f;
static float expectedRain(const WxHour & f) {
  float p = f.prob >= 0 ? f.prob : (f.rain > 0 ? 100.f : 0.f);
  if (p < 20) return 0;
  return (p / 100.f) * rainPenalty(fmaxf(f.rain, 0.1f));
}

static float coldPenalty(float t) { return t < 2 ? (t < -2 ? 4.f : 2.f) : 0.f; }
static float codePenalty(int code) { return (code >= 95 ? 4.f : 0.f) + (code >= 71 && code <= 86 ? 3.f : 0.f); }

float wxPenalty(float temp, float rainMm, float windKmh, int code) {
  float p = rainPenalty(rainMm) + windPenalty(windKmh) + coldPenalty(temp) + codePenalty(code);
  return fminf(1.f, p / 10.f);
}

float wxPenaltyHour(const WxHour & f) {
  float p = expectedRain(f) + windPenalty(f.wind) + coldPenalty(f.temp) + codePenalty(f.code);
  return fminf(1.f, p / 10.f);
}

void wxScore(Weather & w) {
  struct R { float pts; char txt[56]; } rs[8];
  int n = 0;
  auto add = [&](float pts, const char * fmt, float a = 0, float b = 0) {
    if (fabsf(pts) < 0.05f || n >= 8) return;
    rs[n].pts = pts;
    snprintf(rs[n].txt, sizeof(rs[n].txt), fmt, a, b);
    n++;
  };
  float rainNow = rainPenalty(w.rain), windNow = windPenalty(w.wind);
  add(-rainNow, "Pluie %.1f mm/h", w.rain);
  add(-windNow, "Vent %.0f km/h", w.wind);
  if (w.temp < 2) add(-coldPenalty(w.temp), w.temp < -2 ? "Gel %.0f\xC2\xB0" : "Froid %.0f\xC2\xB0", w.temp);
  if (w.code >= 95) add(-4.f, "Orage");
  if (w.code >= 71 && w.code <= 86) add(-3.f, "Neige");

  // Tendance pluie / vent sur +1…+3 h : l'aggravation compte en entier, l'accalmie à 30 %
  auto trend = [&](float now, float (*pen)(const WxHour &), bool isRain) {
    float worst = 0; int wh = 0; const WxHour * wf = nullptr;
    for (int i = 0; i < w.nfc; i++) {
      float wt = fcWeight(w.fc[i].h);
      if (wt <= 0) continue;
      float ex = wt * fmaxf(0.f, pen(w.fc[i]) - now);
      if (ex > worst) { worst = ex; wh = w.fc[i].h; wf = &w.fc[i]; }
    }
    if (wf) {
      if (isRain) add(-worst, now > 0 ? "Pluie qui se renforce (+%.0f h)" : "Pluie probable (+%.0f h, %.0f %%)", (float)wh, (float)wf->prob);
      else add(-worst, "Vent en hausse (+%.0f h, %.0f km/h)", (float)wh, wf->wind);
      return;
    }
    if (now > 0) {
      float ahead = 0; int cnt = 0;
      for (int i = 0; i < w.nfc; i++) if (fcWeight(w.fc[i].h) > 0) { ahead = fmaxf(ahead, pen(w.fc[i])); cnt++; }
      float relief = EASING_SHARE * (now - ahead);
      if (cnt && relief > 0) add(relief, isRain ? "Accalmie attendue" : "Vent en baisse");
    }
  };
  trend(rainNow, [](const WxHour & f) { return expectedRain(f); }, true);
  trend(windNow, [](const WxHour & f) { return windPenalty(f.wind); }, false);

  float total = 10;
  int worstIdx = -1;
  for (int i = 0; i < n; i++) {
    total += rs[i].pts;
    if (rs[i].pts < 0 && (worstIdx < 0 || rs[i].pts < rs[worstIdx].pts)) worstIdx = i;
  }
  w.score = fmaxf(0.f, fminf(10.f, roundf(total * 10) / 10));
  if (worstIdx >= 0) strncpy(w.reason, rs[worstIdx].txt, sizeof(w.reason) - 1);
  else snprintf(w.reason, sizeof(w.reason), "Conditions idéales");
  w.reason[sizeof(w.reason) - 1] = 0;
}

} // namespace vn
