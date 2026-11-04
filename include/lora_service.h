#ifndef LORA_SERVICE_H
#define LORA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

// Each uplink type has its own FPort; ttn/payload_formatter.js decodes them on TTN.
enum class LoraUplink : uint8_t {
    Temperature = 1 << 0, // FPort 10
    Energy = 1 << 1,      // FPort 20
    Gps = 1 << 2,         // FPort 40
};

void lora_service_init();
bool lora_service_request_uplink(LoraUplink uplink);
bool lora_service_read_status(char *destination, size_t capacity);
bool lora_service_read_downlink(char *destination, size_t capacity);

#endif
