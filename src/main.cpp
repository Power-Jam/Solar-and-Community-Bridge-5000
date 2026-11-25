#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display.h"
#include "board_config.h"
#include "rfid.h"
#include "touch.h"
#include "ui.h"
#include "landing_screen.h"
#include "lesson_service.h"
#include "shop_service.h"
#include "users_service.h"
#include "screen_backgrounds.h"
#include "battery_indicator.h"
#include "energy_overview.h"
#include "splash_screen.h"
#include "sd_service.h"
#include "lora_service.h"
#include "audio_service.h"
#include "gps_service.h"
#include "ina_service.h"
#include "temp_service.h"
#include "time_service.h"
#include "wifi_service.h"

namespace
{
    lv_display_t *display = nullptr;

    void lvgl_task(void *)
    {
        Serial.println("[LVGL] LVGL task started");
        while (true) {
            ui_tick();
            users_tick();
            shop_tick();
            lesson_tick();
            landing_screen_tick();
            screen_backgrounds_tick();
            splash_screen_tick();

            static bool first_handler_reported = false;
            if (!first_handler_reported) {
                Serial.println("[LVGL] LVGL task calling timer handler");
                first_handler_reported = true;
            }

            const uint32_t next_timer_ms = lv_timer_handler();
            display_backlight_tick(display);
            static bool first_return_reported = false;
            if (!first_return_reported) {
                Serial.printf("[LVGL] LVGL timer handler returned; stack high-water mark: %u bytes\n",
                              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
                first_return_reported = true;
            }

            static uint32_t last_memory_log = 0;
            if (millis() - last_memory_log >= 30000) {
                lv_mem_monitor_t memory;
                lv_mem_monitor(&memory);
                Serial.printf("[LVGL] free %u of %u bytes / largest block %u / peak %u bytes / fragmentation %u%%\n",
                              static_cast<unsigned>(memory.free_size),
                              static_cast<unsigned>(memory.total_size),
                              static_cast<unsigned>(memory.free_biggest_size),
                              static_cast<unsigned>(memory.max_used),
                              static_cast<unsigned>(memory.frag_pct));
                last_memory_log = millis();
            }

            const uint32_t delay_ms = constrain(next_timer_ms, 2U, 20U);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

void setup()
{
    Serial.begin(115200);
    time_service_init();
    rfid_init();
    const bool i2c_ready = Wire.begin(I2C_GPIO_SDA, I2C_GPIO_SCL, 100000);
    if (i2c_ready) {
        Wire.setTimeOut(20);
    } else {
        Serial.println("[I2C] I2C initialization failed; touch may be unavailable");
    }
    audio_service_init();

    display = display_init();
    if (!display)
    {
        return;
    }

    const bool touch_available = touch_init(i2c_ready);
    ui_init();
    screen_backgrounds_init();
    users_init();
    shop_init();
    lesson_init();
    landing_screen_init();
    battery_indicator_init();
    energy_overview_init();
    splash_screen_init();
    sd_service_init();
#if ASSETS_ON_SD_CARD
    // The SD card shares the SDMMC host with the WiFi chip, so it starts once WiFi has set it up.
    wifi_init(sd_service_start);
#else
    sd_service_start();
    wifi_init([] {});
#endif
    lora_service_init();
    gps_service_init();
    ina_service_init(i2c_ready);
    temp_service_init();
    if (touch_available) {
        touch_setup_lvgl(display);
    }

    Serial.printf("[UI] EEZ UI ready (touch %s): Arduino %s, LVGL %d.%d.%d\n",
                  touch_available ? "enabled" : "unavailable",
                  ESP_ARDUINO_VERSION_STR, LVGL_VERSION_MAJOR,
                  LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    constexpr uint32_t LVGL_TASK_STACK_BYTES = 16 * 1024;
    constexpr UBaseType_t LVGL_TASK_PRIORITY = 3;
    const BaseType_t task_created = xTaskCreate(
        lvgl_task, "lvgl", LVGL_TASK_STACK_BYTES, nullptr, LVGL_TASK_PRIORITY, nullptr);
    if (task_created != pdPASS) {
        Serial.println("[LVGL] Failed to create LVGL task");
    }
}

void loop()
{
    rfid_poll();
    delay(50);
}
