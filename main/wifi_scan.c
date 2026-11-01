// main/wifi_scan.c
#include "wifi_scan.h"
#include "auth_handler.h"
#include "status_led.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* TAG = "wifi_scan";

#define STA_NVS_NAMESPACE "storage"
#define STA_SSID_KEY      "sta_ssid"
#define STA_PASS_KEY      "sta_password"
#define STA_SCAN_MAX      20

#define STA_SUPERVISOR_PERIOD_MS        1000
#define STA_CONNECT_TIMEOUT_MS          25000     // association + DHCP
#define STA_BACKOFF_MAX_MS              60000
#define STA_RADIO_RESTART_FAIL_STREAK   10
#define STA_RADIO_RESTART_AFTER_MS      (10 * 60 * 1000)
#define STA_RADIO_RESTART_COOLDOWN_MS   (10 * 60 * 1000)
#define STA_DISCONNECT_LOG_EVERY        10        // after the first 5 failures
#define STA_IP_CHECK_INTERVAL_MS        10000
#define STA_RSSI_LOG_INTERVAL_MS        60000

typedef enum {
    STA_IDLE,
    STA_CONNECTING,
    STA_CONNECTED,
    STA_DISCONNECTED,
    STA_ERROR,
} sta_state_t;

// All fields below are guarded by s_state_lock. esp_wifi_*() must never be
// called while holding it: the driver generates events synchronously from some
// of its calls, and those events take the same lock.
static SemaphoreHandle_t s_state_lock = NULL;
static sta_state_t s_state = STA_IDLE;
static char s_ssid[33] = { 0 };
static char s_ip[16] = { 0 };
static bool s_have_creds = false;
static bool s_user_disconnect = false;
static bool s_ignore_next_disconnect = false;   // manual /wifi/connect only
static int s_fail_streak = 0;
static int s_attempts_total = 0;
static int s_radio_restarts = 0;
static uint8_t s_last_reason = 0;               // 0 = no disconnect yet
static int64_t s_last_disconnect_us = 0;
static int64_t s_state_since_us = 0;
static int64_t s_next_retry_us = 0;
static int64_t s_first_fail_us = 0;
static int64_t s_last_radio_restart_us = 0;

// Asynchronous scan: handle_wifi_scan kicks off a non-blocking scan and returns
// immediately; the SCAN_DONE event caches the result which
// handle_wifi_scan_results serves to the polling SPA.
static SemaphoreHandle_t s_scan_mutex = NULL;
static volatile bool s_scan_in_progress = false;
static char* s_scan_json = NULL;

typedef struct {
    sta_state_t state;
    bool have_creds;
    bool user_disconnect;
    int fail_streak;
    int attempts_total;
    int radio_restarts;
    uint8_t last_reason;
    int64_t last_disconnect_us;
    int64_t state_since_us;
    int64_t next_retry_us;
    int64_t first_fail_us;
    int64_t last_radio_restart_us;
    char ssid[33];
    char ip[16];
} sta_snapshot_t;

static void state_lock(void)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

static void take_snapshot(sta_snapshot_t* out)
{
    state_lock();
    out->state = s_state;
    out->have_creds = s_have_creds;
    out->user_disconnect = s_user_disconnect;
    out->fail_streak = s_fail_streak;
    out->attempts_total = s_attempts_total;
    out->radio_restarts = s_radio_restarts;
    out->last_reason = s_last_reason;
    out->last_disconnect_us = s_last_disconnect_us;
    out->state_since_us = s_state_since_us;
    out->next_retry_us = s_next_retry_us;
    out->first_fail_us = s_first_fail_us;
    out->last_radio_restart_us = s_last_radio_restart_us;
    memcpy(out->ssid, s_ssid, sizeof(out->ssid));
    memcpy(out->ip, s_ip, sizeof(out->ip));
    state_unlock();
}

static const char* state_name(sta_state_t st)
{
    switch (st) {
        case STA_IDLE:         return "idle";
        case STA_CONNECTING:   return "connecting";
        case STA_CONNECTED:    return "connected";
        case STA_DISCONNECTED: return "disconnected";
        case STA_ERROR:        return "error";
        default:               return "idle";
    }
}

