#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>

// The pictures in the board's own flash: a LittleFS partition ("spiffs" in partitions.csv),
// filled from the sd/ folder with `pio run -t uploadfs`. It is mounted where the SD card would be,
// so every "/sdcard/..." and "S:/..." path works unchanged.
class FlashStorage {
public:
    bool mount()
    {
        if (mounted) {
            return true;
        }
        // Never format: an empty or missing partition means the files were not uploaded yet.
        mounted = LittleFS.begin(false, "/sdcard", MaxOpenFiles, "spiffs");
        if (mounted) {
            Serial.printf("[Storage] Pictures from flash: %u of %u KB used\n",
                          static_cast<unsigned>(LittleFS.usedBytes() / 1024),
                          static_cast<unsigned>(LittleFS.totalBytes() / 1024));
        } else {
            Serial.println("[Storage] No pictures in flash / run `pio run -t uploadfs` once");
        }
        return mounted;
    }

private:
    static constexpr uint8_t MaxOpenFiles = 8;
    bool mounted = false;
};

#endif
