#ifndef GPS_SERVICE_H
#define GPS_SERVICE_H

#include <stddef.h>
#include <stdint.h>

struct GpsFix {
    double latitude;
    double longitude;
    bool satellites_valid;
    uint8_t satellites;
};

void gps_service_init();
bool gps_service_read_status(char *destination, size_t capacity);
bool gps_service_latest(GpsFix *destination);

#endif
