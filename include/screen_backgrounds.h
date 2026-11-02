#pragma once

/** Register the background files and let EEZ flows use "S:/..." SD paths as image sources. Call right after ui_init(), before the first ui_tick() and sd_service_start(). */
void screen_backgrounds_init();
/** Show background files as the SD service finishes decoding them into PSRAM. Call from the LVGL task. */
void screen_backgrounds_tick();
