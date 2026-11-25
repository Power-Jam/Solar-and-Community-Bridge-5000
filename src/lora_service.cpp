#include "lora_service.h"

#include "board_config.h"
#include "gps_service.h"
#include "ina_service.h"
#include "temp_service.h"

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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

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

constexpr uint8_t AllUplinks = static_cast<uint8_t>(LoraUplink::Temperature) |
                               static_cast<uint8_t>(LoraUplink::Energy) |
                               static_cast<uint8_t>(LoraUplink::Gps);

struct Uplink {
    uint8_t port;
    uint8_t length;
    uint8_t data[16];
};

const char *uplink_name(uint8_t uplink)
{
    switch (static_cast<LoraUplink>(uplink)) {
    case LoraUplink::Temperature:
        return "Temperature";
    case LoraUplink::Energy:
        return "Energy";
    case LoraUplink::Gps:
        return "GPS";
    }
    return "Uplink";
}

// Values are scaled to integers and sent big-endian to keep the airtime per uplink low.
template <typename T> void put(Uplink &uplink, T value)
{
    for (size_t i = sizeof(T); i-- > 0;) {
        uplink.data[uplink.length++] =
            static_cast<uint8_t>(static_cast<uint64_t>(value) >> (i * 8));
    }
}

template <typename T> T scaled(double value, double scale)
{
    const double result = std::round(value * scale);
    return static_cast<T>(std::min<double>(
        std::max<double>(result, std::numeric_limits<T>::min()),
        std::numeric_limits<T>::max()));
}

