#include "energy_overview.h"

#include "screens.h"
#include "time_service.h"

#include <lvgl.h>
#include <stdio.h>
#include <time.h>

namespace
{
// Battery chart time axis, the usual way for a time chart: "now" with the exact time stays fixed
// in the middle, and full hours (10:00, 11:00, ...) slide past it with their grid lines. An hour
// label that would touch "now" is hidden, so at 10:55 the axis reads 09:00, 10:00, now 10:55,
// 12:00 (as in the design). Positions are in the chart card, as EEZ placed its widgets there.
constexpr int NowX = 267;        // The "now" axis line.
constexpr int PxPerHour = 114;   // Two hours either side of now fit in the plot.
constexpr int PlotLeft = 24;     // The lv_chart widget: x 24..524, y 47..234.
constexpr int PlotRight = 524;
constexpr int PlotTop = 47;
constexpr int PlotHeight = 187;
constexpr int LabelWidth = 62;   // Hour labels from EEZ, centred on their hour.
constexpr int LabelMinCentre = LabelWidth / 2;       // Keep them inside the 548 px card.
constexpr int LabelMaxCentre = 548 - LabelWidth / 2;
// Closer than this, an hour would touch "now 10:55" below it: times are at most 46 px wide in
// Montserrat 16, plus 10 px between them.
constexpr int NowClearance = 56;
constexpr int HorizontalLines = 5; // 0, 25, 50, 75 and 100 %.
constexpr int MaxGridLines = 5;  // The plot is under 4.5 hours wide.
const lv_color_t GridColor = lv_color_hex(0xc4cdbe);

lv_obj_t **const hour_labels[] = {
    &objects.chart_time_label_1,
    &objects.chart_time_label_2,
    &objects.chart_time_label_3,
    &objects.chart_time_label_4,
};
constexpr int HourLabelCount = sizeof(hour_labels) / sizeof(hour_labels[0]);

lv_obj_t *grid_lines[MaxGridLines];

// The minute shown last, so the axis only changes when the clock does; -1 for "unknown".
long shown_minute = -2;

// The date, right-aligned on the greeting's line ("Good Day! Let's plan your energy"), ending
// where the chart card ends. Made here, not in EEZ, like the battery indicator.
lv_obj_t *date_label;
constexpr int DateRight = 937;
constexpr int DateWidth = 400;
constexpr int DateTop = 82;

void set_text(lv_obj_t *label, const char *text)
{
    if (label) {
        lv_label_set_text(label, text);
    }
}

void set_hidden(lv_obj_t *object, bool hidden)
{
    if (object) {
        if (hidden) {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void create_date_label()
{
    if (!objects.ai_forecast) {
        return;
    }
    date_label = lv_label_create(objects.ai_forecast);
    lv_obj_set_pos(date_label, DateRight - DateWidth, DateTop);
    lv_obj_set_width(date_label, DateWidth);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x2f6b45), LV_PART_MAIN);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(date_label, "");
}

// The chart draws only the horizontal lines; the hour lines move with the time, so they are
// separate 1 px boxes behind the chart's own lines.
void create_grid()
{
    if (!objects.battery_chart) {
        return;
    }
    lv_chart_set_div_line_count(objects.battery_chart, HorizontalLines, 0);
    lv_obj_t *card = lv_obj_get_parent(objects.battery_chart);
    for (lv_obj_t *&line : grid_lines) {
        line = lv_obj_create(card);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 1, PlotHeight);
        lv_obj_set_y(line, PlotTop);
        lv_obj_set_style_bg_color(line, GridColor, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(line, 0);
    }
}

// Where an hour falls on the axis, in card pixels.
int x_of(time_t at, time_t now)
{
    const long seconds = static_cast<long>(at - now);
    // Round to the nearest pixel, also for times before now.
    const long scaled = seconds * PxPerHour;
    return NowX + static_cast<int>((scaled >= 0 ? scaled + 1800 : scaled - 1800) / 3600);
}

void update_axis(time_t now)
{
    // South African time is a whole number of hours from UTC, so UTC hours are local hours.
    const time_t first_hour = (now - 3 * 3600) / 3600 * 3600;
    int label = 0;
    int line = 0;
    for (time_t hour = first_hour; hour <= now + 3 * 3600; hour += 3600) {
        const int x = x_of(hour, now);
        if (x >= PlotLeft && x < PlotRight && line < MaxGridLines && grid_lines[line]) {
            lv_obj_set_x(grid_lines[line], x);
            set_hidden(grid_lines[line], false);
            ++line;
        }
        const bool fits = x >= LabelMinCentre && x <= LabelMaxCentre;
        const bool clear_of_now = x <= NowX - NowClearance || x >= NowX + NowClearance;
        if (fits && clear_of_now && label < HourLabelCount && *hour_labels[label]) {
            struct tm local {};
            localtime_r(&hour, &local);
            char text[8];
            snprintf(text, sizeof(text), "%02d:00", local.tm_hour);
            lv_obj_t *object = *hour_labels[label];
            lv_label_set_text(object, text);
            lv_obj_set_x(object, x - LabelWidth / 2);
            set_hidden(object, false);
            ++label;
        }
    }
    for (; line < MaxGridLines; ++line) {
        set_hidden(grid_lines[line], true);
    }
    for (; label < HourLabelCount; ++label) {
        set_hidden(*hour_labels[label], true);
    }
}

void update(lv_timer_t *)
{
    struct tm local {};
    const bool known = time_service_now(&local);
    const time_t now = time(nullptr);
    const long minute = known ? static_cast<long>(now / 60) : -1;
    if (minute == shown_minute) {
        return;
    }
    shown_minute = minute;

    if (!known) {
        // No time yet: no date, no hours, only "now".
        set_text(date_label, "");
        set_text(objects.chart_now_label, "now\n--:--");
        for (lv_obj_t *const *label : hour_labels) {
            set_hidden(*label, true);
        }
        for (lv_obj_t *line : grid_lines) {
            set_hidden(line, true);
        }
        return;
    }

    // "Wednesday 25 November 2026": the C locale's English day and month names.
    char weekday[16];
    char month[16];
    strftime(weekday, sizeof(weekday), "%A", &local);
    strftime(month, sizeof(month), "%B", &local);
    char date[48];
    snprintf(date, sizeof(date), "%s %d %s %d", weekday, local.tm_mday, month, local.tm_year + 1900);
    set_text(date_label, date);

    char text[16];
    snprintf(text, sizeof(text), "now\n%02d:%02d", local.tm_hour, local.tm_min);
    set_text(objects.chart_now_label, text);

    update_axis(now);
}
}

void energy_overview_init()
{
    create_date_label();
    create_grid();
    update(nullptr);
    lv_timer_create(update, 1000, nullptr);
}
