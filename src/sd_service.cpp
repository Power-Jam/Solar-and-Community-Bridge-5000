#include "sd_service.h"

#include "audio_service.h"
#include "board_config.h"
#include "asset_storage.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <new>
#include <sys/stat.h>

namespace
{
constexpr size_t MaxJpegBytes = 4 * 1024 * 1024;
constexpr size_t MaxAudioBytes = 4 * 1024 * 1024;
constexpr int MaxImageWidth = 1024;
constexpr int MaxImageHeight = 600;
constexpr int MountAttempts = 5;
constexpr uint32_t MountRetryMs = 500;

struct ImageBuffer {
    uint16_t *pixels;
    int width;
    int height;
};

struct StatusMessage {
    char text[128];
};

QueueHandle_t command_queue;
QueueHandle_t status_queue;
QueueHandle_t background_queue;
const char *const *background_paths = nullptr;
size_t background_count = 0;

int draw_jpeg_block(JPEGDRAW *block)
{
    auto *image = static_cast<ImageBuffer *>(block->pUser);
    if (!image || !image->pixels || block->x < 0 || block->y < 0 ||
        block->x >= image->width || block->y >= image->height) {
        return 0;
    }

    const int copy_width = std::min(block->iWidthUsed, image->width - block->x);
    if (copy_width <= 0) {
        return 1;
    }
    for (int row = 0; row < block->iHeight && block->y + row < image->height; ++row) {
        memcpy(image->pixels + (block->y + row) * image->width + block->x,
               block->pPixels + row * block->iWidth,
               static_cast<size_t>(copy_width) * sizeof(uint16_t));
    }
    return 1;
}

void publish_status(const char *text)
{
    StatusMessage message{};
    strlcpy(message.text, text, sizeof(message.text));
    xQueueOverwrite(status_queue, &message);
}

// JPEGDEC holds ~18 KB of buffers: too big for this task's stack and for internal
// RAM, where the SD driver needs its DMA buffers.
JPEGDEC *jpeg_decoder()
{
    static JPEGDEC *decoder = nullptr;
    if (!decoder) {
        void *memory = ps_malloc(sizeof(JPEGDEC));
        decoder = memory ? new (memory) JPEGDEC : nullptr;
    }
    return decoder;
}

// Layers are stored ready to draw, so they only need reading.
bool load_layer(const String &path, SdImage *result)
{
    const String full_path = String("/sdcard") + path;
    FILE *file = fopen(full_path.c_str(), "rb");
    uint8_t header[12];
    bool ok = file && fread(header, 1, sizeof(header), file) == sizeof(header) &&
              memcmp(header, "LAY1", 4) == 0;
    const auto field = [&header](int index) {
        return static_cast<uint16_t>(header[4 + index * 2] | (header[5 + index * 2] << 8));
    };
    const size_t width = ok ? field(2) : 0;
    const size_t height = ok ? field(3) : 0;
    ok = ok && width > 0 && height > 0 && width <= MaxImageWidth && height <= MaxImageHeight;
    const size_t size = width * height * 3;
    auto *pixels = ok ? static_cast<uint16_t *>(ps_malloc(size)) : nullptr;
    ok = pixels && fread(pixels, 1, size, file) == size;
    if (file) {
        fclose(file);
    }
    if (!ok) {
        free(pixels);
        return false;
    }
    result->pixels = pixels;
    result->width = static_cast<uint16_t>(width);
    result->height = static_cast<uint16_t>(height);
    result->format = SdImageFormat::Rgb565A8;
    result->x = static_cast<int16_t>(field(0));
    result->y = static_cast<int16_t>(field(1));
    return true;
}

bool load_image(const String &path, SdImage *result)
{
    if (path.endsWith(".lay")) {
        return load_layer(path, result);
    }
    const uint32_t read_started = millis();
    const String full_path = String("/sdcard") + path;
    FILE *file = fopen(full_path.c_str(), "rb");
    struct stat info{};
    const bool valid_file = file && fstat(fileno(file), &info) == 0 &&
                            info.st_size > 0 &&
                            static_cast<uint64_t>(info.st_size) <= MaxJpegBytes;
    const size_t size = valid_file ? static_cast<size_t>(info.st_size) : 0;
    auto *compressed = size > 0 ? static_cast<uint8_t *>(ps_malloc(size)) : nullptr;
    bool ok = compressed && fread(compressed, 1, size, file) == size;
    if (file) {
        fclose(file);
    }
    const uint32_t decode_started = millis();

    JPEGDEC *jpeg = ok ? jpeg_decoder() : nullptr;
    ok = jpeg != nullptr;
    ImageBuffer image{};
    int scale = 0;
    bool jpeg_open = false;
    if (ok) {
        jpeg_open = jpeg->openRAM(compressed, static_cast<int>(size), draw_jpeg_block) != 0;
        ok = jpeg_open;
    }
    if (ok) {
        while (scale < 3 &&
               ((jpeg->getWidth() >> scale) > MaxImageWidth ||
                (jpeg->getHeight() >> scale) > MaxImageHeight)) {
            ++scale;
        }
        const int divisor = 1 << scale;
        image.width = (jpeg->getWidth() + divisor - 1) / divisor;
        image.height = (jpeg->getHeight() + divisor - 1) / divisor;
        ok = jpeg->getJPEGType() == JPEG_MODE_BASELINE && image.width > 0 &&
             image.height > 0 && image.width <= MaxImageWidth &&
             image.height <= MaxImageHeight;
    }
    if (ok) {
        image.pixels = static_cast<uint16_t *>(
            ps_calloc(static_cast<size_t>(image.width) * image.height, sizeof(uint16_t)));
        ok = image.pixels != nullptr;
    }
    if (ok) {
        jpeg->setUserPointer(&image);
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        const int scales[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
        ok = jpeg->decode(0, 0, scales[scale]) != 0;
    }
    if (jpeg_open) {
        jpeg->close();
    }
    free(compressed);

    if (!ok) {
        free(image.pixels);
        return false;
    }

    result->pixels = image.pixels;
    result->width = static_cast<uint16_t>(image.width);
    result->height = static_cast<uint16_t>(image.height);
    result->format = SdImageFormat::Rgb565;
    result->x = 0;
    result->y = 0;
    Serial.printf("[SD] %s: %u bytes, read %u ms, decode %u ms\n", path.c_str(),
                  static_cast<unsigned>(size),
                  static_cast<unsigned>(decode_started - read_started),
                  static_cast<unsigned>(millis() - decode_started));
    return true;
}

uint32_t little_endian_32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

void load_audio()
{
    const String path = String("/sdcard") + AUDIO_FILE;
    FILE *file = fopen(path.c_str(), "rb");
    struct stat info{};
    uint8_t header[44] = {};
    bool ok = file && fread(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        ok = memcmp(header, "RIFF", 4) == 0 &&
             memcmp(header + 8, "WAVEfmt ", 8) == 0 &&
             little_endian_32(header + 16) == 16 &&
             header[20] == 1 && header[21] == 0 &&
             header[22] == 2 && header[23] == 0 &&
             little_endian_32(header + 24) == 48000 &&
             little_endian_32(header + 28) == 192000 &&
             header[32] == 4 && header[33] == 0 &&
             header[34] == 16 && header[35] == 0 &&
             memcmp(header + 36, "data", 4) == 0;
    }

    const size_t pcm_size = ok ? little_endian_32(header + 40) : 0;
    ok = ok && pcm_size > 0 && pcm_size % 4 == 0 &&
         pcm_size <= MaxAudioBytes &&
         fstat(fileno(file), &info) == 0 && info.st_size >= 44 &&
         pcm_size <= static_cast<size_t>(info.st_size - 44);

    auto *pcm = ok ? static_cast<uint8_t *>(ps_malloc(pcm_size)) : nullptr;
    ok = ok && pcm && fread(pcm, 1, pcm_size, file) == pcm_size;
    if (file) {
        fclose(file);
    }

    if (ok && audio_service_submit(pcm, pcm_size)) {
        publish_status("Queued /audio/demo.wav for playback");
        return;
    }
    free(pcm);
    publish_status(ok ? "Audio queue busy / wait and try again"
                      : "WAV unavailable / expected 48 kHz stereo PCM in /audio");
}

void load_backgrounds()
{
    const uint32_t started = millis();
    size_t loaded_count = 0;
    for (size_t i = 0; i < background_count; ++i) {
        SdBackground background{};
        background.index = i;
        if (!load_image(String(background_paths[i]), &background.image)) {
            Serial.printf("[SD] Could not load background %s\n", background_paths[i]);
            continue;
        }
        if (xQueueSend(background_queue, &background, portMAX_DELAY) != pdTRUE) {
            free(background.image.pixels);
            continue;
        }
        ++loaded_count;
    }
    Serial.printf("[SD] Loaded %u of %u backgrounds into PSRAM in %u ms / PSRAM free %u bytes\n",
                  static_cast<unsigned>(loaded_count),
                  static_cast<unsigned>(background_count),
                  static_cast<unsigned>(millis() - started),
                  static_cast<unsigned>(ESP.getFreePsram()));
}

void media_task(void *)
{
    AssetStorage storage;
    bool mounted = false;
    SdCommand command = SdCommand::Retry;

    for (;;) {
        if (command == SdCommand::Retry) {
            // A card that is still powering up, or a bad first contact, can fail the first exchange
            // (e.g. ESP_ERR_INVALID_CRC on CMD8); a few more tries usually get it.
            mounted = storage.mount();
            for (int attempt = 2; !mounted && attempt <= MountAttempts; ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(MountRetryMs));
                Serial.printf("[SD] Mount attempt %d of %d\n", attempt, MountAttempts);
                mounted = storage.mount();
            }
            Serial.printf("[SD] Mount %s / DMA RAM free %u, largest block %u bytes\n",
                          mounted ? "ok" : "failed",
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
            if (!mounted) {
                publish_status(ASSETS_ON_SD_CARD ? "SD mount failed / check card and wiring"
                                                 : "No pictures in flash / run pio run -t uploadfs");
            } else {
                publish_status(ASSETS_ON_SD_CARD ? "SD ready / loading pictures" : "Flash ready / loading pictures");
                load_backgrounds();
                publish_status(ASSETS_ON_SD_CARD ? "SD ready" : "Flash ready");
            }
        } else if (command == SdCommand::PlayAudio) {
            if (mounted) {
                load_audio();
            } else {
                publish_status("Storage unavailable / tap Reload");
            }
        }

        xQueueReceive(command_queue, &command, portMAX_DELAY);
    }
}
}

void sd_service_init()
{
    command_queue = xQueueCreate(4, sizeof(SdCommand));
    status_queue = xQueueCreate(1, sizeof(StatusMessage));
    background_queue = xQueueCreate(1, sizeof(SdBackground));
    if (!command_queue || !status_queue || !background_queue) {
        Serial.println("[SD] SD service queue allocation failed");
        if (command_queue) {
            vQueueDelete(command_queue);
            command_queue = nullptr;
        }
        if (status_queue) {
            vQueueDelete(status_queue);
            status_queue = nullptr;
        }
        if (background_queue) {
            vQueueDelete(background_queue);
            background_queue = nullptr;
        }
        return;
    }
    publish_status("Starting SD card...");
}

void sd_service_start()
{
    if (!command_queue) {
        return;
    }
    if (xTaskCreate(media_task, "sd-media", 12288, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[SD] SD media task creation failed");
        vQueueDelete(command_queue);
        vQueueDelete(status_queue);
        vQueueDelete(background_queue);
        command_queue = nullptr;
        status_queue = nullptr;
        background_queue = nullptr;
    }
}

bool sd_service_request(SdCommand command)
{
    return command_queue && xQueueSend(command_queue, &command, 0) == pdTRUE;
}

bool sd_service_read_status(char *destination, size_t capacity)
{
    if (!status_queue || !destination || capacity == 0) {
        return false;
    }
    StatusMessage message{};
    if (xQueueReceive(status_queue, &message, 0) != pdTRUE) {
        return false;
    }
    strlcpy(destination, message.text, capacity);
    return true;
}

void sd_service_set_backgrounds(const char *const *paths, size_t count)
{
    background_paths = paths;
    background_count = count;
}

bool sd_service_take_background(SdBackground *background)
{
    return background_queue && background &&
           xQueueReceive(background_queue, background, 0) == pdTRUE;
}
