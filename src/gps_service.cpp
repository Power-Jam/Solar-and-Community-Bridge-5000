#include "gps_service.h"

#include "board_config.h"
#include "time_service.h"

#include <Arduino.h>
#include <TinyGPS++.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdio>

namespace
{
struct GpsStatus {
    char text[128];
};

QueueHandle_t status_queue;
portMUX_TYPE latest_lock = portMUX_INITIALIZER_UNLOCKED;
GpsFix latest_fix;
bool latest_valid;

void publish_fix(const GpsFix *fix)
{
    portENTER_CRITICAL(&latest_lock);
    latest_valid = fix != nullptr;
    if (fix) {
        latest_fix = *fix;
    }
    portEXIT_CRITICAL(&latest_lock);
}

void publish_status(const char *text)
{
    GpsStatus status{};
    strlcpy(status.text, text, sizeof(status.text));
    if (status_queue && xQueueOverwrite(status_queue, &status) != pdTRUE) {
        Serial.println("[GPS] Unable to publish GPS status");
    }
}

void gps_task(void *)
{
    TinyGPSPlus gps;
    Serial1.setRxBufferSize(4096);
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_GPIO_RX, -1);

    uint32_t last_nmea = 0;
    uint32_t last_report = 0;
    uint32_t last_log = 0;
    for (;;) {
        const uint32_t now = millis();
        while (Serial1.available()) {
            if (gps.encode(static_cast<char>(Serial1.read()))) {
                last_nmea = now;
            }
        }

        if (now - last_report >= 1000) {
            char status[128];
            if (gps.location.isValid() && gps.location.age() < 10000) {
                GpsFix fix{};
                fix.latitude = gps.location.lat();
                fix.longitude = gps.location.lng();
                fix.satellites_valid =
                    gps.satellites.isValid() && gps.satellites.age() < 10000;
                fix.satellites = static_cast<uint8_t>(
                    std::min<uint32_t>(gps.satellites.value(), 254));
                if (gps.date.isValid() && gps.date.year() >= 2020) {
                    fix.date = gps.date.year() * 10000UL + gps.date.month() * 100UL +
                               gps.date.day();
                    // A fresh time from the same fix sets the clock when it is unknown or off.
                    if (gps.time.isValid() && gps.time.age() < 1500) {
                        time_service_set_from_gps(gps.date.year(), gps.date.month(),
                                                  gps.date.day(), gps.time.hour(),
                                                  gps.time.minute(), gps.time.second());
                    }
                }
                publish_fix(&fix);

                const char *satellites =
                    gps.satellites.isValid() && gps.satellites.age() < 10000
                        ? nullptr
                        : "--";
                if (satellites) {
                    snprintf(status, sizeof(status),
                             "%.6f, %.6f | %s sats",
                             gps.location.lat(), gps.location.lng(), satellites);
                } else {
                    snprintf(status, sizeof(status),
                             "%.6f, %.6f | %lu sats",
                             gps.location.lat(), gps.location.lng(),
                             static_cast<unsigned long>(gps.satellites.value()));
                }
            } else if (gps.passedChecksum() == 0 || now - last_nmea >= 10000) {
                publish_fix(nullptr);
                strlcpy(status, "No valid NMEA / check UART and baud rate",
                        sizeof(status));
            } else {
                publish_fix(nullptr);
                strlcpy(status, "No fresh fix / move antenna outdoors",
                        sizeof(status));
            }
            publish_status(status);
            last_report = now;

            if (now - last_log >= 5000) {
                Serial.printf("[GPS] chars=%lu ok=%lu failed=%lu fix=%lu sats=%lu / %s\n",
                              static_cast<unsigned long>(gps.charsProcessed()),
                              static_cast<unsigned long>(gps.passedChecksum()),
                              static_cast<unsigned long>(gps.failedChecksum()),
                              static_cast<unsigned long>(gps.sentencesWithFix()),
                              static_cast<unsigned long>(gps.satellites.value()),
                              status);
                last_log = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
}

void gps_service_init()
{
    status_queue = xQueueCreate(1, sizeof(GpsStatus));
    if (!status_queue) {
        Serial.println("[GPS] Unable to create GPS status queue");
        return;
    }

    publish_status("Waiting for GPS data...");
    if (xTaskCreate(gps_task, "gps", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[GPS] Unable to start GPS task");
        vQueueDelete(status_queue);
        status_queue = nullptr;
    }
}

bool gps_service_read_status(char *destination, size_t capacity)
{
    if (!status_queue || !destination || capacity == 0) {
        return false;
    }

    GpsStatus status{};
    if (xQueueReceive(status_queue, &status, 0) != pdTRUE) {
        return false;
    }
    strlcpy(destination, status.text, capacity);
    return true;
}

bool gps_service_latest(GpsFix *destination)
{
    if (!destination) {
        return false;
    }
    portENTER_CRITICAL(&latest_lock);
    const bool valid = latest_valid;
    *destination = latest_fix;
    portEXIT_CRITICAL(&latest_lock);
    return valid;
}