static const char* reason_name(uint8_t reason)
{
    switch (reason) {
        case 0:                                         return "none";
        case WIFI_REASON_AUTH_EXPIRE:                   return "AUTH_EXPIRE";
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:    return "DISASSOC_DUE_TO_INACTIVITY";
        case WIFI_REASON_ASSOC_LEAVE:                   return "ASSOC_LEAVE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:        return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:                return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:                   return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:                     return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:                    return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:             return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:               return "CONNECTION_FAIL";
        case WIFI_REASON_ROAMING:                       return "ROAMING";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
        default:                                        return "UNKNOWN";
    }
}

static const char* ps_name(wifi_ps_type_t ps)
{
    switch (ps) {
        case WIFI_PS_NONE:      return "none";
        case WIFI_PS_MIN_MODEM: return "min_modem";
        case WIFI_PS_MAX_MODEM: return "max_modem";
        default:                return "unknown";
    }
}

// Exponential backoff, indexed by the failure count after the failure is
// recorded. Deliberately jitter-free so the behaviour is predictable in tests.
static int64_t backoff_ms(int fail_streak)
{
    static const int64_t schedule[] = { 1000, 2000, 5000, 10000,
                                        20000, 30000, STA_BACKOFF_MAX_MS };
    int idx = (fail_streak <= 1) ? 0 : fail_streak - 1;
    if (idx >= (int)(sizeof(schedule) / sizeof(schedule[0]))) {
        idx = (int)(sizeof(schedule) / sizeof(schedule[0])) - 1;
    }
    return schedule[idx];
}

static void handle_scan_done(void);

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED:
                state_lock();
                s_state = STA_CONNECTING; // link up, waiting for DHCP
                s_state_since_us = esp_timer_get_time();
                state_unlock();
                ESP_LOGI(TAG, "STA associated, waiting for DHCP");
                status_led_set_state(STATUS_LED_WIFI_CONNECTING);
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
                uint8_t reason = d ? d->reason : 0;
                int64_t now = esp_timer_get_time();
                bool consumed = false;
                int streak = 0;
                int64_t retry_in_ms = 0;

                state_lock();
                s_ip[0] = '\0';
                if (s_user_disconnect) {
                    s_user_disconnect = false;
                    s_state = STA_IDLE;
                    s_fail_streak = 0;
                    s_first_fail_us = 0;
                    s_next_retry_us = 0;
                    consumed = true;
                } else if (s_ignore_next_disconnect) {
                    // This is the tear-down before a manual reconnect; it is
                    // not a link failure.
                    s_ignore_next_disconnect = false;
                    consumed = true;
                } else {
                    s_state = STA_DISCONNECTED;
                    s_last_reason = reason;
                    s_last_disconnect_us = now;
                    s_fail_streak++;
                    if (s_first_fail_us == 0) {
                        s_first_fail_us = now;
                    }
                    s_state_since_us = now;
                    s_next_retry_us = now + backoff_ms(s_fail_streak) * 1000;
                    streak = s_fail_streak;
                    retry_in_ms = backoff_ms(streak);
                }
                state_unlock();

                if (consumed) {
                    break;
                }

                if (streak <= 5 || (streak % STA_DISCONNECT_LOG_EVERY) == 0) {
                    ESP_LOGW(TAG, "STA link lost (reason %u %s); failure %d, retry in %d s",
                             (unsigned)reason, reason_name(reason), streak,
                             (int)(retry_in_ms / 1000));
                }
                status_led_set_state(streak > 5 ? STATUS_LED_ERROR
                                                : STATUS_LED_WIFI_DISCONNECTED);
                break;
            }

            case WIFI_EVENT_STA_START: {
                int64_t now = esp_timer_get_time();
                state_lock();
                if (s_have_creds && !s_user_disconnect) {
                    s_next_retry_us = now;
                    if (s_state == STA_IDLE) {
                        s_state = STA_DISCONNECTED;
                    }
                }
                state_unlock();
                break;
            }

            case WIFI_EVENT_STA_BEACON_TIMEOUT: {
                int rssi = 0;
                esp_wifi_sta_get_rssi(&rssi);
                wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
                esp_wifi_get_ps(&ps);
                ESP_LOGW(TAG, "STA beacon timeout (rssi %d dBm, power save %s)",
                         rssi, ps_name(ps));
                break;
            }

            case WIFI_EVENT_SCAN_DONE:
                handle_scan_done();
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t* ev = (ip_event_got_ip_t*)event_data;
                char ip[16];
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));

                state_lock();
                strlcpy(s_ip, ip, sizeof(s_ip));
                s_state = STA_CONNECTED;
                s_fail_streak = 0;
                s_first_fail_us = 0;
                s_next_retry_us = 0;
                s_state_since_us = esp_timer_get_time();
                state_unlock();

                status_led_set_state(STATUS_LED_READY);
                int rssi = 0;
                esp_wifi_sta_get_rssi(&rssi);
                wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
                esp_wifi_get_ps(&ps);
                ESP_LOGI(TAG, "STA got IP %s (rssi %d dBm, power save %s)",
                         ip, rssi, ps_name(ps));
                break;
            }

            case IP_EVENT_STA_LOST_IP: {
                int64_t now = esp_timer_get_time();
                bool was_connected = false;
                state_lock();
                s_ip[0] = '\0';
                if (s_state == STA_CONNECTED) {
                    s_state = STA_CONNECTING; // supervisor applies the timeout
                    s_state_since_us = now;
                    was_connected = true;
                }
                state_unlock();
                if (was_connected) {
                    ESP_LOGW(TAG, "STA lost its IP; waiting for DHCP");
                }
                break;
            }

            default:
                break;
        }
    }
}

