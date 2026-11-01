#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <Arduino.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <soc/sdmmc_pins.h>

static_assert(43 == SDMMC_SLOT0_IOMUX_PIN_NUM_CLK &&
              44 == SDMMC_SLOT0_IOMUX_PIN_NUM_CMD &&
              39 == SDMMC_SLOT0_IOMUX_PIN_NUM_D0);

class SdStorage {
public:
    bool mount()
    {
        if (card && !unmountFailed) {
            unmountFailed = esp_vfs_fat_sdcard_unmount("/sdcard", card) != ESP_OK;
            card = nullptr;
        }
        if (unmountFailed) {
            return false;
        }

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.init = []() {
            sdmmc_host_state_t current{};
            const esp_err_t error = sdmmc_host_get_state(&current);
            if (error != ESP_OK) {
                Serial.printf("[SD] SDMMC state check failed: %s\n", esp_err_to_name(error));
                return error;
            }
            if (current.host_initialized) {
                Serial.println("[SD] SDMMC host already initialized; reusing it");
                return ESP_OK;
            }
            return sdmmc_host_init();
        };

        sdmmc_host_state_t state{};
        if (sdmmc_host_get_state(&state) != ESP_OK) {
            return false;
        }
        slotsBefore = state.num_of_init_slots;
        host.deinit_p = [](int slot) {
            sdmmc_host_state_t current{};
            esp_err_t error = sdmmc_host_get_state(&current);
            if (error == ESP_OK && current.num_of_init_slots > slotsBefore) {
                error = sdmmc_host_deinit_slot(slot);
            }
            return error;
        };

        sdmmc_slot_config_t slot{};
        slot.width = 1;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;

        esp_vfs_fat_sdmmc_mount_config_t config{};
        config.max_files = 4;
        return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &card) == ESP_OK;
    }

private:
    inline static uint8_t slotsBefore = 0;
    sdmmc_card_t *card = nullptr;
    bool unmountFailed = false;
};

#endif
