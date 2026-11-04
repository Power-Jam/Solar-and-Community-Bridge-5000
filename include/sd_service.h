#ifndef SD_SERVICE_H
#define SD_SERVICE_H

#include <stddef.h>
#include <stdint.h>

// Pictures from the board's flash (0) or an SD card (1); set in platformio.ini.
#ifndef ASSETS_ON_SD_CARD
#define ASSETS_ON_SD_CARD 0
#endif

enum class SdImageFormat : uint8_t {
    Rgb565,   // JPG
    Rgb565A8  // .lay: RGB565 pixels followed by one alpha byte per pixel
};

struct SdImage {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    SdImageFormat format;
    // Where a .lay layer sits on its canvas; 0 for JPGs.
    int16_t x;
    int16_t y;
};

struct SdBackground {
    size_t index;
    SdImage image;
};

enum class SdCommand : uint8_t {
    Retry,  // mount the storage again and reload the pictures
    PlayAudio
};

void sd_service_init();
void sd_service_start();
bool sd_service_request(SdCommand command);
bool sd_service_read_status(char *destination, size_t capacity);
/** Files to load into PSRAM after every mount, as card paths like "/assets/nav-bar.jpg": JPGs, or .lay
 * layers ("LAY1", little-endian uint16 x, y, width, height, RGB565 pixels, then alpha bytes).
 * Paths must stay valid. Call before sd_service_start(). */
void sd_service_set_backgrounds(const char *const *paths, size_t count);
/** The next decoded file, with index into the registered paths. The caller owns image.pixels. */
bool sd_service_take_background(SdBackground *background);

#endif
