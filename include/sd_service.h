#ifndef SD_SERVICE_H
#define SD_SERVICE_H

#include <stddef.h>
#include <stdint.h>

struct SdImage {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
};

struct SdBackground {
    size_t index;
    SdImage image;
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
/** JPGs to decode into PSRAM after every mount, as card paths like "/assets/nav-bar.jpg". Paths must stay valid. Call before sd_service_start(). */
void sd_service_set_backgrounds(const char *const *paths, size_t count);
/** The next decoded file, with index into the registered paths. The caller owns image.pixels. */
bool sd_service_take_background(SdBackground *background);

#endif