static bool load_sta_creds(char* ssid, size_t ssid_len, char* pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(STA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    bool have_ssid = false;
    size_t len = ssid_len;
    if (nvs_get_str(h, STA_SSID_KEY, ssid, &len) == ESP_OK && ssid[0]) {
        have_ssid = true;
    }

    len = pass_len;
    if (nvs_get_str(h, STA_PASS_KEY, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }

    nvs_close(h);
    return have_ssid;
}

static void save_sta_creds(const char* ssid, const char* password)
{
    nvs_handle_t h;
    if (nvs_open(STA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for STA credentials");
        return;
    }
    nvs_set_str(h, STA_SSID_KEY, ssid);
    nvs_set_str(h, STA_PASS_KEY, password ? password : "");
    nvs_commit(h);
    nvs_close(h);
}

static void clear_sta_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(STA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, STA_SSID_KEY);
    nvs_erase_key(h, STA_PASS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

// Soft radio restart: recovers a station that the driver can no longer use
// without rebooting the whole gateway. It briefly (1-2 s) interrupts the
// SoftAP because stop/start acts on the whole AP+STA radio.
static void radio_restart(void)
{
    int64_t now = esp_timer_get_time();
    int restarts = 0;

    state_lock();
    s_radio_restarts++;
    restarts = s_radio_restarts;
    s_last_radio_restart_us = now;
    s_fail_streak = 0;
    s_first_fail_us = 0;
    s_next_retry_us = now + 1000000;
    state_unlock();

    ESP_LOGW(TAG, "STA unreachable for %d min; restarting Wi-Fi radio (restart #%d)",
             STA_RADIO_RESTART_AFTER_MS / 60000, restarts);

    // After esp_wifi_stop() the SCAN_DONE event may never arrive, which would
    // leave /wifi/scan/results stuck on "scanning" forever.
    if (s_scan_mutex) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    free(s_scan_json);
    s_scan_json = NULL;
    s_scan_in_progress = false;
    if (s_scan_mutex) {
        xSemaphoreGive(s_scan_mutex);
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        state_lock();
        // The radio is unusable but the cooldown must still apply; retry the
        // restart once it expires instead of spinning on esp_wifi_connect().
        s_fail_streak = STA_RADIO_RESTART_FAIL_STREAK;
        s_first_fail_us = now - (int64_t)STA_RADIO_RESTART_AFTER_MS * 1000;
        s_next_retry_us = now + 1000000;
        state_unlock();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start after radio restart failed: %s",
                 esp_err_to_name(err));
        state_lock();
        s_fail_streak = STA_RADIO_RESTART_FAIL_STREAK;
        s_first_fail_us = now - (int64_t)STA_RADIO_RESTART_AFTER_MS * 1000;
        s_next_retry_us = now + 1000000;
        state_unlock();
        return;
    }

    // stop/start can reset the power-save mode; re-apply and verify it.
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGW(TAG, "power save was %s after radio restart; restored WIFI_PS_NONE",
                 ps_name(ps));
    }

    err = esp_wifi_connect();
    if (err == ESP_OK) {
        bool reconnecting = false;
        state_lock();
        if (s_state != STA_CONNECTED) {
            s_attempts_total++;
            s_state = STA_CONNECTING;
            s_state_since_us = esp_timer_get_time();
            s_next_retry_us = 0;
            reconnecting = true;
        }
        state_unlock();
        if (reconnecting) {
            ESP_LOGI(TAG, "radio restarted; STA reconnecting");
        }
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect after radio restart failed: %s",
                 esp_err_to_name(err));
    }
}

static void wifi_supervisor_task(void* arg)
{
    (void)arg;
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    int64_t next_ip_check_us = 0;
    int64_t next_rssi_log_us = 0;
    int connect_errors = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STA_SUPERVISOR_PERIOD_MS));

        sta_snapshot_t s;
        take_snapshot(&s);

        if (!s.have_creds || s.user_disconnect) {
            continue;
        }

        int64_t now = esp_timer_get_time();

        switch (s.state) {
            case STA_CONNECTED: {
                if (now >= next_ip_check_us) {
                    next_ip_check_us = now + (int64_t)STA_IP_CHECK_INTERVAL_MS * 1000;
                    esp_netif_ip_info_t ip_info;
                    if (sta_netif &&
                        esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK &&
                        ip_info.ip.addr == 0) {
                        bool lost = false;
                        state_lock();
                        if (s_state == STA_CONNECTED) {
                            s_state = STA_DISCONNECTED;
                            s_state_since_us = now;
                            s_next_retry_us = now + 1000000;
                            lost = true;
                        }
                        state_unlock();
                        if (lost) {
                            ESP_LOGW(TAG, "STA address disappeared; scheduling reconnect");
                            // The link may still be up without an address, in
                            // which case esp_wifi_connect() would be rejected.
                            // Tear it down and let the DISCONNECTED event
                            // schedule the retry.
                            esp_wifi_disconnect();
                        }
                    }
                }
                if (now >= next_rssi_log_us) {
                    next_rssi_log_us = now + (int64_t)STA_RSSI_LOG_INTERVAL_MS * 1000;
                    int rssi = 0;
                    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
                        ESP_LOGI(TAG, "STA link up, rssi %d dBm", rssi);
                    }
                }
                break;
            }

            case STA_CONNECTING: {
                if (now - s.state_since_us <= (int64_t)STA_CONNECT_TIMEOUT_MS * 1000) {
                    break;
                }

                // Extend the deadline before touching the driver so a slow
                // DISCONNECTED event cannot trigger a second disconnect.
                bool timed_out = false;
                state_lock();
                if (s_state == STA_CONNECTING) {
                    s_state_since_us = now;
                    timed_out = true;
                }
                state_unlock();
                if (!timed_out) {
                    break;
                }

                ESP_LOGW(TAG, "STA connect timed out after %d s; forcing reconnect",
                         STA_CONNECT_TIMEOUT_MS / 1000);

                esp_err_t err = esp_wifi_disconnect();
                if (err != ESP_OK) {
                    // No event will follow; record the failure here and let the
                    // supervisor retry.
                    int streak = 0;
                    int64_t retry_in_ms = 0;
                    state_lock();
                    if (s_state == STA_CONNECTING) {
                        s_state = STA_DISCONNECTED;
                        s_last_reason = 0;
                        s_last_disconnect_us = esp_timer_get_time();
                        s_fail_streak++;
                        if (s_first_fail_us == 0) {
                            s_first_fail_us = s_last_disconnect_us;
                        }
                        s_state_since_us = s_last_disconnect_us;
                        retry_in_ms = backoff_ms(s_fail_streak);
                        s_next_retry_us = s_last_disconnect_us + retry_in_ms * 1000;
                        streak = s_fail_streak;
                    }
                    state_unlock();
                    if (streak > 0) {
                        ESP_LOGW(TAG, "esp_wifi_disconnect failed (%s); failure %d, retry in %d s",
                                 esp_err_to_name(err), streak, (int)(retry_in_ms / 1000));
                        status_led_set_state(streak > 5 ? STATUS_LED_ERROR
                                                        : STATUS_LED_WIFI_DISCONNECTED);
                    }
                }
                break;
            }

            case STA_DISCONNECTED:
            case STA_ERROR: {
                if (now < s.next_retry_us) {
                    break;
                }

                bool restart_wanted = (s.fail_streak >= STA_RADIO_RESTART_FAIL_STREAK) &&
                                      (s.first_fail_us != 0) &&
                                      (now - s.first_fail_us >=
                                       (int64_t)STA_RADIO_RESTART_AFTER_MS * 1000);
                bool cooldown_ok = (s.last_radio_restart_us == 0) ||
                                   (now - s.last_radio_restart_us >=
                                    (int64_t)STA_RADIO_RESTART_COOLDOWN_MS * 1000);
                if (restart_wanted && cooldown_ok) {
                    radio_restart();
                    break;
                }

                esp_err_t err = esp_wifi_connect();
                if (err == ESP_OK) {
                    bool started = false;
                    int attempts = 0;
                    int streak = 0;
                    connect_errors = 0;
                    state_lock();
                    if (s_state == STA_DISCONNECTED || s_state == STA_ERROR) {
                        s_attempts_total++;
                        attempts = s_attempts_total;
                        streak = s.fail_streak;
                        s_state = STA_CONNECTING;
                        s_state_since_us = esp_timer_get_time();
                        s_next_retry_us = 0;
                        started = true;
                    }
                    state_unlock();
                    if (started) {
                        ESP_LOGI(TAG, "STA connect attempt %d (after %d failure(s))",
                                 attempts, streak);
                        status_led_set_state(STATUS_LED_WIFI_CONNECTING);
                    }
                } else if (err == ESP_ERR_WIFI_STATE) {
                    // The driver is still busy; this is not a link failure.
                    ESP_LOGD(TAG, "esp_wifi_connect not ready (%s); retrying next tick",
                             esp_err_to_name(err));
                    state_lock();
                    s_next_retry_us = esp_timer_get_time() + 1000000;
                    state_unlock();
                } else {
                    connect_errors++;
                    if (connect_errors <= 3 || (connect_errors % 60) == 0) {
                        ESP_LOGW(TAG, "esp_wifi_connect failed: %s",
                                 esp_err_to_name(err));
                    }
                    state_lock();
                    s_next_retry_us = esp_timer_get_time() + 5000000;
                    state_unlock();
                }
                break;
            }

            case STA_IDLE:
            default:
                break;
        }
    }
}

