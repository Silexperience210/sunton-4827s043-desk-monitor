// ── Étalonnage tactile : conversion brut → écran et résolution 4 points ─
#include "touch_api.h"
#include <stdlib.h>
#include <math.h>

namespace hal {

static long mapClamp(long v, long a, long b, long lo, long hi) {
  if (a == b) return lo;
  long r = lo + (v - a) * (hi - lo) / (b - a);
  return r < lo ? lo : r > hi ? hi : r;
}

static void apply(const vn::TouchCal & c, int rx, int ry, int16_t & x, int16_t & y) {
  if (c.swap) { int t = rx; rx = ry; ry = t; }
  x = (int16_t)mapClamp(rx, c.x0, c.x1, 0, 479);
  y = (int16_t)mapClamp(ry, c.y0, c.y1, 0, 271);
}

void touchToScreen(int rx, int ry, int16_t & x, int16_t & y) {
  vn::TouchCal c = vn::g_set.tcal;
  if (!c.valid) {
    bool sw;
    touchDefaultCal(c.x0, c.x1, c.y0, c.y1, sw);
    c.swap = sw;
  }
  apply(c, rx, ry, x, y);
}

int touchCalError(const vn::TouchCal & c, int rx, int ry, int tx, int ty) {
  int16_t x, y;
  apply(c, rx, ry, x, y);
  return (int)lroundf(sqrtf((float)((x - tx) * (x - tx) + (y - ty) * (y - ty))));
}

bool touchSolveCal(const int16_t rx[4], const int16_t ry[4], const int16_t tx[4], const int16_t ty[4],
                   vn::TouchCal & out) {
  // Axes échangés ? Entre les deux cibles du haut, seul X écran varie.
  int dxr = abs(rx[1] - rx[0]), dyr = abs(ry[1] - ry[0]);
  bool swap = dyr > dxr;
  float u[4], v[4];
  for (int i = 0; i < 4; i++) { u[i] = swap ? ry[i] : rx[i]; v[i] = swap ? rx[i] : ry[i]; }
  // X : moyenne gauche (0, 3) / droite (1, 2) ; Y : haut (0, 1) / bas (2, 3)
  float uL = (u[0] + u[3]) / 2, uR = (u[1] + u[2]) / 2;
  float vT = (v[0] + v[1]) / 2, vB = (v[2] + v[3]) / 2;
  float xL = (tx[0] + tx[3]) / 2.f, xR = (tx[1] + tx[2]) / 2.f;
  float yT = (ty[0] + ty[1]) / 2.f, yB = (ty[2] + ty[3]) / 2.f;
  if (fabsf(uR - uL) < 20 || fabsf(vB - vT) < 20 || xR - xL < 50 || yB - yT < 50) return false;
  float ku = (uR - uL) / (xR - xL), kv = (vB - vT) / (yB - yT);   // brut par pixel
  float x0 = uL + (0 - xL) * ku, x1 = uL + (479 - xL) * ku;
  float y0 = vT + (0 - yT) * kv, y1 = vT + (271 - yT) * kv;
  if (fabsf(x0) > 30000 || fabsf(x1) > 30000 || fabsf(y0) > 30000 || fabsf(y1) > 30000) return false;
  out.valid = 1;
  out.swap = swap ? 1 : 0;
  out.x0 = (int16_t)lroundf(x0); out.x1 = (int16_t)lroundf(x1);
  out.y0 = (int16_t)lroundf(y0); out.y1 = (int16_t)lroundf(y1);
  return true;
}

} // namespace hal
