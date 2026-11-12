// main/ntp_time.c
#include "ntp_time.h"
#include "config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "ntp_time";

// Default poll interval: one hour (also what CONFIG_LWIP_SNTP_UPDATE_DELAY
// provides, requested explicitly so the behaviour does not depend on Kconfig).
#define NTP_SYNC_INTERVAL_MS (60ULL * 60ULL * 1000ULL)

static bool s_started = false;

static void start_sntp(void)
{
    const app_config_t* cfg = get_config();
    const char* server = (cfg && cfg->ntp_server[0]) ? cfg->ntp_server : "pool.ntp.org";

    // lwIP copies the name into its own per-server storage, so the pointer only
    // needs to stay valid for the duration of the call.
    esp_sntp_setservername(0, server);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_sync_interval((uint32_t)NTP_SYNC_INTERVAL_MS);
    esp_sntp_init();

    s_started = true;
    ESP_LOGI(TAG, "SNTP started (server %s, interval %llu ms)",
             server, (unsigned long long)NTP_SYNC_INTERVAL_MS);
}

void ntp_time_start(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg || !cfg->ntp_enable) {
        // Mark the hook as handled anyway so repeated GOT_IP events do not log
        // the same message. Enabling NTP takes a reboot (config semantics).
        if (!s_started) {
            s_started = true;
            ESP_LOGI(TAG, "NTP disabled in configuration");
        }
        return;
    }

    if (s_started) {
        // The on-boot HTTP self-test or a manual POST /system/time may have
        // started SNTP before the STA had an address, in which case the first
        // query failed and the next one is an hour away. Force an immediate
        // attempt now that the network is reachable; a synced clock needs
        // nothing.
        if (!ntp_time_synced()) {
            esp_sntp_restart();
        }
        return;
    }

    start_sntp();
}

bool ntp_time_resync(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg || !cfg->ntp_enable) {
        return false;
    }

    esp_sntp_stop();
    s_started = false;
    start_sntp();
    return true;
}

bool ntp_time_synced(void)
{
    return (int64_t)time(NULL) > NTP_TIME_MIN_EPOCH;
}

int64_t ntp_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec > NTP_TIME_MIN_EPOCH) {
        return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    }
    return esp_timer_get_time() / 1000LL;
}

bool ntp_stamp(char* buf, size_t sz)
{
    if (!buf || sz == 0) {
        return false;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec <= NTP_TIME_MIN_EPOCH) {
        return false;
    }

    const app_config_t* cfg = get_config();
    int32_t tz_offset_s = (cfg ? (int32_t)cfg->tz_offset_min : 0) * 60;
    time_t local = tv.tv_sec + tz_offset_s;

    struct tm tm;
    if (!gmtime_r(&local, &tm)) {
        return false;
    }

    int n = snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     (int)(tv.tv_usec / 1000));
    return n > 0 && (size_t)n < sz;
}