void init_wifi_sta(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!sta_netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &sta_event_handler, NULL);

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        state_lock();
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        if (err == ESP_OK) {
            s_have_creds = true;
            s_state = STA_DISCONNECTED;
            s_state_since_us = esp_timer_get_time();
            s_next_retry_us = s_state_since_us; // supervisor connects immediately
        } else {
            s_have_creds = false;
            s_state = STA_ERROR;
        }
        state_unlock();

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Reconnecting to saved network '%s'", ssid);
            status_led_set_state(STATUS_LED_WIFI_CONNECTING);
        } else {
            ESP_LOGE(TAG, "Failed to apply saved STA configuration: %s",
                     esp_err_to_name(err));
            status_led_set_state(STATUS_LED_ERROR);
        }
    } else {
        state_lock();
        s_state = STA_IDLE;
        s_have_creds = false;
        state_unlock();
        ESP_LOGI(TAG, "No saved STA credentials; station idle");
        status_led_set_state(STATUS_LED_AP_MODE);
    }

    if (xTaskCreate(wifi_supervisor_task, "wifi_sup", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi_sup task");
    }
}

// Reads at most max_len-1 bytes of the request body into buf (NUL-terminated).
static int read_body(httpd_req_t* req, char* buf, size_t max_len)
{
    int total = req->content_len;
    if (total <= 0) {
        buf[0] = '\0';
        return 0;
    }
    if (total > (int)max_len - 1) {
        total = (int)max_len - 1;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        received += r;
    }
    buf[received] = '\0';
    return received;
}