// Returns nullptr when the uplink was built, otherwise why the reading cannot be sent.
const char *build_uplink(uint8_t kind, Uplink &uplink)
{
    uplink = {};
    switch (static_cast<LoraUplink>(kind)) {
    case LoraUplink::Temperature: {
        // FPort 10: int16 temperature in 0.01 C.
        float celsius = 0;
        if (!temp_service_latest(&celsius)) {
#if TEMP_MOCK_WHEN_MISSING
            // Mock reading: 21.00 C .. 23.99 C so successive uplinks differ.
            celsius = 21.0f + static_cast<float>(esp_random() % 300) / 100.0f;
            Serial.printf("[LoRa] DS18B20 missing, sending mock temperature %.2f C\n", celsius);
#else
            return "no current DS18B20 reading";
#endif
        }
        uplink.port = 10;
        put(uplink, scaled<int16_t>(celsius, 100));
        return nullptr;
    }
    case LoraUplink::Energy: {
        // FPort 20, 1 byte: uint8 battery charge in %.
        InaReading battery{};
        if (!ina_service_latest(INA_BATTERY, &battery) || !battery.connected ||
            !battery.has_battery_percent) {
            return "no battery charge yet";
        }
        uplink.port = 20;
        put(uplink, battery.battery_percent);
        return nullptr;
    }
    case LoraUplink::Gps: {
        // FPort 40: int32 latitude and longitude in 1e-7 degrees, uint8 satellites (255 = unknown).
        GpsFix fix{};
        if (!gps_service_latest(&fix)) {
            return "no fresh GPS fix";
        }
        uplink.port = 40;
        put(uplink, scaled<int32_t>(fix.latitude, 10000000));
        put(uplink, scaled<int32_t>(fix.longitude, 10000000));
        put(uplink, static_cast<uint8_t>(fix.satellites_valid ? fix.satellites : 255));
        return nullptr;
    }
    }
    return "unknown uplink type";
}

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
    static_assert(LORA_JOIN_START_SF >= 7 && LORA_JOIN_START_SF <= 12,
                  "LORA_JOIN_START_SF must be 7..12");
    uint8_t datarate = 12 - LORA_JOIN_START_SF;
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

    uint32_t wait_ms() const
    {
        const int32_t remaining = static_cast<int32_t>(allowed_at_ - millis());
        return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
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
    uint8_t pending = 0;

    // Automatic uplinks: each type is queued when its time has come, and the airtime limit
    // spaces them out like button presses. The first ones go right after joining TTN.
    struct AutoUplink {
        uint8_t kind;
        uint32_t interval_ms;
        uint32_t retry_ms;
        uint32_t due_ms;
        bool started;
    };
    AutoUplink schedule[] = {
        {static_cast<uint8_t>(LoraUplink::Energy), LORA_AUTO_BATTERY_MINUTES * 60000UL,
         LORA_AUTO_RETRY_MINUTES * 60000UL},
        {static_cast<uint8_t>(LoraUplink::Temperature), LORA_AUTO_TEMPERATURE_MINUTES * 60000UL,
         LORA_AUTO_RETRY_MINUTES * 60000UL},
        {static_cast<uint8_t>(LoraUplink::Gps), LORA_AUTO_GPS_MINUTES * 60000UL,
         LORA_AUTO_GPS_RETRY_MINUTES * 60000UL},
    };
    // After a send the next one is due after the interval; when there was nothing to send
    // (no fix, no reading) after the shorter retry time.
    auto reschedule = [&](uint8_t kind, bool sent) {
        for (AutoUplink &entry : schedule) {
            if (entry.kind == kind && entry.interval_ms) {
                const uint32_t delay_ms =
                    sent ? entry.interval_ms : std::min(entry.interval_ms, entry.retry_ms);
                entry.due_ms = millis() + delay_ms;
                entry.started = true;
            }
        }
    };

    // The automatic location goes once per calendar day: the GPS date of the last one is kept
    // in flash, so restarts on the same day do not send it again. A button press always sends.
    constexpr uint8_t GpsKind = static_cast<uint8_t>(LoraUplink::Gps);
    bool gps_is_automatic = false;
    uint32_t gps_sent_date = storage.getUInt("gps_date", 0);

    for (;;) {
        if (node.isActivated()) {
            const uint32_t now = millis();
            for (AutoUplink &entry : schedule) {
                if (entry.interval_ms &&
                    (!entry.started || static_cast<int32_t>(now - entry.due_ms) >= 0)) {
                    entry.started = true;
                    // Pushed back here so it is not queued again every loop while it waits
                    // for airtime; set properly once it has been sent.
                    entry.due_ms = now + entry.interval_ms;
                    if (!(pending & entry.kind)) {
                        Serial.printf("[LoRa] Automatic %s uplink\n", uplink_name(entry.kind));
                        if (entry.kind == GpsKind) {
                            gps_is_automatic = true;
                        }
                    }
                    pending |= entry.kind;
                }
            }
        }

        if (!node.isActivated() && node.can_transmit()) {
            // EU868 DR0..DR5 at 125 kHz are SF12..SF7.
            const uint8_t join_dr = join_datarate(join_retry_ms);
            const unsigned join_sf = 12U - join_dr;
            char join_status[160];
            snprintf(join_status, sizeof(join_status),
                     "Joining TTN / LoRaWAN 1.1 / SF%u...", join_sf);
            publish_status(join_status);
            result = node.setDatarate(join_dr);
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
                    snprintf(join_status, sizeof(join_status),
                             "Connected to TTN / LoRaWAN 1.1 / SF%u", join_sf);
                    publish_status(join_status);
                } else {
                    snprintf(join_status, sizeof(join_status),
                             "TTN join failed at SF%u (%d) / retrying", join_sf, result);
                    publish_status(join_status);
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
        while (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
            command &= AllUplinks;
            Serial.printf("[LoRa] %s uplink requested (%s)\n",
                          uplink_name(command),
                          node.isActivated() ? "joined" : "not joined yet");
            if (!node.isActivated() || !node.can_transmit()) {
                char status[160];
                if (!node.isActivated()) {
                    snprintf(status, sizeof(status),
                             "%s queued / waiting for TTN join",
                             uplink_name(command));
                } else {
                    snprintf(status, sizeof(status),
                             "%s queued / airtime limit, sending in %lu s",
                             uplink_name(command),
                             static_cast<unsigned long>(node.wait_ms() / 1000 + 1));
                }
                publish_status(status);
            }
            if (command & GpsKind) {
                gps_is_automatic = false;
            }
            pending |= command;
        }
        if (pending && node.isActivated() && node.can_transmit()) {
            // Send the queued types one at a time, lowest bit first, as airtime allows.
            const uint8_t kind = pending & static_cast<uint8_t>(-pending);
            pending &= static_cast<uint8_t>(~kind);

            if (kind == GpsKind && gps_is_automatic) {
                gps_is_automatic = false;
                GpsFix fix{};
                if (!gps_service_latest(&fix)) {
                    publish_status("GPS not sent / waiting for a GPS fix");
                    reschedule(kind, false);
                    continue;
                }
                if (fix.date != 0 && fix.date == gps_sent_date) {
                    Serial.printf("[LoRa] Location already sent today (%lu)\n",
                                  static_cast<unsigned long>(fix.date));
                    reschedule(kind, true);
                    continue;
                }
            }

            Uplink uplink{};
            const char *error = build_uplink(kind, uplink);
            if (error) {
                char status[160];
                snprintf(status, sizeof(status), "%s not sent / %s",
                         uplink_name(kind), error);
                publish_status(status);
                reschedule(kind, false);
                continue;
            }
            reschedule(kind, true);

            uint8_t downlink[255] = {};
            size_t downlink_length = sizeof(downlink);
            LoRaWANEvent_t uplink_event{}, downlink_event{};
            char status[160];
            snprintf(status, sizeof(status),
                     "Sending %s / waiting for RX windows...", uplink_name(kind));
            publish_status(status);
            node.scheduleTransmission(millis());
            result = node.sendReceive(uplink.data, uplink.length, uplink.port,
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

            if (uplink_event.fPort == uplink.port && result >= 0) {
                snprintf(status, sizeof(status),
                         "%s sent / fPort %u / RX status %d",
                         uplink_name(kind), uplink.port, result);
                GpsFix fix{};
                if (kind == GpsKind && gps_service_latest(&fix) && fix.date != 0 &&
                    fix.date != gps_sent_date) {
                    gps_sent_date = fix.date;
                    storage.putUInt("gps_date", gps_sent_date);
                }
            } else {
                snprintf(status, sizeof(status),
                         "%s uplink failed (%d) / retry with button",
                         uplink_name(kind), result);
            }
            publish_status(status);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}

void lora_service_init()
{
    // Buffers button presses while a send blocks the task; it merges them into a pending set.
    command_queue = xQueueCreate(8, sizeof(uint8_t));
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

bool lora_service_request_uplink(LoraUplink uplink)
{
    const uint8_t request = static_cast<uint8_t>(uplink);
    return command_queue && xQueueSend(command_queue, &request, 0) == pdTRUE;
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
