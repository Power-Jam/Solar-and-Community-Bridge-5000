#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "display.h"

namespace
{
constexpr size_t LVGL_BUFFER_ROWS = 20;

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
    digitalWrite(LCD_GPIO_BLIGHT, HIGH);

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
