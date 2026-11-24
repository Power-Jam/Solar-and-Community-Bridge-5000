#pragma once

/** Draw the battery icon and the real charge (from the INA260 battery gauge) in the battery
 * buttons of every nav bar, and keep them up to date. Call from the LVGL task, after ui_init(). */
void battery_indicator_init();
