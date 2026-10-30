#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display.h"
#include "board_config.h"
#include "rfid.h"
#include "touch.h"
#include "ui.h"
#include "sd_service.h"
#include "lora_service.h"
#include "audio_service.h"
#include "gps_service.h"
#include "ina_service.h"
#include "wifi_service.h"

namespace
{
    lv_display_t *display = nullptr;

    void lvgl_task(void *)
    {
        Serial.println("LVGL task started");
        while (true) {
            ui_tick();

            static bool first_handler_reported = false;
            if (!first_handler_reported) {
                Serial.println("LVGL task calling timer handler");
                first_handler_reported = true;
            }

            const uint32_t next_timer_ms = lv_timer_handler();
            static bool first_return_reported = false;
            if (!first_return_reported) {
                Serial.printf("LVGL timer handler returned; stack high-water mark: %u bytes\n",
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
    rfid_init();
    const bool i2c_ready = Wire.begin(I2C_GPIO_SDA, I2C_GPIO_SCL, 100000);
    if (i2c_ready) {
        Wire.setTimeOut(20);
    } else {
        Serial.println("I2C initialization failed; touch may be unavailable");
    }
    audio_service_init();

    display = display_init();
    if (!display)
    {
        return;
    }

    const bool touch_available = touch_init(i2c_ready);
    ui_init();
    sd_service_init();
    wifi_init(sd_service_start);
    lora_service_init();
    gps_service_init();
    ina_service_init(i2c_ready);
    if (touch_available) {
        touch_setup_lvgl(display);
    }

    Serial.printf("EEZ UI ready (touch %s): Arduino %s, LVGL %d.%d.%d\n",
                  touch_available ? "enabled" : "unavailable",
                  ESP_ARDUINO_VERSION_STR, LVGL_VERSION_MAJOR,
                  LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    constexpr uint32_t LVGL_TASK_STACK_BYTES = 16 * 1024;
    constexpr UBaseType_t LVGL_TASK_PRIORITY = 3;
    const BaseType_t task_created = xTaskCreate(
        lvgl_task, "lvgl", LVGL_TASK_STACK_BYTES, nullptr, LVGL_TASK_PRIORITY, nullptr);
    if (task_created != pdPASS) {
        Serial.println("Failed to create LVGL task");
    }
}

void loop()
{
    rfid_poll();
    delay(50);
}
