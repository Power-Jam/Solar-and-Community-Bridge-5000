#pragma once

/** Let EEZ flows use "S:/..." SD paths as image sources. Call right after ui_init(), before the first ui_tick(). */
void screen_backgrounds_init();
/** Point the EEZ background widgets at their SD files whenever the card (re)mounts. Call from the LVGL task. */
void screen_backgrounds_tick();
