#include "ina_service.h"

#include "board_config.h"

#include <Arduino.h>
#include <INA226.h>
#include <Preferences.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>

namespace
{
constexpr uint32_t ReadIntervalMs = 1000;
// A sensor that was there is looked for again quickly; one that was never found only now and
// then: probing an absent address gets a NACK, and the next I2C transfer on the bus (the other
// sensor) sometimes fails right after it.
constexpr uint32_t RetryIntervalMs = 5000;
constexpr uint32_t MissingRetryIntervalMs = 30000;
// Failed reads in a row before a sensor counts as disconnected; until then the last good
// reading stays on the dashboard, so a single I2C hiccup does not show up as "--".
constexpr uint8_t MaxFailedReads = 3;
constexpr uint32_t LogIntervalMs = 5000;

enum class Chip { Ina226, Ina260 };

struct SensorConfig {
    const char *name;
    Chip chip;
    uint8_t address;
    float shunt_ohms; // INA226 only: the INA260 has its shunt built in.
    float max_current_a;
};

const SensorConfig Sensors[INA_SENSOR_COUNT] = {
    {"INA226 solar", Chip::Ina226, INA_ADDRESS, INA_SHUNT_OHMS, INA_MAX_CURRENT_A},
    {"INA260 battery", Chip::Ina260, INA2_ADDRESS, 0.0f, INA2_MAX_CURRENT_A},
};

// The INA260 has another register map than the INA226 (register 1 is current, not shunt
// voltage, and there is no calibration), so it is read here directly instead of with the
// INA226 library.
namespace ina260
{
constexpr uint8_t RegConfig = 0x00;
constexpr uint8_t RegCurrent = 0x01;
constexpr uint8_t RegBus = 0x02;
constexpr uint8_t RegPower = 0x03;
constexpr uint8_t RegManufacturer = 0xFE;
constexpr uint8_t RegDie = 0xFF;
constexpr uint16_t ManufacturerTi = 0x5449;
constexpr uint16_t DieIna260 = 0x2270;
// 16 samples averaged, 1.1 ms conversions for current and voltage, both continuous.
constexpr uint16_t Config = 0x6000 | (0b010 << 9) | (0b100 << 6) | (0b100 << 3) | 0b111;
constexpr float CurrentLsbA = 0.00125f;
constexpr float BusLsbV = 0.00125f;
constexpr float PowerLsbW = 0.010f;

bool read(uint8_t address, uint8_t reg, uint16_t *value)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, uint8_t{2}) != 2) {
        return false;
    }
    const uint8_t high = Wire.read();
    const uint8_t low = Wire.read();
    *value = static_cast<uint16_t>(high << 8 | low);
    return true;
}

bool write(uint8_t address, uint8_t reg, uint16_t value)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(static_cast<uint8_t>(value >> 8));
    Wire.write(static_cast<uint8_t>(value & 0xFF));
    return Wire.endTransmission() == 0;
}

bool configure(const SensorConfig &config)
{
    uint16_t manufacturer = 0;
    uint16_t die = 0;
    uint16_t written = 0;
    if (!read(config.address, RegManufacturer, &manufacturer) ||
        !read(config.address, RegDie, &die)) {
        return false;
    }
    if (manufacturer != ManufacturerTi || (die & 0xFFF0) != DieIna260) {
        Serial.printf("[%s] Chip at 0x%02X is not an INA260 (id 0x%04X 0x%04X)\n",
                      config.name, config.address, manufacturer, die);
        return false;
    }
    if (!write(config.address, RegConfig, Config) ||
        !read(config.address, RegConfig, &written) || written != Config) {
        Serial.printf("[%s] Configuration failed (config=0x%04X)\n", config.name, written);
        return false;
    }
    Serial.printf("[%s] Ready at 0x%02X / config 0x%04X / max %.1f A\n", config.name,
                  config.address, written, config.max_current_a);
    return true;
}

bool measure(const SensorConfig &config, InaReading *reading)
{
    uint16_t configuration = 0;
    uint16_t current = 0;
    uint16_t bus = 0;
    uint16_t power = 0;
    // A chip that lost power comes back in its default mode: the config check catches that.
    if (!read(config.address, RegConfig, &configuration) || configuration != Config ||
        !read(config.address, RegCurrent, &current) || !read(config.address, RegBus, &bus) ||
        !read(config.address, RegPower, &power)) {
        return false;
    }
    reading->current = static_cast<int16_t>(current) * CurrentLsbA;
    reading->voltage = bus * BusLsbV;
    reading->power = power * PowerLsbW;
    return true;
}
}

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

