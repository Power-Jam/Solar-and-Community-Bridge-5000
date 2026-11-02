#include "sd_service.h"

#include "audio_service.h"
#include "board_config.h"
#include "sd_storage.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <new>
#include <sys/stat.h>
#include <vector>

namespace
{
constexpr size_t MaxImages = 128;
constexpr size_t MaxJpegBytes = 4 * 1024 * 1024;
constexpr size_t MaxAudioBytes = 4 * 1024 * 1024;
constexpr int MaxImageWidth = 1024;
constexpr int MaxImageHeight = 600;

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
QueueHandle_t image_queue;

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

void publish_empty_image()
{
    const SdImage image{};
    xQueueSend(image_queue, &image, portMAX_DELAY);
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

bool load_image(const String &path, SdImage *result)
{
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

bool scan_images(std::vector<String> *images)
{
    DIR *directory = opendir("/sdcard/images");
    if (!directory) {
        return false;
    }

    while (images->size() < MaxImages) {
        struct dirent *entry = readdir(directory);
        if (!entry) {
            break;
        }
        String name = entry->d_name;
        String lower = name;
        lower.toLowerCase();
        struct stat info{};
        const String full_path = String("/sdcard/images/") + name;
        if ((lower.endsWith(".jpg") || lower.endsWith(".jpeg")) &&
            stat(full_path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            images->push_back(String("/images/") + name);
        }
    }
    closedir(directory);
    std::sort(images->begin(), images->end());
    return true;
}

void media_task(void *)
{
    SdStorage storage;
    std::vector<String> images;
    bool mounted = false;
    size_t index = 0;
    SdCommand command = SdCommand::Retry;

    for (;;) {
        if (command == SdCommand::Retry) {
            mounted = storage.mount();
            Serial.printf("[SD] Mount %s / DMA RAM free %u, largest block %u bytes\n",
                          mounted ? "ok" : "failed",
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
            images.clear();
            if (!mounted) {
                publish_empty_image();
                publish_status("SD mount failed / check card and wiring");
            } else if (!scan_images(&images)) {
                publish_empty_image();
                publish_status("SD ready / create an /images folder");
            } else if (images.empty()) {
                publish_empty_image();
                publish_status("SD ready / no JPGs in /images");
            } else {
                index = 0;
                char status[128];
                snprintf(status, sizeof(status), "SD ready / %u JPG image(s)",
                         static_cast<unsigned>(images.size()));
                publish_status(status);
            }
        } else if (command == SdCommand::PlayAudio) {
            if (mounted) {
                load_audio();
            } else {
                publish_status("SD unavailable / tap Retry SD");
            }
        } else if (!mounted) {
            publish_status("SD unavailable / tap Retry SD");
        } else if (images.empty()) {
            publish_status("No JPGs found / add files to /images");
        } else {
            if (command == SdCommand::Next) {
                index = (index + 1) % images.size();
            } else if (command == SdCommand::Previous) {
                index = (index + images.size() - 1) % images.size();
            }

            SdImage image{};
            if (load_image(images[index], &image)) {
                if (xQueueSend(image_queue, &image, portMAX_DELAY) != pdTRUE) {
                    free(image.pixels);
                } else {
                    char status[128];
                    snprintf(status, sizeof(status), "%u/%u  %s",
                             static_cast<unsigned>(index + 1),
                             static_cast<unsigned>(images.size()),
                             images[index].substring(String("/images/").length()).c_str());
                    publish_status(status);
                }
            } else {
                char status[128];
                snprintf(status, sizeof(status), "Could not decode %s",
                         images[index].substring(String("/images/").length()).c_str());
                publish_status(status);
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
    image_queue = xQueueCreate(1, sizeof(SdImage));
    if (!command_queue || !status_queue || !image_queue) {
        Serial.println("[SD] SD service queue allocation failed");
        if (command_queue) {
            vQueueDelete(command_queue);
            command_queue = nullptr;
        }
        if (status_queue) {
            vQueueDelete(status_queue);
            status_queue = nullptr;
        }
        if (image_queue) {
            vQueueDelete(image_queue);
            image_queue = nullptr;
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
        vQueueDelete(image_queue);
        command_queue = nullptr;
        status_queue = nullptr;
        image_queue = nullptr;
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

bool sd_service_take_image(SdImage *image)
{
    return image_queue && image &&
           xQueueReceive(image_queue, image, 0) == pdTRUE;
}
