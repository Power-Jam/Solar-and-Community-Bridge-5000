#pragma once

#include <lvgl.h>

/** Initialize the LCD hardware and create its LVGL display. */
lv_display_t *display_init();

/** Dim the backlight after a minute without touch, and restore it on the next touch. */
void display_backlight_tick(lv_display_t *display);

/** Restore full brightness when dimmed; returns true if it was dimmed, so the touch can be ignored. */
bool display_backlight_wake(lv_display_t *display);
