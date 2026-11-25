#include "wifi_service.h"

#include "time_service.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstring>

namespace
{
struct WifiCommand
{
    char ssid[33];
    char password[65];
};

struct WifiStatus
{
    char text[160];
};

QueueHandle_t commands;
QueueHandle_t statuses;
void (*host_ready_callback)();

void publish_status(const char *text)
{
    WifiStatus status{};
    strlcpy(status.text, text, sizeof(status.text));
    if (statuses && xQueueOverwrite(statuses, &status) != pdTRUE) {
        Serial.println("[WiFi] Unable to publish WiFi status");
    }
}

void wifi_task(void *)
{
    Preferences preferences;
    const bool storage_ready = preferences.begin("crow-wifi", false);
    WifiCommand command{};
    if (storage_ready) {
        const size_t stored_size = preferences.getBytes("credentials", &command, sizeof(command));
        if (stored_size != sizeof(command) || command.ssid[sizeof(command.ssid) - 1] != '\0' ||
            command.password[sizeof(command.password) - 1] != '\0') {
            command = {};
        }
    }

    WiFi.setPins(18, 19, 17, 16, 15, 14, 32);
    const bool wifi_available = WiFi.mode(WIFI_STA);
    host_ready_callback();
    if (!storage_ready) {
        publish_status("WiFi settings storage unavailable");
    } else if (wifi_available && command.ssid[0]) {
        WiFi.begin(command.ssid, command.password);
        publish_status("Connecting to saved WiFi...");
    } else {
        publish_status(wifi_available ? "Not configured / tap WiFi setup"
                                      : "C6 unavailable / check ESP-Hosted firmware");
    }

    uint32_t report_at = 0;
    uint32_t connect_at = millis();
    bool save_on_connect = false;
    for (;;) {
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(100)) == pdTRUE && wifi_available) {
            save_on_connect = WiFi.STA.connect(command.ssid, command.password);
            connect_at = millis() - (save_on_connect ? 0U : 20001U);
            if (!save_on_connect) {
                publish_status("Connection request failed / retry setup");
                report_at = millis();
            }
        }

        if (millis() - report_at >= 2000) {
            char text[160];
            if (WiFi.STA.connected() && WiFi.STA.hasIP()) {
                // With an IP address the network stack is up: NTP can start (only once).
                time_service_start_ntp();
                snprintf(text, sizeof(text), "%s / %s", WiFi.SSID().c_str(),
                         WiFi.localIP().toString().c_str());
                if (save_on_connect && WiFi.SSID() == command.ssid) {
                    const bool saved =
                        storage_ready &&
                        preferences.putBytes("credentials", &command, sizeof(command)) == sizeof(command);
                    if (!saved) {
                        snprintf(text, sizeof(text), "Connected; saving credentials failed");
                    }
                    save_on_connect = false;
                }
            } else if (!wifi_available) {
                strlcpy(text, "C6 unavailable / check firmware and restart", sizeof(text));
            } else if (command.ssid[0]) {
                strlcpy(text, millis() - connect_at > 20000 ? "Connection failed / tap WiFi setup"
                                                            : "Connecting...",
                        sizeof(text));
            } else {
                strlcpy(text, "Not configured / tap WiFi setup", sizeof(text));
            }
            publish_status(text);
            report_at = millis();
        }
    }
}
}

void wifi_init(void (*on_host_ready)())
{
    host_ready_callback = on_host_ready;
    commands = xQueueCreate(1, sizeof(WifiCommand));
    statuses = xQueueCreate(1, sizeof(WifiStatus));
    if (!commands || !statuses) {
        Serial.println("[WiFi] Unable to create WiFi queues");
        on_host_ready();
        return;
    }

    const BaseType_t created = xTaskCreate(wifi_task, "wifi", 8192, nullptr, 1, nullptr);
    if (created != pdPASS) {
        Serial.println("[WiFi] Unable to start WiFi task");
        on_host_ready();
        vQueueDelete(commands);
        vQueueDelete(statuses);
        commands = nullptr;
        statuses = nullptr;
    }
}

bool wifi_submit_credentials(const char *ssid, const char *password)
{
    if (!commands || !ssid || !password || strlen(ssid) >= sizeof(WifiCommand::ssid) ||
        strlen(password) >= sizeof(WifiCommand::password)) {
        return false;
    }

    WifiCommand command{};
    strlcpy(command.ssid, ssid, sizeof(command.ssid));
    strlcpy(command.password, password, sizeof(command.password));
    return xQueueOverwrite(commands, &command) == pdTRUE;
}

bool wifi_read_status(char *output, size_t output_size)
{
    if (!statuses || !output || output_size == 0) {
        return false;
    }

    WifiStatus status{};
    if (xQueueReceive(statuses, &status, 0) != pdTRUE) {
        return false;
    }
    strlcpy(output, status.text, output_size);
    return true;
}