static esp_err_t reply_json(httpd_req_t* req, const char* status_line, const char* json)
{
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reject_unauthorized(httpd_req_t* req)
{
    return reply_json(req, "401 Unauthorized", "{\"error\":\"Authentication required\"}");
}

static int cmp_rssi_desc(const void* a, const void* b)
{
    const wifi_ap_record_t* ra = a;
    const wifi_ap_record_t* rb = b;
    return (int)rb->rssi - (int)ra->rssi;
}

// Fetches the driver's completed scan results and serializes them. Caller owns
// the returned heap string (or NULL on failure).
static char* build_scan_json(void)
{
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > STA_SCAN_MAX) {
        count = STA_SCAN_MAX;
    }

    wifi_ap_record_t* records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_clear_ap_list();
        return NULL;
    }

    uint16_t got = count;
    if (got > 0 && esp_wifi_scan_get_ap_records(&got, records) != ESP_OK) {
        got = 0;
    }
    esp_wifi_clear_ap_list();

    qsort(records, got, sizeof(wifi_ap_record_t), cmp_rssi_desc);

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        free(records);
        return NULL;
    }
    cJSON_AddItemToObject(root, "networks", arr);

    for (uint16_t i = 0; i < got; i++) {
        cJSON* net = cJSON_CreateObject();
        if (!net) {
            break;
        }
        cJSON_AddStringToObject(net, "ssid", (const char*)records[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(net, "authmode", (int)records[i].authmode);
        cJSON_AddNumberToObject(net, "channel", records[i].primary);
        cJSON_AddItemToArray(arr, net);
    }
    free(records);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void handle_scan_done(void)
{
    // Serializing the driver's list must not run under the served-results lock,
    // so build outside it and swap the pointer atomically.
    char* json = build_scan_json();

    if (s_scan_mutex) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    free(s_scan_json);
    s_scan_json = json;
    s_scan_in_progress = false;
    if (s_scan_mutex) {
        xSemaphoreGive(s_scan_mutex);
    }
}

esp_err_t handle_wifi_scan(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    if (s_scan_in_progress) {
        return reply_json(req, "202 Accepted", "{\"status\":\"scanning\"}");
    }

    // Drop any previous results so the SPA cannot read a stale list while the
    // new scan is running.
    if (s_scan_mutex) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    free(s_scan_json);
    s_scan_json = NULL;
    s_scan_in_progress = true;
    if (s_scan_mutex) {
        xSemaphoreGive(s_scan_mutex);
    }

    // Non-blocking: results arrive via WIFI_EVENT_SCAN_DONE so the shared httpd
    // task is not stalled for the duration of the scan.
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        if (s_scan_mutex) {
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        }
        s_scan_in_progress = false;
        if (s_scan_mutex) {
            xSemaphoreGive(s_scan_mutex);
        }
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Scan failed\"}");
    }

    return reply_json(req, "202 Accepted", "{\"status\":\"scanning\"}");
}

