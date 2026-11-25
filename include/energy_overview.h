#pragma once

/** Keep the energy overview on the current date and time: the date next to the greeting, and in
 * the battery chart "now" with the clock and the hours around it on the axis. Call from the LVGL
 * task, after ui_init(). */
void energy_overview_init();
