#ifndef SD_SERVICE_H
#define SD_SERVICE_H

#include <stddef.h>
#include <stdint.h>

struct SdImage {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
};

enum class SdCommand : uint8_t {
    Previous,
    Next,
    Retry,
    PlayAudio
};

void sd_service_init();
void sd_service_start();
bool sd_service_request(SdCommand command);
bool sd_service_read_status(char *destination, size_t capacity);
bool sd_service_take_image(SdImage *image);

#endif
