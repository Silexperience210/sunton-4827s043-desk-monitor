#pragma once
#include "lvgl.h"

namespace hal {
lv_disp_t * displayBegin();     // dalle RGB + tampons + pilote LVGL
void        touchBegin();       // GT911 (VN_TOUCH_GT911) ou XPT2046 (VN_TOUCH_XPT2046)
}
