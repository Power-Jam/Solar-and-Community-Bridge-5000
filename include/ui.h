#ifndef EEZ_LVGL_UI_GUI_H
#define EEZ_LVGL_UI_GUI_H

#include <lvgl.h>

#include "eez-flow.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t assets[8132];

void ui_init();
void ui_tick();
void landing_screen_init();
void landing_screen_tick();

#ifdef __cplusplus
}
#endif

#endif // EEZ_LVGL_UI_GUI_H