#include "lora_service.h"

#include "board_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace
{
struct StatusMessage {
    char text[160];
};

QueueHandle_t command_queue;
QueueHandle_t status_queue;
QueueHandle_t downlink_queue;

struct DownlinkMessage {
    char text[256];
};

void publish_status(const char *text)
{
    Serial.printf("[LoRa] %s\n", text);
    StatusMessage status{};
    strlcpy(status.text, text, sizeof(status.text));
    if (status_queue && xQueueOverwrite(status_queue, &status) != pdTRUE) {
        Serial.println("[LoRa] Unable to publish LoRaWAN status");
    }
}

void publish_downlink(uint8_t port, const uint8_t *data, size_t length)
{
    DownlinkMessage message{};
    int used = snprintf(message.text, sizeof(message.text), "FPort %u: ", port);
    bool printable = length < sizeof(message.text) - static_cast<size_t>(used) - 1;
    for (size_t i = 0; i < length && printable; ++i) {
        printable = data[i] >= 32 && data[i] <= 126;
    }

    if (printable) {
        for (size_t i = 0; i < length; ++i) {
            message.text[used++] = static_cast<char>(data[i]);
        }
        message.text[used] = '\0';
    } else {
        for (size_t i = 0; i < length && used + 4 < sizeof(message.text); ++i) {
            used += snprintf(message.text + used, sizeof(message.text) - used,
                             "%02X ", data[i]);
        }
        if (length > (sizeof(message.text) - static_cast<size_t>(used)) / 3) {
            strlcat(message.text, "...", sizeof(message.text));
        }
    }

    Serial.printf("[LoRa] Downlink received / %s\n", message.text);
    if (!downlink_queue || xQueueOverwrite(downlink_queue, &message) != pdTRUE) {
        Serial.println("[LoRa] Unable to publish LoRaWAN downlink");
    }
}

uint8_t join_datarate(uint32_t retry_ms)
{
    uint8_t datarate = 5;
    while (datarate > 0 && retry_ms >= 120000) {
        --datarate;
        retry_ms /= 2;
    }
    return datarate;
}

uint32_t airtime_delay(uint32_t airtime_ms, uint32_t allowance_ms_per_hour)
{
    if (airtime_ms == 0 || allowance_ms_per_hour == 0) {
        return 0;
    }
    const uint64_t numerator = (static_cast<uint64_t>(airtime_ms) + 1) * 3600000;
    const uint64_t interval =
        (numerator + allowance_ms_per_hour - 1) / allowance_ms_per_hour;
    return interval > INT32_MAX ? INT32_MAX : static_cast<uint32_t>(interval);
}

class PersistentNode : public LoRaWANNode {
public:
    PersistentNode(PhysicalLayer *physical_layer, Preferences &storage)
        : LoRaWANNode(physical_layer, &EU868), storage_(storage)
    {
    }

    bool healthy = true;

    bool can_transmit() const
    {
        return static_cast<int32_t>(millis() - allowed_at_) >= 0;
    }

    void defer(uint64_t delay_ms, uint32_t jitter_ms = 0)
    {
        const uint32_t now = millis();
        const int32_t remaining = static_cast<int32_t>(allowed_at_ - now);
        const uint64_t wait_ms = std::max<uint64_t>(
            delay_ms, remaining > 0 ? static_cast<uint32_t>(remaining) : 0U);
        allowed_at_ = now + static_cast<uint32_t>(
            std::min<uint64_t>(INT32_MAX, wait_ms + jitter_ms));
    }

    int16_t activateOTAA(LoRaWANJoinEvent_t *event) override
    {
        if (devNonce >= UINT16_MAX || !healthy) {
            healthy = false;
            return RADIOLIB_ERR_UNKNOWN;
        }

        const uint16_t next_nonce = devNonce + 1;
        hton<uint16_t>(&bufferNonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE],
                       next_nonce);
        healthy = storage_.putBytes("nonces", getBufferNonces(),
                                    RADIOLIB_LORAWAN_NONCES_BUF_SIZE) ==
                  RADIOLIB_LORAWAN_NONCES_BUF_SIZE;
        hton<uint16_t>(&bufferNonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE],
                       devNonce);
        getBufferNonces();
        if (!healthy) {
            return RADIOLIB_ERR_UNKNOWN;
        }

        scheduleTransmission(millis());
        const int16_t result = LoRaWANNode::activateOTAA(event);
        account_airtime();

        devNonce = std::max(devNonce, next_nonce);
        hton<uint16_t>(&bufferNonces[RADIOLIB_LORAWAN_NONCES_DEV_NONCE],
                       devNonce);
        healthy = storage_.putBytes("nonces", getBufferNonces(),
                                    RADIOLIB_LORAWAN_NONCES_BUF_SIZE) ==
                  RADIOLIB_LORAWAN_NONCES_BUF_SIZE;
        return result;
    }

    void account_airtime()
    {
        const uint32_t airtime = getLastToA();
        const uint32_t allowance = std::min<uint32_t>(
            band->dutyCycle, dutyCycle ? dutyCycle : band->dutyCycle);
        const uint32_t public_allowance =
            std::min<uint32_t>(allowance, LORA_AIRTIME_ALLOWANCE_MS_PER_HOUR);
        defer(static_cast<uint64_t>(airtime_delay(airtime, public_allowance)) +
              airtime);
    }