// State of charge of a Li-ion/LiPo cell from its resting voltage (typical curve at room
// temperature), linearly interpolated between the points.
float battery_percent_from_rest_voltage(float volts)
{
    struct Point {
        float volts;
        float percent;
    };
    static const Point Curve[] = {
        {3.27f, 0},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20}, {3.75f, 25},
        {3.77f, 30}, {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50}, {3.85f, 55},
        {3.87f, 60}, {3.91f, 65}, {3.95f, 70}, {3.98f, 75}, {4.02f, 80}, {4.08f, 85},
        {4.11f, 90}, {4.15f, 95}, {4.20f, 100},
    };
    constexpr size_t Count = sizeof(Curve) / sizeof(Curve[0]);
    if (volts <= Curve[0].volts) {
        return 0;
    }
    for (size_t i = 1; i < Count; ++i) {
        if (volts <= Curve[i].volts) {
            const Point &low = Curve[i - 1];
            const Point &high = Curve[i];
            return low.percent +
                   (volts - low.volts) / (high.volts - low.volts) * (high.percent - low.percent);
        }
    }
    return 100;
}

// Battery state of charge: coulomb counting with the INA260 current (positive = the battery
// supplies the system, negative = it is being charged), slowly corrected by the voltage, and
// set to 100% when charging has finished. The value is kept in flash across restarts.
class BatteryGauge {
public:
    void update(InaReading *reading)
    {
        if (!reading->connected) {
            return;
        }
        const uint32_t now = millis();
        // Under load the cell voltage sags by current * internal resistance (and rises while
        // charging), so take that out before looking up the curve.
        const float rest_volts = reading->voltage + reading->current * BATTERY_INTERNAL_OHMS;
        const float voltage_percent = battery_percent_from_rest_voltage(rest_volts);

        if (!started_) {
            start(voltage_percent, reading->current);
            last_ms_ = now;
        } else {
            // A gap (I2C error, reconnect) is counted as at most 5 s, so it cannot jump far.
            const float hours = std::min(now - last_ms_, uint32_t{5000}) / 3600000.0f;
            last_ms_ = now;
            percent_ -= reading->current * 1000.0f * hours / BATTERY_CAPACITY_MAH * 100.0f;
            correct(voltage_percent, reading->current, reading->voltage, hours * 3600.0f);
        }
        percent_ = std::min(100.0f, std::max(0.0f, percent_));
        save_if_changed();

        reading->has_battery_percent = true;
        reading->battery_percent = static_cast<uint8_t>(std::lround(percent_));
    }

private:
    static constexpr float RestCurrentA = 0.05f;
    // Charging is done when the cell is at its charge voltage and the charge current has
    // dropped (constant-voltage phase): then it is full.
    static constexpr float FullVolts = 4.15f;
    static constexpr float FullTaperCurrentA = 0.25f;
    static constexpr uint32_t FullHoldMs = 60000;
    static constexpr uint32_t SettleWaitMs = 120000;
    static constexpr uint32_t SettleMs = 600000;

    void start(float voltage_percent, float current)
    {
        Preferences storage;
        float saved = -1.0f;
        if (storage.begin("battery", true)) {
            saved = storage.getFloat("percent", -1.0f);
            storage.end();
        }
        // The board is usually off while the sun charges the battery, so the saved value is
        // then too low: the voltage may always raise it. With nothing drawn while off, the
        // charge cannot have dropped much, so the saved value stays unless the voltage says
        // far less. While charging the voltage reads too high: start from the saved value then,
        // and let the voltage settle it quickly once charging stops.
        const bool charging = current < -RestCurrentA;
        const char *source;
        if (saved < 0.0f) {
            percent_ = voltage_percent;
            source = "from voltage, nothing saved";
            settle_ = charging;
        } else if (charging) {
            percent_ = saved;
            source = "saved value, charging: settles after charging";
            settle_ = true;
        } else if (voltage_percent > saved || saved - voltage_percent > 20.0f) {
            percent_ = voltage_percent;
            source = voltage_percent > saved ? "from voltage, charged while off"
                                             : "from voltage, saved value too high";
        } else {
            percent_ = saved;
            source = "saved value";
        }
        saved_percent_ = saved;
        started_ = true;
        Serial.printf("[Battery] Start at %.1f%% (%s; saved %.1f%%, voltage says %.1f%%)\n",
                      percent_, source, saved, voltage_percent);
    }

    void correct(float voltage_percent, float current, float volts, float seconds)
    {
        const uint32_t now = millis();
        const bool charging = current < -RestCurrentA;
        if (current <= 0.0f && volts >= FullVolts && -current < FullTaperCurrentA) {
            if (full_since_ms_ == 0) {
                full_since_ms_ = now;
            }
            if (now - full_since_ms_ >= FullHoldMs) {
                percent_ = 100.0f;
                settle_ = false;
                return;
            }
        } else {
            full_since_ms_ = 0;
        }
        // How fast the voltage may pull the count: little under load (internal resistance is
        // only an estimate), fast at rest and near empty (the curve is steep there), never
        // while charging (the voltage reads too high then).
        float time_constant_s;
        if (charging) {
            discharging_since_ms_ = 0;
            return;
        }
        if (discharging_since_ms_ == 0) {
            discharging_since_ms_ = now;
        }
        const uint32_t discharging_ms = now - discharging_since_ms_;
        if (settle_ && discharging_ms < SettleWaitMs) {
            // Right after charging the voltage is still a little too high: wait for it.
            return;
        }
        if (settle_ && discharging_ms < SettleWaitMs + SettleMs) {
            // Started while charging, so the saved value is likely too low: follow the voltage.
            time_constant_s = 120.0f;
        } else if (settle_) {
            settle_ = false;
            time_constant_s = 7200.0f;
        } else if (voltage_percent < 10.0f) {
            time_constant_s = 120.0f;
        } else if (std::fabs(current) < RestCurrentA) {
            time_constant_s = 600.0f;
        } else {
            time_constant_s = 7200.0f;
        }
        percent_ += (voltage_percent - percent_) * std::min(1.0f, seconds / time_constant_s);
    }

