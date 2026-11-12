// main/ntp_time.h
#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lowest plausible Unix time (Nov 2023). The ESP32-C6 has no battery-backed
// RTC, so at boot CLOCK_REALTIME starts near zero and only becomes meaningful
// after SNTP sets it. Everything in this module uses this threshold to tell
// "synced" from "still counting from boot".
#define NTP_TIME_MIN_EPOCH 1700000000

// Starts SNTP. Called from the Wi-Fi STA GOT_IP event and therefore only runs
// once the network is reachable; repeated calls are no-ops. Does nothing when
// ntp_enable is 0 in the configuration.
void ntp_time_start(void);

// Stops and restarts SNTP on request (POST /system/time). Returns false when
// NTP is disabled in the configuration.
bool ntp_time_resync(void);

// True when the system clock holds a plausible real time (epoch seconds above
// NTP_TIME_MIN_EPOCH).
bool ntp_time_synced(void);

// Milliseconds since the Unix epoch when the clock is synced, otherwise
// milliseconds since boot (the same value esp_timer_get_time()/1000 returns).
// Used by the CSV sensor log and MQTT payloads.
int64_t ntp_time_ms(void);

// Formats local time as "YYYY-MM-DD HH:MM:SS.mmm" using tz_offset_min from the
// configuration (fixed UTC offset, no DST). Returns false and leaves buf
// untouched when the clock is not synced or the buffer is too small.
bool ntp_stamp(char* buf, size_t sz);

#endif // NTP_TIME_H