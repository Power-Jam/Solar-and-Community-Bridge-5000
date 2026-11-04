#include "lesson_service.h"

#include "eez-flow.h"
#include "screen_backgrounds.h"
#include "screens.h"
#include "vars.h"

#include <lvgl.h>

#include <cstdio>
#include <cstring>

// Level 1 follows the LevelDesign Figma file. EEZ shows the step in currentPanel (and the story
// in lessonChoice); this file runs what the flows can't: the questions and the Next button's fade.
namespace
{
constexpr int32_t LessonLevel = 1;
constexpr int32_t SunQuestion = 1;      // "What do you need to charge a battery?"
// "What would you use your battery charge for?": every tile is right and picks the story
// (lessonChoice). It goes straight on to step 7, without KEEP UP, whose last tile shows the pick;
// the story starts at step 8.
// After every story the learner comes back to the question; a story that is done is no longer
// shown, so it can't be picked again. Once all three are done there is no button: after a moment
// a reward overlay with the coins and confetti shows, and when it closes the level is completed.
constexpr int32_t CloudQuestion = 6;
constexpr int32_t CloudAnswer = 7;
constexpr int32_t LastStoryStep = 11;
constexpr uint8_t AllStories = 0b111;
constexpr int32_t RewardCoins = 10;      // what onCompleteClick in the EEZ flow adds
constexpr uint32_t RewardDelayMs = 500; // on the empty question before the overlay shows
constexpr uint32_t RewardShowMs = 4000;  // before the overlay closes by itself (a tap closes it sooner)
constexpr uint32_t ConfettiCount = 40;
// The last tile of step 7 for lessonChoice 1 (battery), 2 (lightbulb) and 3 (house).
const char *const PickPictures[] = {
    "S:/assets/lesson1/battery-full-tile.jpg",
    "S:/assets/lesson1/lightbulb-tile.jpg",
    "S:/assets/lesson1/house-tile-3.jpg",
};
// Steps that move on by themselves, so the run plays as one flow with Next only on its last step:
// the charging run (steps 3-5). The stories (steps 8-11) use Next on every step.
struct AutoRun {
    int32_t first;
    int32_t last;  // the step that gets Next
};
constexpr AutoRun AutoRuns[] = {{3, 5}};
constexpr uint32_t AutoStepMs = 1800;
constexpr uint32_t KeepUpMs = 1400;
constexpr uint32_t NextDelayMs = 800;   // as in Figma: Next appears after 0.8 s, in 0.3 s
constexpr uint32_t NextFadeMs = 300;

enum class Answer : uint8_t { Right, Wrong };

struct Tile {
    lv_obj_t *const *object;
    int32_t step;
    Answer answer;
    int32_t story;  // lessonChoice a right answer sets (1 battery, 2 lamp, 3 house); 0 keeps it
};

const Tile tiles[] = {
    {&objects.lesson_q1_sun, SunQuestion, Answer::Right, 0},
    {&objects.lesson_q1_lamp, SunQuestion, Answer::Wrong, 0},
    {&objects.lesson_q1_house, SunQuestion, Answer::Wrong, 0},
    {&objects.lesson_q2_battery, CloudQuestion, Answer::Right, 1},
    {&objects.lesson_q2_lamp, CloudQuestion, Answer::Right, 2},
    {&objects.lesson_q2_house, CloudQuestion, Answer::Right, 3},
};

lv_timer_t *keep_up_timer = nullptr;
uint8_t done_stories = 0;  // bit n: story n + 1 picked in this run of the level
lv_timer_t *reward_timer = nullptr;
lv_obj_t *reward_overlay = nullptr;
bool completing = false;
lv_timer_t *auto_timer = nullptr;
int32_t shown_step = -1;
int32_t shown_level = -1;
bool on_level_screen = false;

int32_t flow_int(uint32_t index)
{
    return eez::flow::getGlobalVariable(index).getInt32();
}

void set_flow_int(uint32_t index, int32_t value)
{
    eez::flow::setGlobalVariable(index, eez::Value(static_cast<int>(value)));
}

lv_obj_t *next_button()
{
    return objects.level1_next__lvl1_complete_level_button;
}

bool is_question(int32_t step)
{
    return step == SunQuestion || step == CloudQuestion;
}

bool is_auto(int32_t step)
{
    for (const AutoRun &run : AutoRuns) {
        if (step >= run.first && step < run.last) {
            return true;
        }
    }
    return false;
}

void stop_auto()
{
    if (auto_timer) {
        lv_timer_delete(auto_timer);
        auto_timer = nullptr;
    }
}

void auto_done(lv_timer_t *timer)
{
    const int32_t step = static_cast<int32_t>(reinterpret_cast<intptr_t>(lv_timer_get_user_data(timer)));
    auto_timer = nullptr;  // a one-shot timer deletes itself
    if (flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL) == step) {
        set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, step + 1);
    }
}

