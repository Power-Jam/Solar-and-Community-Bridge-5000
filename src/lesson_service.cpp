#include "lesson_service.h"

#include "eez-flow.h"
#include "screen_backgrounds.h"
#include "screens.h"
#include "vars.h"

#include <lvgl.h>

// Level 1 follows the LevelDesign Figma file. EEZ shows the step in currentPanel (and the story
// in lessonChoice); this file runs what the flows can't: the questions and the Next button's fade.
namespace
{
constexpr int32_t LessonLevel = 1;
constexpr int32_t SunQuestion = 1;      // "What do you need to charge a battery?"
constexpr int32_t CloudQuestion = 6;    // "What would you use your battery charge for?"
constexpr int32_t StoryChoice = 8;      // "What are you going to do with ...?"
constexpr int32_t FirstStoryStep = 9;
constexpr uint32_t KeepUpMs = 1400;
constexpr uint32_t NextDelayMs = 800;   // as in Figma: Next appears after 0.8 s, in 0.3 s
constexpr uint32_t NextFadeMs = 300;

enum class Answer : uint8_t { Right, Wrong, Story1, Story2, Story3 };

struct Tile {
    lv_obj_t *const *object;
    int32_t step;
    Answer answer;
};

const Tile tiles[] = {
    {&objects.lesson_q1_sun, SunQuestion, Answer::Right},
    {&objects.lesson_q1_lamp, SunQuestion, Answer::Wrong},
    {&objects.lesson_q1_house, SunQuestion, Answer::Wrong},
    {&objects.lesson_q2_battery, CloudQuestion, Answer::Right},
    {&objects.lesson_q2_lamp, CloudQuestion, Answer::Wrong},
    {&objects.lesson_q2_house, CloudQuestion, Answer::Wrong},
    {&objects.lesson_choice_battery, StoryChoice, Answer::Story1},
    {&objects.lesson_choice_lamp, StoryChoice, Answer::Story2},
    {&objects.lesson_choice_house, StoryChoice, Answer::Story3},
};

lv_timer_t *keep_up_timer = nullptr;
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
    return step == SunQuestion || step == CloudQuestion || step == StoryChoice;
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
    switch (tile.answer) {
    case Answer::Right:
        show_keep_up();
        break;
    case Answer::Wrong:
        shake(*tile.object);
        break;
    default:
        set_flow_int(FLOW_GLOBAL_VARIABLE_LESSON_CHOICE, static_cast<int32_t>(tile.answer) - static_cast<int32_t>(Answer::Story1) + 1);
        set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL, FirstStoryStep);
        break;
    }
}
}

void lesson_init()
{
    for (const Tile &tile : tiles) {
        lv_obj_add_flag(*tile.object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(*tile.object, on_tile_clicked, LV_EVENT_CLICKED, const_cast<Tile *>(&tile));
    }
    lv_obj_add_flag(objects.lesson_keep_up, LV_OBJ_FLAG_HIDDEN);
}

void lesson_tick()
{
    const bool on_level = lv_screen_active() == objects.level1;
    if (!on_level) {
        if (on_level_screen) {
            hide_keep_up();
        }
        on_level_screen = false;
        return;
    }
    const int32_t step = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_PANEL);
    const int32_t level = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_LEVEL);
    if (!on_level_screen || step != shown_step || level != shown_level) {
        on_level_screen = true;
        shown_step = step;
        shown_level = level;
        show_next(level == LessonLevel && is_question(step));
    }
}
