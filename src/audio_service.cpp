#include "audio_service.h"

#include "board_config.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
struct AudioBuffer {
    uint8_t *bytes;
    size_t size;
};

struct AudioStatus {
    char text[128];
};

QueueHandle_t playback_queue;
QueueHandle_t status_queue;

void publish_status(const char *text)
{
    AudioStatus status{};
    strlcpy(status.text, text, sizeof(status.text));
    if (status_queue && xQueueOverwrite(status_queue, &status) != pdTRUE) {
        Serial.println("Unable to publish audio status");
    }
}

void audio_task(void *)
{
    I2SClass output;
    output.setPins(AUDIO_GPIO_BCLK, AUDIO_GPIO_LRCLK, AUDIO_GPIO_DOUT);
    bool output_ready = output.begin(
        I2S_MODE_STD, 48000, I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    if (!output_ready) {
        publish_status("I2S output initialization failed");
    }

    AudioBuffer buffer{};
    for (;;) {
        if (xQueueReceive(playback_queue, &buffer, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!output_ready) {
            publish_status("Audio output unavailable");
            free(buffer.bytes);
            continue;
        }

        publish_status("Playing /audio/demo.wav");
        digitalWrite(AUDIO_GPIO_AMP, AUDIO_AMP_ON_LEVEL);
        size_t written = 0;
        while (written < buffer.size) {
            const size_t chunk = std::min<size_t>(1024, buffer.size - written);
            const size_t result = output.write(buffer.bytes + written, chunk);
            if (result == 0) {
                output_ready = false;
                break;
            }
            written += result;
            vTaskDelay(1);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(AUDIO_GPIO_AMP, !AUDIO_AMP_ON_LEVEL);
        free(buffer.bytes);
        publish_status(output_ready ? "Playback finished"
                                    : "I2S write failed / audio stopped");
    }
}
}

void audio_service_init()
{
    playback_queue = xQueueCreate(1, sizeof(AudioBuffer));
    status_queue = xQueueCreate(1, sizeof(AudioStatus));
    if (!playback_queue || !status_queue) {
        Serial.println("Unable to create audio queues");
        if (playback_queue) {
            vQueueDelete(playback_queue);
            playback_queue = nullptr;
        }
        if (status_queue) {
            vQueueDelete(status_queue);
            status_queue = nullptr;
        }
        return;
    }

    pinMode(AUDIO_GPIO_AMP, OUTPUT);
    digitalWrite(AUDIO_GPIO_AMP, !AUDIO_AMP_ON_LEVEL);

    publish_status("Audio ready / 48 kHz stereo WAV");
    if (xTaskCreate(audio_task, "audio", 6144, nullptr, 2, nullptr) != pdPASS) {
        Serial.println("Unable to start audio task");
        publish_status("Unable to start audio task");
        vQueueDelete(playback_queue);
        playback_queue = nullptr;
    }
}

bool audio_service_submit(uint8_t *pcm, size_t size)
{
    if (!pcm || size == 0 || !playback_queue) {
        return false;
    }

    const AudioBuffer buffer{pcm, size};
    return xQueueSend(playback_queue, &buffer, 0) == pdTRUE;
}

bool audio_service_read_status(char *destination, size_t capacity)
{
    if (!status_queue || !destination || capacity == 0) {
        return false;
    }

    AudioStatus status{};
    if (xQueueReceive(status_queue, &status, 0) != pdTRUE) {
        return false;
    }
    strlcpy(destination, status.text, capacity);
    return true;
}