void start_auto(int32_t step)
{
    stop_auto();
    auto_timer = lv_timer_create(auto_done, AutoStepMs, reinterpret_cast<void *>(static_cast<intptr_t>(step)));
    lv_timer_set_repeat_count(auto_timer, 1);
}

void set_opa(void *object, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(value), 0);
}

void next_faded_in(lv_anim_t *anim)
{
    lv_obj_add_flag(static_cast<lv_obj_t *>(anim->var), LV_OBJ_FLAG_CLICKABLE);
}

// Questions are answered with their tiles; every other step gets a Next that only works once it shows.
void show_next(bool question)
{
    lv_obj_t *button = next_button();
    lv_anim_delete(button, set_opa);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    set_opa(button, LV_OPA_TRANSP);
    if (question) {
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, button);
    lv_anim_set_exec_cb(&anim, set_opa);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&anim, NextDelayMs);
    lv_anim_set_duration(&anim, NextFadeMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, next_faded_in);
    lv_anim_start(&anim);
}

void hide_keep_up()
{
    if (keep_up_timer) {
        lv_timer_delete(keep_up_timer);
        keep_up_timer = nullptr;
    }
    lv_anim_delete(objects.lesson_keep_up, set_opa);
    lv_obj_add_flag(objects.lesson_keep_up, LV_OBJ_FLAG_HIDDEN);
}

void keep_up_done(lv_timer_t *)
{
    const int32_t step = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL);
    hide_keep_up();
    set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, step + 1);
}

// "KEEP UP!" for a right answer, then on to the step that shows the answer.
void show_keep_up()
{
    int16_t x = 0;
    int16_t y = 0;
    const void *image = screen_backgrounds_layer("S:/assets/lesson1/keep-up.lay", &x, &y);
    if (image) {
        lv_image_set_src(objects.lesson_keep_up, image);
        lv_obj_set_pos(objects.lesson_keep_up, x, y);
        lv_obj_remove_flag(objects.lesson_keep_up, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(objects.lesson_keep_up);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, objects.lesson_keep_up);
        lv_anim_set_exec_cb(&anim, set_opa);
        lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&anim, 200);
        lv_anim_start(&anim);
    }
    keep_up_timer = lv_timer_create(keep_up_done, image ? KeepUpMs : 0, nullptr);
    lv_timer_set_repeat_count(keep_up_timer, 1);
}

void set_translate_x(void *object, int32_t value)
{
    lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(object), value, 0);
}

// The offset itself (a path returns the animated value): 1.5 swings of 8 px that die out.
int32_t shake_path(const lv_anim_t *anim)
{
    const int32_t time = LV_CLAMP(0, anim->act_time, anim->duration);
    const int32_t phase = (time * 540) / anim->duration;
    const int32_t fade = 1024 - (time * 1024) / anim->duration;
    return (lv_trigo_sin(static_cast<int16_t>(phase)) * 8 * fade) / (LV_TRIGO_SIN_MAX * 1024);
}

// A wrong tile shakes "no" and the question stays.
void shake(lv_obj_t *tile)
{
    lv_anim_delete(tile, set_translate_x);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tile);
    lv_anim_set_exec_cb(&anim, set_translate_x);
    lv_anim_set_values(&anim, 0, 0);
    lv_anim_set_duration(&anim, 420);
    lv_anim_set_path_cb(&anim, shake_path);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t *a) { set_translate_x(a->var, 0); });
    lv_anim_start(&anim);
}

void on_tile_clicked(lv_event_t *event)
{
    const Tile &tile = *static_cast<const Tile *>(lv_event_get_user_data(event));
    if (keep_up_timer || flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL) != tile.step) {
        return;
    }
    if (tile.answer == Answer::Wrong) {
        shake(*tile.object);
        return;
    }
    if (tile.story) {
        const uint8_t bit = static_cast<uint8_t>(1u << (tile.story - 1));
        if (done_stories & bit) {
            return;
        }
        done_stories |= bit;
        set_flow_int(FLOW_GLOBAL_VARIABLE_LESSON_CHOICE, tile.story);
        set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, tile.step + 1);
        return;
    }
    show_keep_up();
}

bool stories_left()
{
    return done_stories != AllStories;
}

