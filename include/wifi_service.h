#pragma once

#include <stddef.h>

// on_host_ready runs once WiFi has set up the SDMMC host that the SD card shares.
void wifi_init(void (*on_host_ready)());
bool wifi_submit_credentials(const char *ssid, const char *password);
bool wifi_read_status(char *output, size_t output_size);
