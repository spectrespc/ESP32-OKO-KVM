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
#include "freertos/event_groups.h"
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
#define STA_MAX_RETRY     5
#define STA_CONNECTED_BIT BIT0

typedef enum {
    STA_IDLE,
    STA_CONNECTING,
    STA_CONNECTED,
    STA_DISCONNECTED,
    STA_ERROR,
} sta_state_t;

static EventGroupHandle_t s_wifi_event_group = NULL;
static volatile sta_state_t s_state = STA_IDLE;
static char s_ssid[33] = { 0 };
static char s_ip[16] = { 0 };

// Guards for self-initiated radio transitions so the asynchronous driver events
// do not overwrite the state the HTTP handlers set, and a bounded retry counter
// for transient link loss.
static volatile bool s_user_disconnect = false;
static volatile bool s_self_reconnect = false;
static int s_retry = 0;

// Consecutive STA connect failures within a rolling 60 s window; drives the
// status LED from "disconnected" (yellow) to "error" (red) when the link keeps
// failing (see status_led.h).
static int s_fail_count = 0;
static int64_t s_fail_window_us = 0;

// Asynchronous scan: handle_wifi_scan kicks off a non-blocking scan and returns
// immediately; the SCAN_DONE event caches the result which
// handle_wifi_scan_results serves to the polling SPA.
static SemaphoreHandle_t s_scan_mutex = NULL;
static volatile bool s_scan_in_progress = false;
static char* s_scan_json = NULL;

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

// Reasons that are worth an automatic reconnect; credential/handshake failures
// are not (retrying them would just hammer the AP with bad credentials).
static bool reason_is_transient(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        case WIFI_REASON_CONNECTION_FAIL:
        case WIFI_REASON_ROAMING:
            return true;
        default:
            return false;
    }
}

static void handle_scan_done(void);

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED:
                s_state = STA_CONNECTING; // link up, waiting for DHCP
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                if (s_wifi_event_group) {
                    xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT);
                }
                s_ip[0] = '\0';

                // A disconnect we asked for (user disconnect or a reconnect in
                // progress) must not be reported as an error or retried.
                if (s_user_disconnect) {
                    s_user_disconnect = false;
                    s_retry = 0;
                    s_state = STA_IDLE;
                    status_led_set_state(STATUS_LED_AP_MODE);
                    break;
                }
                if (s_self_reconnect) {
                    s_self_reconnect = false;
                    break;
                }

                s_state = STA_DISCONNECTED;

                wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
                if (s_ssid[0] && s_retry < STA_MAX_RETRY && reason_is_transient(d->reason)) {
                    s_retry++;
                    s_state = STA_CONNECTING;
                    ESP_LOGW(TAG, "STA link lost (reason %d); reconnect %d/%d",
                             d->reason, s_retry, STA_MAX_RETRY);
                    esp_wifi_connect();
                    status_led_set_state(STATUS_LED_WIFI_CONNECTING);
                } else {
                    // Count consecutive failures in a 60 s window so a persistent
                    // problem shows as red instead of a steady "disconnected".
                    int64_t now = esp_timer_get_time();
                    if (s_fail_window_us == 0 || now - s_fail_window_us > 60LL * 1000000) {
                        s_fail_window_us = now;
                        s_fail_count = 0;
                    }
                    s_fail_count++;
                    status_led_set_state(s_fail_count > 5 ? STATUS_LED_ERROR
                                                          : STATUS_LED_WIFI_DISCONNECTED);
                }
                break;
            }

            case WIFI_EVENT_SCAN_DONE:
                handle_scan_done();
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = STA_CONNECTED;
        s_retry = 0;
        s_fail_count = 0;
        s_fail_window_us = 0;
        status_led_set_state(STATUS_LED_READY);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&ev->ip_info.ip));
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

void init_wifi_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
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

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        strlcpy(s_ssid, ssid, sizeof(s_ssid));

        if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
            s_state = STA_CONNECTING;
            ESP_LOGI(TAG, "Reconnecting to saved network '%s'", ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Failed to apply saved STA configuration");
            s_state = STA_ERROR;
        }
    } else {
        ESP_LOGI(TAG, "No saved STA credentials; station idle");
        status_led_set_state(STATUS_LED_AP_MODE);
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

    bool was_connected = (s_state == STA_CONNECTED);

    wifi_config_t sta_cfg = { 0 };
    strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char*)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    save_sta_creds(ssid, password);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_ip[0] = '\0';
    s_user_disconnect = false;
    s_retry = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err == ESP_OK) {
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT);
        }
        s_state = STA_CONNECTING;
        status_led_set_state(STATUS_LED_WIFI_CONNECTING);
        // Tear down any existing link without reporting it as a drop; the
        // DISCONNECTED event it produces is consumed by the guard above.
        if (was_connected) {
            s_self_reconnect = true;
        }
        esp_wifi_disconnect();
        err = esp_wifi_connect();
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        s_self_reconnect = false;
        ESP_LOGE(TAG, "Failed to start STA connection: %s", esp_err_to_name(err));
        s_state = STA_ERROR;
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Failed to start connection\"}");
    }

    return reply_json(req, "200 OK", "{\"status\":\"connecting\"}");
}

esp_err_t handle_wifi_status(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    bool connected = (s_state == STA_CONNECTED);
    int rssi = 0;
    if (connected) {
        int r = 0;
        if (esp_wifi_sta_get_rssi(&r) == ESP_OK) {
            rssi = r;
        }
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Out of memory\"}");
    }
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "ssid", s_ssid);
    cJSON_AddStringToObject(root, "ip", s_ip);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "state", state_name(s_state));

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
    s_retry = 0;
    s_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_user_disconnect = true;
    if (esp_wifi_disconnect() != ESP_OK) {
        // Nothing was connected, so no DISCONNECTED event will consume the
        // guard; clear it here to avoid swallowing a later real drop.
        s_user_disconnect = false;
    }
    clear_sta_creds();

    s_state = STA_IDLE;
    status_led_set_state(STATUS_LED_AP_MODE);
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT);
    }

    return reply_json(req, "200 OK", "{\"status\":\"idle\"}");
}