#pragma once

#include <stdint.h>
#include <time.h>

// The current time, from GPS (works offline) or from the internet (NTP over WiFi), whichever
// set the clock last. In between, the ESP32 clock keeps running. Shown in South African time.

enum class TimeSource : uint8_t {
    None, // Not set since the device started.
    Gps,
    Wifi,
};

/** Sets the time zone. Call once at start-up, before anything reads the time. */
void time_service_init();

/** Starts NTP. Call from the WiFi task once it has an IP address; calling it again does nothing. */
void time_service_start_ntp();

/** A UTC date and time from the GPS. Sets the clock if it is unknown or more than 2 s off. */
void time_service_set_from_gps(int year, int month, int day, int hour, int minute, int second);

/** Local time (SAST). Returns false while the clock has not been set yet. */
bool time_service_now(struct tm *local);

/** Where the clock was set from most recently. */
TimeSource time_service_source();
