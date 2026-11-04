#pragma once

/** Hook up the Level 1 lesson: its answer tiles, the KEEP UP popup and the Next button. Call after ui_init(). */
void lesson_init();
/** Fade the Next button in after every step. Call from the LVGL task, after ui_tick(). */
void lesson_tick();
