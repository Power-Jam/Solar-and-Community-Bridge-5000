#pragma once

#include <stdint.h>

/** Register the background files and let EEZ flows use "S:/..." SD paths as image sources. Call right after ui_init(), before the first ui_tick() and sd_service_start(). */
void screen_backgrounds_init();
/** Show background files as the SD service finishes decoding them into PSRAM. Call from the LVGL task. */
void screen_backgrounds_tick();
/** The PSRAM copy of an "S:/..." file from the load list, or nullptr while it is not loaded yet. */
const void *screen_backgrounds_image(const char *path);
/** Changes every time a file finished loading, so callers know when to ask screen_backgrounds_image() again. */
uint32_t screen_backgrounds_version();
