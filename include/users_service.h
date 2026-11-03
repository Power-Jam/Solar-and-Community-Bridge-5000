#pragma once

/** Load the saved learners, show the selected one on every screen and hook up the learner widgets. Call after ui_init() and screen_backgrounds_init(), before the first ui_tick(). */
void users_init();
/** Keep the selected learner's coins and unlocked level saved, and refresh avatars as they finish loading. Call from the LVGL task, after ui_tick(). */
void users_tick();
