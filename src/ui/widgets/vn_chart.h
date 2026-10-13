// ── Graphique sur mesure (aire en dégradé, courbe, pointillés, barres) ─
// Dessiné directement (LV_EVENT_DRAW_MAIN) : remplissage en fondu sous la
// courbe, prévision en pointillés, repère « maintenant » lumineux —
// ce que lv_chart (8.3) ne sait pas faire proprement.
#pragma once
#include "lvgl.h"
#include <math.h>

namespace ui {

enum SeriesKind : uint8_t { SK_AREA, SK_LINE, SK_DOTS, SK_BARS };

struct Series {
  const float * y = nullptr;     // NAN = trou
  int        n = 0;
  int        x0 = 0;             // le point i est à l'abscisse x0 + i
  lv_color_t color = lv_color_white();
  uint8_t    width = 2;
  SeriesKind kind = SK_LINE;
  lv_opa_t   opa = LV_OPA_COVER;
  lv_opa_t   fillOpa = 90;       // aire : opacité en haut du dégradé
  const lv_color_t * barColors = nullptr;
};

struct ChartData {
  int    nx = 2;                 // taille du domaine X (points)
  float  ymin = 0, ymax = 1;
  Series s[4];
  int    ns = 0;
  int    nowX = -1;              // repère vertical « maintenant »
  float  hline = NAN;            // ligne horizontale (capacité…)
  bool   gridY = true;
  bool   endDot = true;          // point lumineux en fin de série 0
  lv_coord_t padL = 24, padR = 4, padT = 4, padB = 12;
  char   ylab[2][10] = { "", "" };   // haut / bas
  int    nxl = 0;
  int    xlPos[8] = {};
  char   xl[8][8] = {};
};

lv_obj_t *  chartCreate(lv_obj_t * parent, lv_coord_t w, lv_coord_t h);
ChartData * chartData(lv_obj_t * chart);
void        chartInvalidate(lv_obj_t * chart);

} // namespace ui
