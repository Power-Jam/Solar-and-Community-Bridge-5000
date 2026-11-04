#pragma once

#include <stdint.h>

/** Load the saved learners, show the selected one on every screen and hook up the learner widgets. Call after ui_init() and screen_backgrounds_init(), before the first ui_tick(). */
void users_init();
/** Keep the selected learner's coins and unlocked level saved, and refresh avatars as they finish loading. Call from the LVGL task, after ui_tick(). */
void users_tick();
/** The selected learner's avatar, 1..9 as numbered in the user creation grid; 0 without a learner. */
uint8_t users_current_avatar();
/** Shop items the selected learner owns and wears (bit n = item n + 1); 0 without a learner. */
uint16_t users_owned_items();
uint16_t users_worn_items();
/** Store the selected learner's shop items and save them. */
void users_set_items(uint16_t owned, uint16_t worn);
