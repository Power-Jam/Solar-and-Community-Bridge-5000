#ifndef GPS_SERVICE_H
#define GPS_SERVICE_H

#include <stddef.h>

void gps_service_init();
bool gps_service_read_status(char *destination, size_t capacity);

#endif
