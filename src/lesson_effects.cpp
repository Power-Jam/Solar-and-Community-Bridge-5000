#include "lesson_effects.h"

#include <algorithm>

namespace
{
const lv_color_t Green = lv_color_hex(0x2F6B45);
const lv_color_t Track = lv_color_hex(0xE4DCC8);
const lv_color_t Wrong = lv_color_hex(0xD64545);

constexpr int32_t BarX = 62;      // lines up with the lesson cards (x 62, width 900)
constexpr int32_t BarY = 84;      // between the nav bar (height 66) and the cards (y 116)
constexpr int32_t BarWidth = 900;
constexpr int32_t BarHeight = 14;

constexpr uint32_t CardFadeMs = 180;
constexpr uint32_t PieceMs = 320;
constexpr uint32_t PieceGapMs = 90;   // between one picture and the next
constexpr int32_t PieceRise = 18;     // px a picture rises while it fades in
constexpr size_t MaxPieces = 16;

lv_obj_t *bar = nullptr;
lv_obj_t *revealed = nullptr;
lv_obj_t *const *const *tile_list = nullptr;
size_t tile_total = 0;
lv_style_t pressed_style;

void set_opa(void *object, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(value), 0);
}

void set_rise(void *object, int32_t value)
{
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(object), value, 0);
}

void set_tint(void *object, int32_t value)
{
    lv_obj_set_style_image_recolor_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(value), 0);
}

bool is_tile(const lv_obj_t *object)
{
    for (size_t i = 0; i < tile_total; ++i) {
        if (*tile_list[i] == object) {
            return true;
        }
    }
    return false;
}

void animate(lv_obj_t *object, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t duration,
             uint32_t delay, lv_anim_path_cb_t path)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, object);
    lv_anim_set_exec_cb(&anim, exec);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, path);
    lv_anim_start(&anim);
}

void settle(lv_obj_t *panel)
{
    const uint32_t count = lv_obj_get_child_count(panel);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, static_cast<int32_t>(i));
        lv_anim_delete(child, set_opa);
        lv_anim_delete(child, set_rise);
        set_opa(child, LV_OPA_COVER);
        set_rise(child, 0);
    }
}

// Order in which a step builds up: the title, then the pictures left to right (so a sum reads
// "solar panel + sun -> battery"), then the tiles to answer with.
int rank(lv_obj_t *child)
{
    if (is_tile(child)) {
        return 2;
    }
    return lv_obj_get_y(child) < 240 ? 0 : 1;
}
}

void lesson_effects_init(lv_obj_t *screen, lv_obj_t *const *const *tiles, size_t tile_count)
{
    tile_list = tiles;
    tile_total = tile_count;

    bar = lv_bar_create(screen);
    lv_obj_set_pos(bar, BarX, BarY);
    lv_obj_set_size(bar, BarWidth, BarHeight);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, Track, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, Green, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 500, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    // A pressed tile darkens a little, so the learner sees the tap land.
    lv_style_init(&pressed_style);
    lv_style_set_image_recolor(&pressed_style, lv_color_hex(0x18352A));
    lv_style_set_image_recolor_opa(&pressed_style, LV_OPA_20);
    for (size_t i = 0; i < tile_count; ++i) {
        lv_obj_add_style(*tiles[i], &pressed_style, LV_STATE_PRESSED);
    }
}

void lesson_effects_progress(int32_t percent, bool visible)
{
    if (!bar) {
        return;
    }
    if (!visible) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const bool was_hidden = lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bar);
    // Coming back to the lesson starts from the value it has, without filling up from the last run.
    lv_bar_set_value(bar, LV_CLAMP(0, percent, 100), was_hidden ? LV_ANIM_OFF : LV_ANIM_ON);
}

void lesson_effects_reveal(lv_obj_t *panel)
{
    if (revealed) {
        settle(revealed);
    }
    revealed = panel;
    if (!panel) {
        return;
    }

    lv_obj_t *pieces[MaxPieces];
    size_t count = 0;
    lv_obj_t *card = nullptr;
    const uint32_t children = lv_obj_get_child_count(panel);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (!lv_obj_check_type(child, &lv_image_class)) {
            card = card ? card : child;  // the step's card(s) fade in first, all at once
            set_opa(child, LV_OPA_TRANSP);
            animate(child, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, CardFadeMs, 0, lv_anim_path_ease_out);
            continue;
        }
        if (count < MaxPieces) {
            pieces[count++] = child;
        }
    }
    std::stable_sort(pieces, pieces + count, [](lv_obj_t *a, lv_obj_t *b) {
        const int ra = rank(a);
        const int rb = rank(b);
        return ra != rb ? ra < rb : lv_obj_get_x(a) < lv_obj_get_x(b);
    });

    uint32_t delay = card ? CardFadeMs / 2 : 0;
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *piece = pieces[i];
        set_opa(piece, LV_OPA_TRANSP);
        set_rise(piece, PieceRise);
        animate(piece, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, PieceMs, delay, lv_anim_path_ease_out);
        animate(piece, set_rise, PieceRise, 0, PieceMs, delay, lv_anim_path_overshoot);
        delay += PieceGapMs;
    }
}

void lesson_effects_wrong(lv_obj_t *tile)
{
    lv_anim_delete(tile, set_tint);
    lv_obj_set_style_image_recolor(tile, Wrong, 0);
    set_tint(tile, LV_OPA_60);
    animate(tile, set_tint, LV_OPA_60, LV_OPA_TRANSP, 500, 120, lv_anim_path_ease_in);
}
