#ifndef INA_SERVICE_H
#define INA_SERVICE_H

struct InaReading {
    bool connected;
    bool over_range;
    float voltage;
    float current;
    float power;
};

void ina_service_init(bool i2c_available);
bool ina_service_read(InaReading *destination);
// Unlike ina_service_read, this leaves the reading in the queue for the UI.
bool ina_service_latest(InaReading *destination);

#endif