esp_err_t handle_wifi_scan_results(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    if (s_scan_mutex) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    bool in_progress = s_scan_in_progress;
    char* json = s_scan_json ? strdup(s_scan_json) : NULL;
    if (s_scan_mutex) {
        xSemaphoreGive(s_scan_mutex);
    }

    if (in_progress) {
        free(json);
        return reply_json(req, "200 OK", "{\"status\":\"scanning\"}");
    }
    if (!json) {
        return reply_json(req, "200 OK", "{\"networks\":[]}");
    }

    esp_err_t err = reply_json(req, "200 OK", json);
    free(json);
    return err;
}

esp_err_t handle_wifi_connect(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    char body[256];
    if (read_body(req, body, sizeof(body)) <= 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Empty body\"}");
    }

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
    }

    cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON* pass_item = cJSON_GetObjectItem(root, "password");
    const char* ssid = (ssid_item && cJSON_IsString(ssid_item)) ? ssid_item->valuestring : NULL;
    const char* password = (pass_item && cJSON_IsString(pass_item)) ? pass_item->valuestring : "";

    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) {
        cJSON_Delete(root);
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid SSID or password\"}");
    }

    bool was_connected;
    state_lock();
    was_connected = (s_state == STA_CONNECTED);
    state_unlock();

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char*)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    save_sta_creds(ssid, password);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err == ESP_OK) {
        int64_t now = esp_timer_get_time();
        state_lock();
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        s_have_creds = true;
        s_user_disconnect = false;
        s_state = STA_CONNECTING;
        s_state_since_us = now;
        s_fail_streak = 0;
        s_first_fail_us = 0;
        s_next_retry_us = 0;
        s_ip[0] = '\0';
        s_attempts_total++;
        s_ignore_next_disconnect = was_connected;
        state_unlock();

        if (was_connected) {
            // Tear down the existing link; the DISCONNECTED event it produces
            // is consumed by s_ignore_next_disconnect.
            esp_err_t derr = esp_wifi_disconnect();
            if (derr != ESP_OK) {
                state_lock();
                s_ignore_next_disconnect = false;
                state_unlock();
                ESP_LOGW(TAG, "esp_wifi_disconnect before reconnect failed: %s",
                         esp_err_to_name(derr));
            }
        }

        status_led_set_state(STATUS_LED_WIFI_CONNECTING);
        err = esp_wifi_connect();
    }

    if (err != ESP_OK) {
        state_lock();
        if (s_ssid[0] == '\0') {
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
        }
        s_have_creds = true;
        s_state = STA_ERROR;
        s_next_retry_us = esp_timer_get_time() + 1000 * 1000;
        s_ignore_next_disconnect = false;
        state_unlock();

        ESP_LOGE(TAG, "Failed to start STA connection: %s", esp_err_to_name(err));
        status_led_set_state(STATUS_LED_ERROR);
        cJSON_Delete(root);
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Failed to start connection\"}");
    }

    cJSON_Delete(root);
    return reply_json(req, "200 OK", "{\"status\":\"connecting\"}");
}

