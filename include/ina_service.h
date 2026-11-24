#ifndef INA_SERVICE_H
#define INA_SERVICE_H

#include <stddef.h>

struct InaReading {
    bool connected;
    bool over_range;
    float voltage;
    float current;
    float power;
};

// Which INA226: solar panel -> inverter, and battery -> inverter.
enum InaSensor : size_t {
    INA_SOLAR = 0,
    INA_BATTERY = 1,
    INA_SENSOR_COUNT = 2,
};

void ina_service_init(bool i2c_available);
bool ina_service_read(InaSensor sensor, InaReading *destination);
// Unlike ina_service_read, this leaves the reading in the queue for the UI.
bool ina_service_latest(InaSensor sensor, InaReading *destination);

#endif
