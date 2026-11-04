#pragma once

/** The logo on the splash screen; loaded into PSRAM before every other picture. */
#define SPLASH_LOGO_PATH "S:/images/Just-jam_logo_strawberry.jpg"

/** Show the start-up screen (logo, device name, loading bar) on top of the current screen. Call after the other screens are created. */
void splash_screen_init();
/** Advance the loading bar and return to the previous screen when it is full. Call from the LVGL task. */
void splash_screen_tick();
