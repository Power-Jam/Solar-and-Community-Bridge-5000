#include "ina_service.h"

#include "board_config.h"

#include <Arduino.h>
#include <INA226.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cmath>

namespace
{
constexpr uint32_t ReadIntervalMs = 1000;
constexpr uint32_t RetryIntervalMs = 5000;
constexpr uint32_t LogIntervalMs = 5000;

QueueHandle_t reading_queue;

void publish_reading(const InaReading &reading)
{
    if (reading_queue && xQueueOverwrite(reading_queue, &reading) != pdTRUE) {
        Serial.println("[INA226] Unable to publish INA226 reading");
    }
}

bool configure(INA226 &meter)
{
    if (!meter.begin() || !meter.setAverage(INA226_16_SAMPLES)) {
        return false;
    }
    const int result = meter.setMaxCurrentShunt(INA_MAX_CURRENT_A, INA_SHUNT_OHMS);
    if (result != INA226_ERR_NONE) {
        Serial.printf("[INA226] Calibration failed (0x%04X)\n", result);
        return false;
    }
    Serial.printf("[INA226] Ready at 0x%02X / shunt %.3f ohm / max %.2f A\n",
                  INA_ADDRESS, INA_SHUNT_OHMS, INA_MAX_CURRENT_A);
    return true;
}

void ina_task(void *)
{
    INA226 meter(INA_ADDRESS, &Wire);
    bool configured = false;
    uint32_t last_log = 0;
    for (;;) {
        InaReading reading{};
        if (!configured) {
            configured = configure(meter);
        }
        if (configured) {
            reading.voltage = meter.getBusVoltage();
            reading.current = meter.getCurrent();
            reading.power = meter.getPower();
            // The chip stays silent on a lost connection, so check before trusting the values.
            reading.connected = meter.getLastError() == 0 && meter.isConnected();
            reading.over_range = std::fabs(reading.current) > INA_MAX_CURRENT_A;
            configured = reading.connected;
        }
        publish_reading(reading);

        const uint32_t now = millis();
        if (now - last_log >= LogIntervalMs) {
            if (reading.connected) {
                Serial.printf("[INA226] %.2f V / %.3f A / %.2f W%s\n",
                              reading.voltage, reading.current, reading.power,
                              reading.over_range ? " / OVER RANGE" : "");
            } else {
                Serial.printf("[INA226] Not found at 0x%02X / check SDA, SCL, VCC and GND\n",
                              INA_ADDRESS);
            }
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(configured ? ReadIntervalMs : RetryIntervalMs));
    }
}
}

void ina_service_init(bool i2c_available)
{
    if (!i2c_available) {
        Serial.println("[INA226] Unavailable: I2C bus failed");
        return;
    }

    reading_queue = xQueueCreate(1, sizeof(InaReading));
    if (!reading_queue) {
        Serial.println("[INA226] Unable to create INA226 queue");
        return;
    }
    if (xTaskCreate(ina_task, "ina226", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[INA226] Unable to start INA226 task");
        vQueueDelete(reading_queue);
        reading_queue = nullptr;
    }
}

bool ina_service_read(InaReading *destination)
{
    return reading_queue && destination &&
           xQueueReceive(reading_queue, destination, 0) == pdTRUE;
}
