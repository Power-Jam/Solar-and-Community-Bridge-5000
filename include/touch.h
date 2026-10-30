#pragma once

#include <lvgl.h>

/** Reset and verify the GT911 touch controller over the shared I2C bus. */
bool touch_init(bool i2c_available);
/** Read the latest touch point; pressed is false when there is no contact. */
bool touch_read_state(int16_t & x, int16_t & y, bool & pressed);
/** Register the touch input and home-screen navigation callbacks with LVGL. */
void touch_setup_lvgl(lv_display_t * display);