// Hides the stories already done on the question.
void show_done_stories()
{
    for (const Tile &tile : tiles) {
        if (tile.step == CloudQuestion && tile.story) {
            const bool done = done_stories & (1u << (tile.story - 1));
            if (done) {
                lv_obj_add_flag(*tile.object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(*tile.object, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

bool all_done_on_question(int32_t step)
{
    return step == CloudQuestion && !stories_left();
}

// The flow's Next runs after this handler (in the next flow tick): it completes the level on
// LastStoryStep and adds 1 to currentPanel on every other step. So the end of a story is moved
// to the step before the question, and the reward's click on the question to LastStoryStep.
void on_next_clicked(lv_event_t *)
{
    if (flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_LEVEL) != LessonLevel) {
        return;
    }
    const int32_t step = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL);
    if (step == LastStoryStep) {
        set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, CloudQuestion - 1);
    } else if (all_done_on_question(step)) {
        set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, LastStoryStep);
    }
}

void stop_reward()
{
    if (reward_timer) {
        lv_timer_delete(reward_timer);
        reward_timer = nullptr;
    }
    if (reward_overlay) {
        lv_obj_delete(reward_overlay);  // also ends the animations on it and its children
        reward_overlay = nullptr;
    }
}

// Closes the overlay and completes the level, as a tap on the (hidden) Complete Level button would.
void finish_reward()
{
    if (completing) {
        return;
    }
    completing = true;
    stop_reward();
    lv_obj_send_event(next_button(), LV_EVENT_SINGLE_CLICKED, nullptr);
    lv_obj_send_event(next_button(), LV_EVENT_CLICKED, nullptr);
}

void set_y(void *object, int32_t value)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
}

void set_width(void *object, int32_t value)
{
    lv_obj_set_width(static_cast<lv_obj_t *>(object), value);
}

void set_scale(void *object, int32_t value)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(object), value, 0);
}

void animate(lv_obj_t *object, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t duration,
             uint32_t delay, lv_anim_path_cb_t path, bool back_and_forth)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, object);
    lv_anim_set_exec_cb(&anim, exec);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, path);
    if (back_and_forth) {
        lv_anim_set_reverse_duration(&anim, duration);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    }
    lv_anim_start(&anim);
}

lv_obj_t *plain_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    return box;
}

lv_obj_t *add_text(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

// Strips of paper that fall over the whole screen, flipping (their width swings) and drifting.
void add_confetti(lv_obj_t *parent)
{
    static const uint32_t colours[] = {0xF2B705, 0x2F6B45, 0xE4572E, 0x3A86FF, 0xFF70A6, 0x7BC950};
    const int32_t width = lv_obj_get_width(parent);
    const int32_t height = lv_obj_get_height(parent);
    for (uint32_t i = 0; i < ConfettiCount; ++i) {
        lv_obj_t *piece = plain_box(parent);
        const int32_t w = static_cast<int32_t>(lv_rand(8, 14));
        lv_obj_set_size(piece, w, static_cast<int32_t>(lv_rand(12, 20)));
        lv_obj_set_style_radius(piece, 2, 0);
        lv_obj_set_style_bg_color(piece, lv_color_hex(colours[i % (sizeof(colours) / sizeof(colours[0]))]), 0);
        lv_obj_set_x(piece, static_cast<int32_t>(lv_rand(0, static_cast<uint32_t>(width))));
        const int32_t start = -static_cast<int32_t>(lv_rand(20, 260));
        lv_obj_set_y(piece, start);
        animate(piece, set_y, start, height + 20, lv_rand(2400, 3800), lv_rand(0, 900), lv_anim_path_linear, false);
        animate(piece, set_width, w, 2, lv_rand(250, 500), 0, lv_anim_path_ease_in_out, true);
        const int32_t drift = static_cast<int32_t>(lv_rand(20, 60));
        lv_obj_set_style_translate_x(piece, -drift, 0);
        animate(piece, set_translate_x, -drift, drift, lv_rand(700, 1300), 0, lv_anim_path_ease_in_out, true);
    }
}

// A gold coin drawn with LVGL: a rim and a face.
lv_obj_t *add_coin(lv_obj_t *parent, int32_t size)
{
    lv_obj_t *coin = plain_box(parent);
    lv_obj_set_size(coin, size, size);
    lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(coin, lv_color_hex(0xD99A00), 0);
    lv_obj_t *face = plain_box(coin);
    lv_obj_set_size(face, size - 12, size - 12);
    lv_obj_center(face);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(0xF2B705), 0);
    lv_obj_t *mark = add_text(face, "C", &lv_font_montserrat_28, lv_color_hex(0xB07800));
    lv_obj_center(mark);
    return coin;
}

