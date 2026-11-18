#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "display.h"

namespace
{
constexpr size_t LVGL_BUFFER_ROWS = 20;
constexpr uint8_t BLIGHT_PWM_BITS = 10;

bool backlight_pwm = false;
bool backlight_dimmed = false;

void set_backlight_percent(uint8_t percent)
{
    constexpr uint32_t max_duty = (1U << BLIGHT_PWM_BITS) - 1;
    uint32_t duty = max_duty * percent / 100;
    if (!BLIGHT_ON_LEVEL) {
        duty = max_duty - duty;
    }
    ledcWrite(LCD_GPIO_BLIGHT, duty);
}

Arduino_ESP32DSIPanel panel_bus(
    LCD_HPW, LCD_HBP, LCD_HFP, LCD_VPW, LCD_VBP, LCD_VFP,
    LCD_CLK_MHZ * 1000000, 1000);
Arduino_DSI_Display panel(
    PANEL_WIDTH, PANEL_HEIGHT, &panel_bus, 0, true, LCD_GPIO_RST,
    ek79007_init_operations,
    sizeof(ek79007_init_operations) / sizeof(lcd_init_cmd_t));

void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    panel.draw16bitRGBBitmap(
        area->x1, area->y1, reinterpret_cast<uint16_t *>(pixel_map),
        area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    lv_display_flush_ready(display);
}

void free_frame_buffers(lv_color_t *frame_buffer_0, lv_color_t *frame_buffer_1)
{
    if (frame_buffer_0) {
        heap_caps_free(frame_buffer_0);
    }
    if (frame_buffer_1) {
        heap_caps_free(frame_buffer_1);
    }
}
}

lv_display_t *display_init()
{
    pinMode(LCD_GPIO_POWER, OUTPUT);
    digitalWrite(LCD_GPIO_POWER, LOW);
    pinMode(LCD_GPIO_BLIGHT, OUTPUT);
    digitalWrite(LCD_GPIO_BLIGHT, LOW);

    if (!panel.begin()) {
        Serial.println("[Display] CrowPanel DSI display startup failed");
        return nullptr;
    }

    panel.setTextWrap(false);
    panel.fillScreen(0);
    backlight_pwm = ledcAttach(LCD_GPIO_BLIGHT, BLIGHT_PWM_HZ, BLIGHT_PWM_BITS);
    if (backlight_pwm) {
        set_backlight_percent(BLIGHT_ACTIVE_PERCENT);
    } else {
        Serial.println("[Display] Backlight PWM unavailable; running at full brightness");
        digitalWrite(LCD_GPIO_BLIGHT, BLIGHT_ON_LEVEL);
    }

    lv_init();
    lv_log_register_print_cb([](lv_log_level_t, const char *message) { Serial.printf("[LVGL] %s", message); });
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    const size_t draw_buffer_size = PANEL_WIDTH * LVGL_BUFFER_ROWS * sizeof(lv_color_t);
    auto *draw_buffer_0 = static_cast<lv_color_t *>(
        heap_caps_malloc(draw_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *draw_buffer_1 = static_cast<lv_color_t *>(
        heap_caps_malloc(draw_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!draw_buffer_0 || !draw_buffer_1) {
        Serial.println("[Display] LVGL partial draw-buffer allocation failed");
        free_frame_buffers(draw_buffer_0, draw_buffer_1);
        return nullptr;
    }

    lv_display_t *display = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
    if (!display) {
        Serial.println("[Display] LVGL display creation failed");
        free_frame_buffers(draw_buffer_0, draw_buffer_1);
        return nullptr;
    }

    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(display, draw_buffer_0, draw_buffer_1, draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    return display;
}

void display_backlight_tick(lv_display_t *display)
{
    if (!backlight_pwm || !display) {
        return;
    }
    // LVGL resets the inactive time on every touch, so a touch brings the brightness back.
    const bool dim = lv_display_get_inactive_time(display) >= BLIGHT_DIM_AFTER_MS;
    if (dim == backlight_dimmed) {
        return;
    }
    backlight_dimmed = dim;
    const uint8_t percent = dim ? BLIGHT_DIM_PERCENT : BLIGHT_ACTIVE_PERCENT;
    set_backlight_percent(percent);
    Serial.printf("[Display] Backlight %u%%\n", static_cast<unsigned>(percent));
}

bool display_backlight_wake(lv_display_t *display)
{
    if (!backlight_pwm || !backlight_dimmed) {
        return false;
    }
    backlight_dimmed = false;
    set_backlight_percent(BLIGHT_ACTIVE_PERCENT);
    // The wake touch is not passed to LVGL, so restart the inactivity timer here.
    if (display) {
        lv_display_trigger_activity(display);
    }
    Serial.printf("[Display] Backlight %u%% (woken by touch)\n",
                  static_cast<unsigned>(BLIGHT_ACTIVE_PERCENT));
    return true;
}
