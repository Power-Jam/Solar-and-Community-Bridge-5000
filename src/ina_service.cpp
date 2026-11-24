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

struct SensorConfig {
    const char *name;
    uint8_t address;
    float shunt_ohms;
    float max_current_a;
};

const SensorConfig Sensors[INA_SENSOR_COUNT] = {
    {"INA226 solar", INA_ADDRESS, INA_SHUNT_OHMS, INA_MAX_CURRENT_A},
    {"INA226 battery", INA2_ADDRESS, INA2_SHUNT_OHMS, INA2_MAX_CURRENT_A},
};

QueueHandle_t reading_queues[INA_SENSOR_COUNT];
portMUX_TYPE latest_lock = portMUX_INITIALIZER_UNLOCKED;
InaReading latest_readings[INA_SENSOR_COUNT];
bool latest_valid[INA_SENSOR_COUNT];

void publish_reading(size_t sensor, const InaReading &reading)
{
    portENTER_CRITICAL(&latest_lock);
    latest_readings[sensor] = reading;
    latest_valid[sensor] = true;
    portEXIT_CRITICAL(&latest_lock);
    if (reading_queues[sensor] && xQueueOverwrite(reading_queues[sensor], &reading) != pdTRUE) {
        Serial.printf("[%s] Unable to publish reading\n", Sensors[sensor].name);
    }
}

bool configure(INA226 &meter, const SensorConfig &config)
{
    if (!meter.begin() || !meter.setAverage(INA226_16_SAMPLES)) {
        return false;
    }
    const int result = meter.setMaxCurrentShunt(config.max_current_a, config.shunt_ohms);
    if (result != INA226_ERR_NONE) {
        Serial.printf("[%s] Calibration failed (0x%04X)\n", config.name, result);
        return false;
    }
    Serial.printf("[%s] Ready at 0x%02X / shunt %.3f ohm / max %.2f A\n",
                  config.name, config.address, config.shunt_ohms, config.max_current_a);
    return true;
}

void ina_task(void *)
{
    INA226 meters[INA_SENSOR_COUNT] = {
        INA226(Sensors[INA_SOLAR].address, &Wire),
        INA226(Sensors[INA_BATTERY].address, &Wire),
    };
    bool configured[INA_SENSOR_COUNT] = {};
    uint32_t last_retry[INA_SENSOR_COUNT] = {};
    bool tried[INA_SENSOR_COUNT] = {};
    uint32_t last_log = 0;
    for (;;) {
        const uint32_t now = millis();
        const bool log_now = now - last_log >= LogIntervalMs;
        for (size_t i = 0; i < INA_SENSOR_COUNT; ++i) {
            const SensorConfig &config = Sensors[i];
            InaReading reading{};
            // A missing sensor is retried every few seconds, so it does not slow the other one.
            if (!configured[i] && (!tried[i] || now - last_retry[i] >= RetryIntervalMs)) {
                tried[i] = true;
                last_retry[i] = now;
                configured[i] = configure(meters[i], config);
            }
            if (configured[i]) {
                reading.voltage = meters[i].getBusVoltage();
                reading.current = meters[i].getCurrent();
                reading.power = meters[i].getPower();
                // The chip stays silent on a lost connection, so check before trusting the values.
                reading.connected = meters[i].getLastError() == 0 && meters[i].isConnected();
                reading.over_range = std::fabs(reading.current) > config.max_current_a;
                configured[i] = reading.connected;
                if (!reading.connected) {
                    last_retry[i] = now;
                }
            }
            publish_reading(i, reading);

            if (log_now) {
                if (reading.connected) {
                    Serial.printf("[%s] %.2f V / %.3f A / %.2f W%s\n", config.name,
                                  reading.voltage, reading.current, reading.power,
                                  reading.over_range ? " / OVER RANGE" : "");
                } else {
                    Serial.printf("[%s] Not found at 0x%02X / check SDA, SCL, VCC, GND and A0/A1\n",
                                  config.name, config.address);
                }
            }
        }
        if (log_now) {
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(ReadIntervalMs));
    }
}
}

void ina_service_init(bool i2c_available)
{
    if (!i2c_available) {
        Serial.println("[INA226] Unavailable: I2C bus failed");
        return;
    }

    for (size_t i = 0; i < INA_SENSOR_COUNT; ++i) {
        reading_queues[i] = xQueueCreate(1, sizeof(InaReading));
        if (!reading_queues[i]) {
            Serial.println("[INA226] Unable to create INA226 queue");
            for (size_t j = 0; j < i; ++j) {
                vQueueDelete(reading_queues[j]);
                reading_queues[j] = nullptr;
            }
            return;
        }
    }
    if (xTaskCreate(ina_task, "ina226", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[INA226] Unable to start INA226 task");
        for (size_t i = 0; i < INA_SENSOR_COUNT; ++i) {
            vQueueDelete(reading_queues[i]);
            reading_queues[i] = nullptr;
        }
    }
}

bool ina_service_read(InaSensor sensor, InaReading *destination)
{
    return sensor < INA_SENSOR_COUNT && reading_queues[sensor] && destination &&
           xQueueReceive(reading_queues[sensor], destination, 0) == pdTRUE;
}

bool ina_service_latest(InaSensor sensor, InaReading *destination)
{
    if (sensor >= INA_SENSOR_COUNT || !destination) {
        return false;
    }
    portENTER_CRITICAL(&latest_lock);
    const bool valid = latest_valid[sensor];
    *destination = latest_readings[sensor];
    portEXIT_CRITICAL(&latest_lock);
    return valid;
}