void reward_timeout(lv_timer_t *)
{
    reward_timer = nullptr;  // a one-shot timer deletes itself
    finish_reward();
}

void show_reward()
{
    reward_overlay = plain_box(objects.level1);
    lv_obj_set_size(reward_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(reward_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(reward_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(reward_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reward_overlay, [](lv_event_t *) { finish_reward(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(reward_overlay);
    lv_obj_update_layout(reward_overlay);
    set_opa(reward_overlay, LV_OPA_TRANSP);
    animate(reward_overlay, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, 250, 0, lv_anim_path_ease_out, false);

    // The card, in the lesson's colours, pops in.
    lv_obj_t *card = plain_box(reward_overlay);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 50, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFAF7EF), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2F6B45), 0);
    lv_obj_set_style_border_width(card, 4, 0);
    lv_obj_set_style_shadow_width(card, 40, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_transform_pivot_x(card, 260, 0);
    lv_obj_set_style_transform_pivot_y(card, 160, 0);
    set_scale(card, 150);
    animate(card, set_scale, 150, 256, 450, 100, lv_anim_path_overshoot, false);

    lv_obj_t *title = add_text(card, "Level complete!", &lv_font_montserrat_28, lv_color_hex(0x2F6B45));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *row = plain_box(card);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 10);
    add_coin(row, 72);
    char amount[16];
    snprintf(amount, sizeof(amount), "+%d", static_cast<int>(RewardCoins));
    add_text(row, amount, &lv_font_montserrat_48, lv_color_hex(0x18352A));

    lv_obj_t *detail = add_text(card, "coins earned", &lv_font_montserrat_20, lv_color_hex(0x18352A));
    lv_obj_align(detail, LV_ALIGN_BOTTOM_MID, 0, -48);

    add_confetti(reward_overlay);

    reward_timer = lv_timer_create(reward_timeout, RewardShowMs, nullptr);
    lv_timer_set_repeat_count(reward_timer, 1);
}

void reward_delay_done(lv_timer_t *)
{
    reward_timer = nullptr;  // a one-shot timer deletes itself
    show_reward();
}

// Called when the question comes back with every story done.
void start_reward()
{
    stop_reward();
    completing = false;
    reward_timer = lv_timer_create(reward_delay_done, RewardDelayMs, nullptr);
    lv_timer_set_repeat_count(reward_timer, 1);
}

// Checked every tick on step 7, because a Reload puts the generated picture back.
void show_pick()
{
    const int32_t story = flow_int(FLOW_GLOBAL_VARIABLE_LESSON_CHOICE);
    if (story < 1 || story > 3) {
        return;
    }
    const void *image = screen_backgrounds_image(PickPictures[story - 1]);
    if (image && lv_image_get_src(objects.lesson_step_7_pic_4) != image) {
        lv_image_set_src(objects.lesson_step_7_pic_4, image);
    }
}
}

void lesson_init()
{
    for (const Tile &tile : tiles) {
        lv_obj_add_flag(*tile.object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(*tile.object, on_tile_clicked, LV_EVENT_CLICKED, const_cast<Tile *>(&tile));
    }
    lv_obj_add_event_cb(next_button(), on_next_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(objects.lesson_keep_up, LV_OBJ_FLAG_HIDDEN);
}

void lesson_tick()
{
    const bool on_level = lv_screen_active() == objects.level1;
    if (!on_level) {
        if (on_level_screen) {
            hide_keep_up();
            stop_auto();
            stop_reward();
        }
        on_level_screen = false;
        return;
    }
    const int32_t step = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL);
    const int32_t level = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_LEVEL);
    if (level == LessonLevel && step == 0) {  // a new run of the level
        done_stories = 0;
    }
    if (level == LessonLevel && step == CloudAnswer) {
        show_pick();
    }
    // EEZ labels the last story step "Complete Level"; here it is a Next, set after every
    // ui_tick() so the generated text never shows.
    if (level == LessonLevel && step == LastStoryStep) {
        lv_obj_t *label = lv_obj_get_child(next_button(), 0);
        if (label && strcmp(lv_label_get_text(label), "Next") != 0) {
            lv_label_set_text(label, "Next");
        }
    }
    if (!on_level_screen || step != shown_step || level != shown_level) {
        on_level_screen = true;
        shown_step = step;
        shown_level = level;
        stop_auto();
        stop_reward();
        if (level == LessonLevel && step == CloudQuestion) {
            show_done_stories();
            if (all_done_on_question(step)) {
                start_reward();
            }
        }
        if (level == LessonLevel && is_auto(step)) {
            show_next(true);
            start_auto(step);
            return;
        }
        show_next(level == LessonLevel && is_question(step));
    }
}