esp_err_t handle_wifi_status(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }

    sta_snapshot_t s;
    take_snapshot(&s);

    int rssi = 0;
    if (s.state == STA_CONNECTED) {
        int r = 0;
        if (esp_wifi_sta_get_rssi(&r) == ESP_OK) {
            rssi = r;
        }
    }

    wifi_ps_type_t ps = WIFI_PS_NONE;
    const char* ps_str = "unknown";
    if (esp_wifi_get_ps(&ps) == ESP_OK) {
        ps_str = ps_name(ps);
    }

    int64_t now = esp_timer_get_time();
    int last_reason = (s.last_reason == 0) ? -1 : (int)s.last_reason;
    int64_t last_age_s = (s.last_disconnect_us == 0)
                             ? -1
                             : (now - s.last_disconnect_us) / 1000000;
    int64_t next_retry_ms = 0;
    if (s.state == STA_DISCONNECTED || s.state == STA_ERROR) {
        next_retry_ms = (s.next_retry_us > now) ? (s.next_retry_us - now) / 1000 : 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Out of memory\"}");
    }
    cJSON_AddBoolToObject(root, "connected", s.state == STA_CONNECTED);
    cJSON_AddStringToObject(root, "ssid", s.ssid);
    cJSON_AddStringToObject(root, "ip", s.ip);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "state", state_name(s.state));
    cJSON_AddNumberToObject(root, "attempts", s.attempts_total);
    cJSON_AddNumberToObject(root, "fail_streak", s.fail_streak);
    cJSON_AddNumberToObject(root, "last_disconnect_reason", last_reason);
    cJSON_AddStringToObject(root, "last_disconnect_reason_name",
                            reason_name(s.last_reason));
    cJSON_AddNumberToObject(root, "last_disconnect_age_s", (double)last_age_s);
    cJSON_AddNumberToObject(root, "next_retry_in_ms", (double)next_retry_ms);
    cJSON_AddNumberToObject(root, "radio_restarts", s.radio_restarts);
    cJSON_AddStringToObject(root, "power_save", ps_str);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Serialization failed\"}");
    }

    esp_err_t err = reply_json(req, "200 OK", out);
    free(out);
    return err;
}

esp_err_t handle_wifi_disconnect(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }

    state_lock();
    s_user_disconnect = true;
    s_have_creds = false;
    s_state = STA_IDLE;
    s_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_next_retry_us = 0;
    s_fail_streak = 0;
    s_first_fail_us = 0;
    s_ignore_next_disconnect = false;
    state_unlock();

    if (esp_wifi_disconnect() != ESP_OK) {
        // Nothing was connected, so no DISCONNECTED event will consume the
        // guard; clear it here to avoid swallowing a later real drop.
        state_lock();
        s_user_disconnect = false;
        state_unlock();
    }
    clear_sta_creds();

    status_led_set_state(STATUS_LED_AP_MODE);
    return reply_json(req, "200 OK", "{\"status\":\"idle\"}");
}