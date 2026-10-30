#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stddef.h>
#include <stdint.h>

void audio_service_init();
bool audio_service_submit(uint8_t *pcm, size_t size);
bool audio_service_read_status(char *destination, size_t capacity);

#endif
