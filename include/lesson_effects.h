#pragma once

#include <lvgl.h>
#include <stddef.h>

// Look and feel of the lesson: a progress bar, steps that build up piece by piece, answer tiles
// that react when pressed, and a red flash for a wrong answer.

/** Create the progress bar on the lesson screen and the tiles' pressed look. Call once. */
void lesson_effects_init(lv_obj_t *screen, lv_obj_t *const *const *tiles, size_t tile_count);
/** Show the progress bar at percent (0-100), animated; hide it with visible false. */
void lesson_effects_progress(int32_t percent, bool visible);
/** Let the pictures of a step that just appeared come in one by one; tiles come last.
 * Ends the previous step's animations. nullptr only ends them. */
void lesson_effects_reveal(lv_obj_t *panel);
/** A short red flash on a wrong tile. */
void lesson_effects_wrong(lv_obj_t *tile);
