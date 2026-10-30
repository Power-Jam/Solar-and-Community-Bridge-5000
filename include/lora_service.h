#ifndef LORA_SERVICE_H
#define LORA_SERVICE_H

#include <stddef.h>

void lora_service_init();
bool lora_service_request_test_uplink();
bool lora_service_read_status(char *destination, size_t capacity);
bool lora_service_read_downlink(char *destination, size_t capacity);

#endif
