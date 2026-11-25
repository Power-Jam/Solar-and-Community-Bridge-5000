#include "battery_indicator.h"

#include "ina_service.h"
#include "screens.h"

#include <lvgl.h>

// The nav bar pictures (sd/assets/nav-bar*.jpg) have an empty light-green pill where the battery
// goes; the transparent battery buttons from EEZ lie exactly on it (80 x 40). This draws an
// outlined battery that fills with the charge, and the percentage next to it, in each button.
namespace
{
const lv_color_t Ink = lv_color_hex(0x18352a);
const lv_color_t Low = lv_color_hex(0xc0392b);
constexpr int LowPercent = 15;

// Battery outline in the button, as it was drawn in the pictures: 20 x 14 with a 2 px border,
// and a 2 x 4 tip 2 px to the right of it.
constexpr int BodyX = 10;
constexpr int BodyY = 13;
constexpr int BodyWidth = 20;
constexpr int BodyHeight = 14;
constexpr int Border = 2;
constexpr int FillInset = Border + 1;
constexpr int FillMaxWidth = BodyWidth - 2 * FillInset;
constexpr int TextX = 41;

struct Indicator {
    lv_obj_t **button; // In the EEZ objects table; created by ui_init().
    // The battery button opens the energy overview; on that screen itself it does nothing.
    bool clickable = true;
    lv_obj_t *attached_to;
    lv_obj_t *fill;
    lv_obj_t *label;
};

Indicator indicators[] = {
    {&objects.nav_bar_chapters__battery_button},
    {&objects.nav_bar_chapters__battery_button_no_coins},
    {&objects.nav_bar_roadmap__battery_button},
    {&objects.nav_bar_roadmap__battery_button_no_coins},
    {&objects.nav_bar_level__battery_button},
    {&objects.nav_bar_level__battery_button_no_coins},
    {&objects.nav_bar_profile__battery_button},
    {&objects.nav_bar_profile__battery_button_no_coins},
    {&objects.nav_bar_shop__battery_button},
    {&objects.nav_bar_shop__battery_button_no_coins},
    {&objects.nav_bar_ai_forecast__battery_button, false},
    {&objects.nav_bar_ai_forecast__battery_button_no_coins, false},
};

// A plain rectangle that lets touches through to the button.
lv_obj_t *create_box(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    return box;
}

void button_deleted(lv_event_t *event)
{
    auto *indicator = static_cast<Indicator *>(lv_event_get_user_data(event));
    indicator->attached_to = nullptr;
    indicator->fill = nullptr;
    indicator->label = nullptr;
}

void attach(Indicator &indicator, lv_obj_t *button)
{
    lv_obj_add_event_cb(button, button_deleted, LV_EVENT_DELETE, &indicator);
    if (!indicator.clickable) {
        // No CLICKED event, so the nav bar flow never starts, and no pressed look either.
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    // The button's own padding would shift the children; place them from its corner.
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);

    lv_obj_t *body = create_box(button, BodyX, BodyY, BodyWidth, BodyHeight);
    lv_obj_set_style_border_width(body, Border, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, Ink, LV_PART_MAIN);
    lv_obj_set_style_border_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 3, LV_PART_MAIN);

    lv_obj_t *tip = create_box(button, BodyX + BodyWidth + 2, BodyY + 5, 2, 4);
    lv_obj_set_style_bg_color(tip, Ink, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);

    indicator.fill = create_box(button, BodyX + FillInset, BodyY + FillInset, 0,
                                BodyHeight - 2 * FillInset);
    lv_obj_set_style_bg_color(indicator.fill, Ink, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator.fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(indicator.fill, 1, LV_PART_MAIN);

    indicator.label = lv_label_create(button);
    lv_obj_remove_flag(indicator.label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(indicator.label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_color(indicator.label, Ink, LV_PART_MAIN);
    lv_obj_set_style_text_font(indicator.label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(indicator.label, "--%");
    lv_obj_align(indicator.label, LV_ALIGN_LEFT_MID, TextX, 0);

    indicator.attached_to = button;
}

void update(lv_timer_t *)
{
    InaReading reading{};
    const bool known = ina_service_latest(INA_BATTERY, &reading) && reading.connected &&
                       reading.has_battery_percent;
    const int percent = known ? reading.battery_percent : -1;

    for (Indicator &indicator : indicators) {
        lv_obj_t *button = *indicator.button;
        if (!button) {
            continue;
        }
        // A screen that was built again has a new button: draw in that one.
        if (indicator.attached_to != button) {
            attach(indicator, button);
        }
        if (percent < 0) {
            lv_label_set_text(indicator.label, "--%");
            lv_obj_set_width(indicator.fill, 0);
            continue;
        }
        lv_label_set_text_fmt(indicator.label, "%d%%", percent);
        // Any charge at all shows at least a sliver.
        const int width = percent == 0 ? 0 : (percent * FillMaxWidth + 99) / 100;
        lv_obj_set_width(indicator.fill, width);
        lv_obj_set_style_bg_color(indicator.fill, percent <= LowPercent ? Low : Ink,
                                  LV_PART_MAIN);
    }
}
}

void battery_indicator_init()
{
    update(nullptr);
    lv_timer_create(update, 1000, nullptr);
}