private:
    Preferences &storage_;
    uint32_t allowed_at_ = 0;
};

void radio_task(void *)
{
    const uint8_t app_key[16] = LORA_APP_KEY;
    const uint8_t network_key[16] = LORA_NWK_KEY;
    uint8_t any_app_key = 0;
    uint8_t any_network_key = 0;
    for (size_t i = 0; i < sizeof(app_key); ++i) {
        any_app_key |= app_key[i];
        any_network_key |= network_key[i];
    }

    // An all-zero JoinEUI is valid on TTN, so only the DevEUI and keys are checked.
    if (LORA_DEV_EUI == 0 || any_app_key == 0 || any_network_key == 0) {
        publish_status("Configure LoRaWAN DevEUI and keys in board_config.h");
        vTaskSuspend(nullptr);
    }

    // The RFID readers poll the global SPI (FSPI) from loop(), so LoRa needs its own bus.
    SPIClass spi(HSPI);
    spi.begin(LORA_GPIO_SCK, LORA_GPIO_MISO, LORA_GPIO_MOSI, LORA_GPIO_CS);
    Module module(LORA_GPIO_CS, LORA_GPIO_IRQ, LORA_GPIO_RESET,
                  LORA_GPIO_BUSY, spi,
                  SPISettings(2000000, MSBFIRST, SPI_MODE0));
    SX1262 radio(&module);
    Preferences storage;
    if (!storage.begin("crow-lora", false)) {
        publish_status("LoRaWAN NVS unavailable; radio stopped");
        vTaskSuspend(nullptr);
    }

    int16_t result = radio.begin(
        868.1, 125, 7, 5, RADIOLIB_SX126X_SYNC_WORD_PUBLIC,
        14, 8, LORA_TCXO_VOLTAGE);
    // Core1262 switch logic is inverted: RXEN=0/TXEN=1 receives, RXEN=1/TXEN=0 transmits.
    radio.setRfSwitchPins(LORA_GPIO_TX_ENABLE, LORA_GPIO_RX_ENABLE);
    if (result == RADIOLIB_ERR_NONE) {
        result = radio.setDio2AsRfSwitch(false);
    }
    if (result != RADIOLIB_ERR_NONE) {
        char status[160];
        snprintf(status, sizeof(status),
                 "LoRa radio init failed (%d) / check module wiring", result);
        publish_status(status);
        vTaskSuspend(nullptr);
    }

    PersistentNode node(&radio, storage);
    result = node.beginOTAA(LORA_JOIN_EUI, LORA_DEV_EUI,
                            network_key, app_key);
    if (result == RADIOLIB_ERR_NONE && storage.isKey("nonces")) {
        uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {};
        const bool loaded =
            storage.getBytes("nonces", nonces, sizeof(nonces)) == sizeof(nonces) &&
            node.setBufferNonces(nonces) == RADIOLIB_ERR_NONE;
        if (!loaded) {
            publish_status("Saved LoRaWAN nonces invalid / radio stopped");
            vTaskSuspend(nullptr);
        }
    }
    if (result != RADIOLIB_ERR_NONE) {
        char status[160];
        snprintf(status, sizeof(status),
                 "LoRaWAN configuration failed (%d)", result);
        publish_status(status);
        vTaskSuspend(nullptr);
    }

    node.setDutyCycle(false);
    node.setADR(true);
    node.scanGuard = 50;

    uint32_t join_retry_ms = 60000;
    bool test_pending = false;
    for (;;) {
        if (!node.isActivated() && node.can_transmit()) {
            publish_status("Joining TTN / LoRaWAN 1.1...");
            result = node.setDatarate(join_datarate(join_retry_ms));
            if (result == RADIOLIB_ERR_NONE) {
                LoRaWANJoinEvent_t event{};
                result = node.activateOTAA(&event);
                const bool joined = result == RADIOLIB_LORAWAN_NEW_SESSION &&
                                    node.getVersionMajor() == 1;
                if (!node.healthy ||
                    (result == RADIOLIB_LORAWAN_NEW_SESSION &&
                     node.getVersionMajor() != 1)) {
                    publish_status("LoRaWAN 1.1 / NVS check failed; radio stopped");
                    vTaskSuspend(nullptr);
                }
                if (joined) {
                    join_retry_ms = 60000;
                    publish_status("Connected to TTN / LoRaWAN 1.1");
                } else {
                    char status[160];
                    snprintf(status, sizeof(status),
                             "TTN join failed (%d) / retrying", result);
                    publish_status(status);
                    node.defer(join_retry_ms, esp_random() % 60001U);
                    join_retry_ms =
                        std::min<uint32_t>(join_retry_ms * 2, 86400000);
                    node.clearSession();
                }
            } else {
                publish_status("LoRaWAN datarate setup failed");
                node.defer(join_retry_ms);
            }
        }

        uint8_t command = 0;
        if (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
            test_pending = true;
            Serial.printf("[LoRa] Test uplink requested (%s)\n",
                          node.isActivated() ? "joined" : "not joined yet");
        }
        if (test_pending && node.isActivated() && node.can_transmit()) {
            uint8_t downlink[255] = {};
            size_t downlink_length = sizeof(downlink);
            LoRaWANEvent_t uplink_event{}, downlink_event{};
            publish_status("Sending test uplink / waiting for RX windows...");
            node.scheduleTransmission(millis());
            result = node.sendReceive(
                reinterpret_cast<const uint8_t *>("TEST"), 4, 2,
                downlink, &downlink_length, false,
                &uplink_event, &downlink_event);
            node.account_airtime();
            if (result > 0) {
                publish_downlink(downlink_event.fPort, downlink,
                                downlink_length);
            }
            if (!node.healthy) {
                publish_status("LoRaWAN NVS write failed; radio stopped");
                vTaskSuspend(nullptr);
            }

            char status[160];
            if (uplink_event.fPort == 2 && result >= 0) {
                snprintf(status, sizeof(status),
                         "Test uplink sent / fPort 2 / RX status %d",
                         result);
            } else {
                snprintf(status, sizeof(status),
                         "Test uplink failed (%d) / retry with button", result);
            }
            publish_status(status);
            test_pending = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}

void lora_service_init()
{
    command_queue = xQueueCreate(1, sizeof(uint8_t));
    status_queue = xQueueCreate(1, sizeof(StatusMessage));
    downlink_queue = xQueueCreate(1, sizeof(DownlinkMessage));
    if (!command_queue || !status_queue || !downlink_queue) {
        Serial.println("[LoRa] Unable to create LoRaWAN queues");
        if (command_queue) {
            vQueueDelete(command_queue);
            command_queue = nullptr;
        }
        if (status_queue) {
            vQueueDelete(status_queue);
            status_queue = nullptr;
        }
        if (downlink_queue) {
            vQueueDelete(downlink_queue);
            downlink_queue = nullptr;
        }
        return;
    }

    publish_status("Starting LoRaWAN...");
    if (xTaskCreate(radio_task, "lora", 12288, nullptr, 2, nullptr) != pdPASS) {
        Serial.println("[LoRa] Unable to start LoRaWAN task");
        vQueueDelete(command_queue);
        vQueueDelete(status_queue);
        vQueueDelete(downlink_queue);
        command_queue = nullptr;
        status_queue = nullptr;
        downlink_queue = nullptr;
    }
}

bool lora_service_request_test_uplink()
{
    const uint8_t request = 1;
    return command_queue && xQueueOverwrite(command_queue, &request) == pdTRUE;
}

bool lora_service_read_status(char *destination, size_t capacity)
{
    if (!status_queue || !destination || capacity == 0) {
        return false;
    }

    StatusMessage status{};
    if (xQueueReceive(status_queue, &status, 0) != pdTRUE) {
        return false;
    }
    strlcpy(destination, status.text, capacity);
    return true;
}

bool lora_service_read_downlink(char *destination, size_t capacity)
{
    if (!downlink_queue || !destination || capacity == 0) {
        return false;
    }

    DownlinkMessage message{};
    if (xQueueReceive(downlink_queue, &message, 0) != pdTRUE) {
        return false;
    }
    strlcpy(destination, message.text, capacity);
    return true;
}
