#include "time_service.h"

#include <Arduino.h>
#include <atomic>
#include <esp_sntp.h>
#include <stdlib.h>
#include <sys/time.h>

namespace
{
// South Africa Standard Time, UTC+2 all year (POSIX writes the offset with the opposite sign).
constexpr const char *TimeZone = "SAST-2";
// Anything before 2024 means the clock was never set (it starts at 1970).
constexpr time_t EarliestValid = 1704067200; // 2024-01-01 00:00 UTC
constexpr time_t GpsMaxDrift = 2;

std::atomic<TimeSource> source{TimeSource::None};
std::atomic<bool> ntp_started{false};

// Days since 1970-01-01 for a proleptic Gregorian date (newlib has no timegm).
int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

void ntp_synced(struct timeval *)
{
    source = TimeSource::Wifi;
    Serial.println("[Time] Set from NTP");
}
}

void time_service_init()
{
    setenv("TZ", TimeZone, 1);
    tzset();
}

void time_service_start_ntp()
{
    if (ntp_started.exchange(true)) {
        return;
    }
    sntp_set_time_sync_notification_cb(ntp_synced);
    configTzTime(TimeZone, "pool.ntp.org", "time.google.com");
    Serial.println("[Time] NTP started");
}

void time_service_set_from_gps(int year, int month, int day, int hour, int minute, int second)
{
    const time_t gps = static_cast<time_t>(days_from_civil(year, month, day) * 86400 +
                                           hour * 3600 + minute * 60 + second);
    if (gps < EarliestValid) {
        return;
    }
    const time_t now = time(nullptr);
    const time_t drift = now > gps ? now - gps : gps - now;
    if (now >= EarliestValid && drift <= GpsMaxDrift) {
        return;
    }
    const timeval value{gps, 0};
    settimeofday(&value, nullptr);
    source = TimeSource::Gps;
    Serial.printf("[Time] Set from GPS (was %ld s off)\n", static_cast<long>(drift));
}

bool time_service_now(struct tm *local)
{
    const time_t now = time(nullptr);
    if (now < EarliestValid) {
        return false;
    }
    localtime_r(&now, local);
    return true;
}

TimeSource time_service_source()
{
    return source;
}