    // Writes to flash only per whole percent, to spare it.
    void save_if_changed()
    {
        if (std::fabs(percent_ - saved_percent_) < 1.0f) {
            return;
        }
        Preferences storage;
        if (storage.begin("battery", false)) {
            storage.putFloat("percent", percent_);
            storage.end();
            saved_percent_ = percent_;
        }
    }

    bool started_ = false;
    float percent_ = 0.0f;
    float saved_percent_ = -1.0f;
    uint32_t last_ms_ = 0;
    uint32_t full_since_ms_ = 0;
    uint32_t discharging_since_ms_ = 0;
    bool settle_ = false;
};

BatteryGauge battery_gauge;

void update_battery_percent(InaReading *reading)
{
    battery_gauge.update(reading);
}

bool configure(INA226 &meter, const SensorConfig &config)
{
    if (config.chip == Chip::Ina260) {
        return ina260::configure(config);
    }
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
    // Only used for the INA226 sensors; an INA260 is read with the ina260 functions.
    INA226 meters[INA_SENSOR_COUNT] = {
        INA226(Sensors[INA_SOLAR].address, &Wire),
        INA226(Sensors[INA_BATTERY].address, &Wire),
    };
    bool configured[INA_SENSOR_COUNT] = {};
    bool found_once[INA_SENSOR_COUNT] = {};
    uint8_t failed_reads[INA_SENSOR_COUNT] = {};
    InaReading last_good[INA_SENSOR_COUNT] = {};
    uint32_t last_retry[INA_SENSOR_COUNT] = {};
    bool tried[INA_SENSOR_COUNT] = {};
    uint32_t last_log = 0;

    auto measure = [&](size_t i, InaReading *reading) {
        const SensorConfig &config = Sensors[i];
        if (config.chip == Chip::Ina260) {
            return ina260::measure(config, reading);
        }
        reading->voltage = meters[i].getBusVoltage();
        reading->current = meters[i].getCurrent();
        reading->power = meters[i].getPower();
        // The chip stays silent on a lost connection, so check before trusting the values.
        return meters[i].getLastError() == 0 && meters[i].isConnected();
    };

    for (;;) {
        const uint32_t now = millis();
        const bool log_now = now - last_log >= LogIntervalMs;
        for (size_t i = 0; i < INA_SENSOR_COUNT; ++i) {
            const SensorConfig &config = Sensors[i];
            InaReading reading{};
            // A missing sensor is retried now and then, so it does not slow the other one.
            const uint32_t retry_interval =
                found_once[i] ? RetryIntervalMs : MissingRetryIntervalMs;
            if (!configured[i] && (!tried[i] || now - last_retry[i] >= retry_interval)) {
                tried[i] = true;
                last_retry[i] = now;
                configured[i] = configure(meters[i], config);
                found_once[i] = found_once[i] || configured[i];
            }
            bool ok = false;
            if (configured[i]) {
                ok = measure(i, &reading);
                if (!ok) {
                    // One more try straight away, configuring again in case the chip reset.
                    vTaskDelay(pdMS_TO_TICKS(5));
                    reading = InaReading{};
                    ok = configure(meters[i], config) && measure(i, &reading);
                }
            }
            if (ok) {
                reading.connected = true;
                reading.over_range = std::fabs(reading.current) > config.max_current_a;
                failed_reads[i] = 0;
            } else if (configured[i] && ++failed_reads[i] < MaxFailedReads) {
                // Keep showing the last good values for a short hiccup.
                reading = last_good[i];
            } else if (configured[i]) {
                configured[i] = false;
                failed_reads[i] = 0;
                last_retry[i] = now;
            }
            if (ok && i == INA_BATTERY) {
                update_battery_percent(&reading);
            }
            if (ok) {
                last_good[i] = reading;
            }
            publish_reading(i, reading);

            if (log_now) {
                if (reading.connected) {
                    char percent[16] = "";
                    if (reading.has_battery_percent) {
                        snprintf(percent, sizeof(percent), " / %u%%", reading.battery_percent);
                    }
                    Serial.printf("[%s] %.2f V / %.3f A / %.2f W%s%s%s\n", config.name,
                                  reading.voltage, reading.current, reading.power, percent,
                                  reading.over_range ? " / OVER RANGE" : "",
                                  ok ? "" : " / read failed, showing last value");
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
