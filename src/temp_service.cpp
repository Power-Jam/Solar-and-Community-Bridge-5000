#include "temp_service.h"

#include "board_config.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The OneWire library does not compile for the ESP32-P4, so the DS18B20 is driven directly.
// Only one sensor is expected on the bus, which lets every command use Skip ROM.
namespace
{
constexpr uint32_t ReadIntervalMs = 5000;
constexpr uint32_t ConversionMs = 750;

constexpr uint8_t CommandSkipRom = 0xCC;
constexpr uint8_t CommandConvert = 0x44;
constexpr uint8_t CommandReadScratchpad = 0xBE;

constexpr gpio_num_t DataPin = static_cast<gpio_num_t>(TEMP_GPIO_DATA);

portMUX_TYPE timing_lock = portMUX_INITIALIZER_UNLOCKED;

bool bus_reset()
{
    // A line held low would pass as a sensor sending all zeros, which has a valid CRC.
    if (gpio_get_level(DataPin) == 0) {
        return false;
    }

    gpio_set_level(DataPin, 0);
    delayMicroseconds(480);

    portENTER_CRITICAL(&timing_lock);
    gpio_set_level(DataPin, 1);
    delayMicroseconds(70);
    const bool present = gpio_get_level(DataPin) == 0;
    portEXIT_CRITICAL(&timing_lock);

    delayMicroseconds(410);
    return present;
}

void write_bit(bool bit)
{
    portENTER_CRITICAL(&timing_lock);
    gpio_set_level(DataPin, 0);
    delayMicroseconds(bit ? 6 : 60);
    gpio_set_level(DataPin, 1);
    portEXIT_CRITICAL(&timing_lock);
    delayMicroseconds(bit ? 64 : 10);
}

bool read_bit()
{
    portENTER_CRITICAL(&timing_lock);
    gpio_set_level(DataPin, 0);
    delayMicroseconds(3);
    gpio_set_level(DataPin, 1);
    delayMicroseconds(9);
    const bool bit = gpio_get_level(DataPin) != 0;
    portEXIT_CRITICAL(&timing_lock);
    delayMicroseconds(55);
    return bit;
}

void write_byte(uint8_t value)
{
    for (uint8_t i = 0; i < 8; ++i) {
        write_bit(value & (1 << i));
    }
}

uint8_t read_byte()
{
    uint8_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        if (read_bit()) {
            value |= 1 << i;
        }
    }
    return value;
}

// Dallas/Maxim CRC-8; a valid scratchpad including its CRC byte yields 0.
uint8_t crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = crc & 1 ? (crc >> 1) ^ 0x8C : crc >> 1;
        }
    }
    return crc;
}

bool read_temperature(float *celsius)
{
    if (!bus_reset()) {
        return false;
    }
    write_byte(CommandSkipRom);
    write_byte(CommandReadScratchpad);

    uint8_t scratchpad[9];
    for (uint8_t &value : scratchpad) {
        value = read_byte();
    }
    if (crc8(scratchpad, sizeof(scratchpad)) != 0) {
        return false;
    }
    *celsius = static_cast<int16_t>((scratchpad[1] << 8) | scratchpad[0]) / 16.0f;
    return true;
}

void temp_task(void *)
{
    pinMode(TEMP_GPIO_DATA, OUTPUT_OPEN_DRAIN);
    gpio_set_level(DataPin, 1);

    for (;;) {
        if (bus_reset()) {
            write_byte(CommandSkipRom);
            write_byte(CommandConvert);
            vTaskDelay(pdMS_TO_TICKS(ConversionMs));

            float celsius = 0;
            if (read_temperature(&celsius)) {
                Serial.printf("[DS18B20] %.2f C\n", celsius);
            } else {
                Serial.println("[DS18B20] Read failed / CRC error or sensor lost");
            }
        } else {
            Serial.printf("[DS18B20] Not found on GPIO%d / check data, 3V3, GND and 4.7k pull-up\n",
                          TEMP_GPIO_DATA);
        }
        vTaskDelay(pdMS_TO_TICKS(ReadIntervalMs));
    }
}
}

void temp_service_init()
{
    if (xTaskCreate(temp_task, "ds18b20", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[DS18B20] Unable to start DS18B20 task");
    }
}
