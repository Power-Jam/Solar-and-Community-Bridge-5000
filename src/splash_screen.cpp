#include "splash_screen.h"

#include "eez-flow.h"
#include "screen_backgrounds.h"
#include "screens.h"
#include "sd_service.h"

#include <Arduino.h>
#include <lvgl.h>

// Set in platformio.ini; without it, the dashboard is shown as before.
#ifndef DEVELOPER_MODE
#define DEVELOPER_MODE 1
#endif

namespace
{
// The bar follows the pictures loading into PSRAM and closes when they are done.
// This is only a safety net in case loading hangs.
constexpr uint32_t MaxDurationMs = 60000;
constexpr const char *DeviceName = "Solar & Community Bridge 5000";

// The logo's corners are black, so the screen is too; the bar takes the strawberry's red.
const lv_color_t Background = lv_color_hex(0x000000);
const lv_color_t Text = lv_color_hex(0xF7F1EC);
const lv_color_t Muted = lv_color_hex(0x8C9CA5);
const lv_color_t Track = lv_color_hex(0x2A2A2A);
const lv_color_t Strawberry = lv_color_hex(0xB0202A);

lv_obj_t *splash = nullptr;
lv_obj_t *next_screen = nullptr;
lv_obj_t *logo = nullptr;
lv_obj_t *bar = nullptr;
lv_obj_t *percent_label = nullptr;
uint32_t started_ms = 0;
uint32_t seen_version = 0;
bool logo_shown = false;

lv_obj_t *add_label(const char *text, const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *label = lv_label_create(splash);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}
}

void splash_screen_init()
{
    next_screen = lv_screen_active();

    splash = lv_obj_create(nullptr);
    lv_obj_set_size(splash, 1024, 600);
    lv_obj_remove_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(splash, Background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(splash, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(splash, 0, LV_PART_MAIN);

    // Stays empty until the flash is mounted and the logo is decoded (a fraction of a second).
    logo = lv_image_create(splash);
    lv_obj_set_size(logo, 250, 250);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 60);

    add_label(DeviceName, &lv_font_montserrat_48, Text, 340);

    bar = lv_bar_create(splash);
    lv_obj_set_size(bar, 600, 18);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 450);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, Track, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, Strawberry, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    percent_label = add_label("Loading... 0%", &lv_font_montserrat_20, Muted, 486);

    started_ms = millis();
    lv_screen_load(splash);
}

void splash_screen_tick()
{
    if (!splash) {
        return;
    }

    if (!logo_shown && screen_backgrounds_version() != seen_version) {
        seen_version = screen_backgrounds_version();
        const void *image = screen_backgrounds_image(SPLASH_LOGO_PATH);
        if (image) {
            lv_image_set_src(logo, image);
            logo_shown = true;
        }
    }

    size_t processed = 0;
    size_t total = 0;
    const bool finished = sd_service_load_progress(&processed, &total);
    const int32_t percent = total > 0 ? static_cast<int32_t>(processed * 100 / total) : 0;
    if (percent != lv_bar_get_value(bar)) {
        lv_bar_set_value(bar, percent, LV_ANIM_ON);
        lv_label_set_text_fmt(percent_label, "Loading... %d%%", static_cast<int>(percent));
    }

    const uint32_t elapsed = millis() - started_ms;
    if (finished || elapsed >= MaxDurationMs) {
        Serial.printf("[Splash] Done after %u ms\n", static_cast<unsigned>(elapsed));
#if DEVELOPER_MODE
        // Developer mode: on to the dashboard. Deleting the splash once the new screen is
        // shown frees its widgets.
        lv_screen_load_anim(next_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
#else
        // Straight to the app screens, through the EEZ flow as the dashboard's button does;
        // the splash is deleted once the fade has finished.
        eez_flow_set_screen(SCREEN_ID_HOME_PAGE, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0);
        lv_obj_delete_delayed(splash, 400);
#endif
        splash = nullptr;
        logo = nullptr;
        bar = nullptr;
        percent_label = nullptr;
    }
}
