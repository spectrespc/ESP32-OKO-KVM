// main/http_server.c
#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "config.h"
#include "pin_validation.h"
#include "sd_handler.h"
#include "sensor_handler.h"
#include "web_handlers.h"
#include "log_handler.h"
#include "wifi_scan.h"
#include "system_handler.h"
#include "ext_control.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "http_server";
static httpd_handle_t server = NULL;

// --- Small helpers -----------------------------------------------------------

static esp_err_t reply_json(httpd_req_t *req, const char* status_line, const char* json)
{
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Reads at most max_len bytes of the request body into buf (NUL-terminated).
// Returns the number of bytes read, or -1 on error.
static int read_body(httpd_req_t *req, char* buf, size_t max_len)
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

// The get_json_* helpers return true when the key is present and its value is
// in range. A present-but-out-of-range value sets *invalid (and returns false)
// so the caller can reject the whole request instead of silently truncating.
static bool get_json_u8(cJSON* root, const char* key, uint8_t* out, bool* invalid)
{
    cJSON* it = cJSON_GetObjectItem(root, key);
    if (!it || !cJSON_IsNumber(it)) {
        return false;
    }
    double v = it->valuedouble;
    if (v != (double)(int64_t)v || v < 0 || v > 1) {
        *invalid = true;
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

// Like get_json_u8 but for full 0-255 values (I2C addresses), not just flags.
static bool get_json_byte(cJSON* root, const char* key, uint8_t* out, bool* invalid)
{
    cJSON* it = cJSON_GetObjectItem(root, key);
    if (!it || !cJSON_IsNumber(it)) {
        return false;
    }
    double v = it->valuedouble;
    if (v != (double)(int64_t)v || v < 0 || v > 255) {
        *invalid = true;
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool get_json_u16(cJSON* root, const char* key, uint16_t* out, bool* invalid)
{
    cJSON* it = cJSON_GetObjectItem(root, key);
    if (!it || !cJSON_IsNumber(it)) {
        return false;
    }
    double v = it->valuedouble;
    if (v != (double)(int64_t)v || v < 1 || v > 65535) {
        *invalid = true;
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static bool get_json_pin(cJSON* obj, const char* key, gpio_num_t* out, bool* invalid)
{
    cJSON* it = cJSON_GetObjectItem(obj, key);
    if (!it || !cJSON_IsNumber(it)) {
        return false;
    }
    double v = it->valuedouble;
    if (v != (double)(int64_t)v || v < 0 || v > PIN_MAX_GPIO) {
        *invalid = true;
        return false;
    }
    *out = (gpio_num_t)(int64_t)v;
    return true;
}

// Replaces *slot with a copy of the JSON string when the key is present.
static bool get_json_string(cJSON* root, const char* key, char** slot)
{
    cJSON* it = cJSON_GetObjectItem(root, key);
    if (it && cJSON_IsString(it) && it->valuestring) {
        char* dup = strdup(it->valuestring);
        if (!dup) {
            return false;
        }
        free(*slot);
        *slot = dup;
        return true;
    }
    return false;
}

// --- Handlers ----------------------------------------------------------------

static esp_err_t handle_get_config(httpd_req_t *req)
{
    char* config_json = get_config_json();
    if (config_json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, config_json, strlen(config_json));
        free(config_json);
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to get config", -1);
    }
    return ESP_OK;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    char body[4096];
    int received = read_body(req, body, sizeof(body));
    if (received < 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Failed to read request body\"}");
    }

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
    }

    // Partial update: start from the current configuration and override only
    // the fields present in the request.
    app_config_t* cfg = get_config();
    app_config_t candidate = *cfg;
    candidate.wifi_ssid = cfg->wifi_ssid ? strdup(cfg->wifi_ssid) : NULL;
    candidate.wifi_password = cfg->wifi_password ? strdup(cfg->wifi_password) : NULL;
    candidate.server_url = cfg->server_url ? strdup(cfg->server_url) : NULL;
    candidate.mqtt_broker_url = cfg->mqtt_broker_url ? strdup(cfg->mqtt_broker_url) : NULL;
    candidate.mqtt_topic_prefix = cfg->mqtt_topic_prefix ? strdup(cfg->mqtt_topic_prefix) : NULL;

    bool touched = false;
    bool invalid = false;
    touched |= get_json_u8(root, "uart0_enable", &candidate.uart0_enable, &invalid);
    touched |= get_json_u8(root, "uart1_enable", &candidate.uart1_enable, &invalid);
    touched |= get_json_u8(root, "sd_enable", &candidate.sd_enable, &invalid);
    touched |= get_json_u8(root, "sensor_enable", &candidate.sensor_enable, &invalid);
    touched |= get_json_u8(root, "led_enable", &candidate.led_enable, &invalid);
    touched |= get_json_u8(root, "telnet0_enable", &candidate.telnet0_enable, &invalid);
    touched |= get_json_u16(root, "telnet0_port", &candidate.telnet0_port, &invalid);
    touched |= get_json_u8(root, "telnet1_enable", &candidate.telnet1_enable, &invalid);
    touched |= get_json_u16(root, "telnet1_port", &candidate.telnet1_port, &invalid);
    touched |= get_json_u8(root, "uart0_sd_log", &candidate.uart0_sd_log, &invalid);
    touched |= get_json_u8(root, "uart1_sd_log", &candidate.uart1_sd_log, &invalid);
    touched |= get_json_u8(root, "sht21_sd_log", &candidate.sht21_sd_log, &invalid);
    touched |= get_json_u8(root, "ina219_sd_log", &candidate.ina219_sd_log, &invalid);
    touched |= get_json_u8(root, "ina226_sd_log", &candidate.ina226_sd_log, &invalid);
    touched |= get_json_u8(root, "env_sd_log", &candidate.env_sd_log, &invalid);
    touched |= get_json_u8(root, "mqtt_enable", &candidate.mqtt_enable, &invalid);

    // External device power/reset control (state is not settable here).
    touched |= get_json_u8(root, "ext_pwr_enable", &candidate.ext_pwr_enable, &invalid);
    touched |= get_json_u8(root, "ext_rst_enable", &candidate.ext_rst_enable, &invalid);
    touched |= get_json_u8(root, "ext_pwr_pol", &candidate.ext_pwr_pol, &invalid);
    touched |= get_json_u8(root, "ext_rst_pol", &candidate.ext_rst_pol, &invalid);
    {
        uint16_t pulse = 0;
        if (get_json_u16(root, "ext_rst_pulse_ms", &pulse, &invalid)) {
            candidate.ext_rst_pulse_ms = pulse;
            touched = true;
        }
    }

    // I2C bus and sensor population.
    touched |= get_json_u8(root, "i2c_enable", &candidate.i2c_enable, &invalid);
    touched |= get_json_u8(root, "sht21_enable", &candidate.sht21_enable, &invalid);
    touched |= get_json_u8(root, "ina219_enable", &candidate.ina219_enable, &invalid);
    touched |= get_json_u8(root, "ina226_enable", &candidate.ina226_enable, &invalid);
    touched |= get_json_byte(root, "ina219_addr", &candidate.ina219_addr, &invalid);
    touched |= get_json_byte(root, "ina226_addr", &candidate.ina226_addr, &invalid);

    cJSON* pins = cJSON_GetObjectItem(root, "pins");
    if (pins && cJSON_IsObject(pins)) {
        touched |= get_json_pin(pins, "uart0_tx", &candidate.pins.uart0_tx, &invalid);
        touched |= get_json_pin(pins, "uart0_rx", &candidate.pins.uart0_rx, &invalid);
        touched |= get_json_pin(pins, "uart1_tx", &candidate.pins.uart1_tx, &invalid);
        touched |= get_json_pin(pins, "uart1_rx", &candidate.pins.uart1_rx, &invalid);
        touched |= get_json_pin(pins, "sdspi_cs", &candidate.pins.sdspi_cs, &invalid);
        touched |= get_json_pin(pins, "sdspi_mosi", &candidate.pins.sdspi_mosi, &invalid);
        touched |= get_json_pin(pins, "sdspi_miso", &candidate.pins.sdspi_miso, &invalid);
        touched |= get_json_pin(pins, "sdspi_sclk", &candidate.pins.sdspi_sclk, &invalid);
        touched |= get_json_pin(pins, "i2c_sda", &candidate.pins.i2c_sda, &invalid);
        touched |= get_json_pin(pins, "i2c_scl", &candidate.pins.i2c_scl, &invalid);
        touched |= get_json_pin(pins, "sensor_1", &candidate.pins.sensor_1, &invalid);
        touched |= get_json_pin(pins, "sensor_2", &candidate.pins.sensor_2, &invalid);
        touched |= get_json_pin(pins, "led_red", &candidate.pins.led_red, &invalid);
        touched |= get_json_pin(pins, "led_green", &candidate.pins.led_green, &invalid);
        touched |= get_json_pin(pins, "ext_pwr", &candidate.pins.ext_pwr, &invalid);
        touched |= get_json_pin(pins, "ext_rst", &candidate.pins.ext_rst, &invalid);
    }

    bool ssid_set = get_json_string(root, "wifi_ssid", &candidate.wifi_ssid);
    bool pw_set   = get_json_string(root, "wifi_password", &candidate.wifi_password);
    bool url_set  = get_json_string(root, "server_url", &candidate.server_url);
    bool mqtt_url_set   = get_json_string(root, "mqtt_broker_url", &candidate.mqtt_broker_url);
    bool mqtt_topic_set = get_json_string(root, "mqtt_topic_prefix", &candidate.mqtt_topic_prefix);
    touched |= ssid_set | pw_set | url_set | mqtt_url_set | mqtt_topic_set;

    // Reject over-long strings rather than letting the driver/NVS truncate
    // them silently (AP ssid buffer is 32 bytes incl. NUL, password 64, and
    // NVS string values are bounded to keep commits reliable).
    bool too_long = (ssid_set && strlen(candidate.wifi_ssid) > 31) ||
                    (pw_set && strlen(candidate.wifi_password) > 63) ||
                    (url_set && strlen(candidate.server_url) > 255) ||
                    (mqtt_url_set && strlen(candidate.mqtt_broker_url) > 255) ||
                    (mqtt_topic_set && strlen(candidate.mqtt_topic_prefix) > 64);

    char err[192] = { 0 };
    bool ok = false;
    if (invalid) {
        snprintf(err, sizeof(err), "Numeric value out of range");
    } else if (too_long) {
        snprintf(err, sizeof(err),
                 "String too long (wifi_ssid<=31, wifi_password<=63, server_url<=255, mqtt_broker_url<=255, mqtt_topic_prefix<=64)");
    } else if (!touched) {
        snprintf(err, sizeof(err), "No valid configuration fields provided");
    } else {
        ok = config_validate_and_set(&candidate, err, sizeof(err));
    }

    free(candidate.wifi_ssid);
    free(candidate.wifi_password);
    free(candidate.server_url);
    free(candidate.mqtt_broker_url);
    free(candidate.mqtt_topic_prefix);
    cJSON_Delete(root);

    if (invalid || too_long || !touched || !ok) {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"error\":\"%s\"}", err);
        return reply_json(req, "400 Bad Request", resp);
    }

    return reply_json(req, "200 OK",
                      "{\"message\":\"Configuration saved. Reboot to apply.\"}");
}

static esp_err_t handle_get_sensor_data(httpd_req_t *req)
{
    char* sensor_data = get_sensor_data_json();
    if (sensor_data) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, sensor_data, strlen(sensor_data));
        free(sensor_data);
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to get sensor data", -1);
    }
    return ESP_OK;
}

static esp_err_t handle_post_sensor_data(httpd_req_t *req)
{
    // Body is accepted and discarded (no real sensor ingestion pipeline yet).
    char body[512];
    (void)read_body(req, body, sizeof(body));
    return reply_json(req, "200 OK", "{\"received\":true}");
}

static esp_err_t handle_get_sd_status(httpd_req_t *req)
{
    const app_config_t* cfg = get_config();
    if (!cfg->sd_enable || get_sd_status() != SD_STATUS_READY) {
        return reply_json(req, "503 Service Unavailable", "{\"error\":\"SD disabled\"}");
    }
    return reply_json(req, "200 OK", "{\"status\":\"ready\"}");
}

static esp_err_t handle_post_sd_write(httpd_req_t *req)
{
    const app_config_t* cfg = get_config();
    if (!cfg->sd_enable || get_sd_status() != SD_STATUS_READY) {
        return reply_json(req, "503 Service Unavailable", "{\"error\":\"SD disabled\"}");
    }

    char body[512];
    int received = read_body(req, body, sizeof(body));
    if (received <= 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Empty body\"}");
    }

    if (sd_write_data("upload.bin", (const uint8_t*)body, (size_t)received) != 0) {
        return reply_json(req, "500 Internal Server Error", "{\"error\":\"SD write failed\"}");
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"written\":%d}", received);
    return reply_json(req, "200 OK", resp);
}

static esp_err_t handle_get_ext_status(httpd_req_t *req)
{
    char* json = ext_get_status_json();
    if (!json) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Failed to get external status\"}");
    }
    esp_err_t err = reply_json(req, "200 OK", json);
    free(json);
    return err;
}

static esp_err_t handle_post_ext_power(httpd_req_t *req)
{
    if (!request_is_same_origin(req)) {
        return reply_json(req, "403 Forbidden",
                          "{\"error\":\"Cross-origin request rejected\"}");
    }

    const app_config_t* cfg = get_config();
    if (!cfg->ext_pwr_enable) {
        return reply_json(req, "503 Service Unavailable",
                          "{\"error\":\"External power control disabled\"}");
    }

    char body[256];
    int received = read_body(req, body, sizeof(body));
    if (received < 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Failed to read request body\"}");
    }

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
    }

    cJSON* on = cJSON_GetObjectItem(root, "on");
    if (!on || !cJSON_IsBool(on)) {
        cJSON_Delete(root);
        return reply_json(req, "400 Bad Request", "{\"error\":\"Missing 'on' field\"}");
    }
    bool value = cJSON_IsTrue(on);
    cJSON_Delete(root);

    // Apply the request and report the authoritative post-operation state.
    (void)ext_power_set(value);
    bool state = ext_power_get();
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"power_state\":%s,\"message\":\"Power %s\"}",
             state ? "true" : "false", state ? "ON" : "OFF");
    return reply_json(req, "200 OK", resp);
}

static esp_err_t handle_post_ext_reset(httpd_req_t *req)
{
    if (!request_is_same_origin(req)) {
        return reply_json(req, "403 Forbidden",
                          "{\"error\":\"Cross-origin request rejected\"}");
    }

    const app_config_t* cfg = get_config();
    if (!cfg->ext_rst_enable) {
        return reply_json(req, "503 Service Unavailable",
                          "{\"error\":\"External reset control disabled\"}");
    }

    // The body is optional and ignored.
    char body[64];
    (void)read_body(req, body, sizeof(body));

    if (!ext_reset_pulse()) {
        return reply_json(req, "429 Too Many Requests",
                          "{\"error\":\"Reset throttled, try again shortly\"}");
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"message\":\"Reset pulse sent (%ums)\"}",
             (unsigned)cfg->ext_rst_pulse_ms);
    return reply_json(req, "200 OK", resp);
}

void init_http_server(void)
{
    if (server) {
        ESP_LOGW(TAG, "HTTP server already initialized");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = tskIDLE_PRIORITY + 5;
    config.stack_size = 8192;
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 10;
    config.max_uri_handlers = 20;
    config.max_resp_headers = 8;
    config.backlog_conn = 16;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    // Enable wildcard URI matching so the static handler can own "/*" while
    // exact REST endpoints (registered first) keep priority.
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started");

    // Register endpoints
    static const http_endpoint_t endpoints[] = {
        {"/config", HTTP_METHOD_GET, handle_get_config},
        {"/config", HTTP_METHOD_POST, handle_post_config},
        {"/sensor", HTTP_METHOD_GET, handle_get_sensor_data},
        {"/sensor", HTTP_METHOD_POST, handle_post_sensor_data},
        {"/sd/status", HTTP_METHOD_GET, handle_get_sd_status},
        {"/sd/write", HTTP_METHOD_POST, handle_post_sd_write},
        {"/logs", HTTP_METHOD_GET, handle_get_logs},
        {"/wifi/scan", HTTP_METHOD_GET, handle_wifi_scan},
        {"/wifi/scan/results", HTTP_METHOD_GET, handle_wifi_scan_results},
        {"/wifi/connect", HTTP_METHOD_POST, handle_wifi_connect},
        {"/wifi/status", HTTP_METHOD_GET, handle_wifi_status},
        {"/wifi/disconnect", HTTP_METHOD_POST, handle_wifi_disconnect},
        {"/system/reboot", HTTP_METHOD_POST, handle_system_reboot},
        {"/system/reset", HTTP_METHOD_POST, handle_system_reset},
        {"/ext/status", HTTP_METHOD_GET, handle_get_ext_status},
        {"/ext/power", HTTP_METHOD_POST, handle_post_ext_power},
        {"/ext/reset", HTTP_METHOD_POST, handle_post_ext_reset},
        // Catch-all static handler must stay last so the exact REST routes
        // above win the URI match.
        {"/*", HTTP_METHOD_GET, handle_web_static},
    };

    for (int i = 0; i < sizeof(endpoints)/sizeof(endpoints[0]); i++) {
        register_http_endpoint(&endpoints[i]);
    }
}

void register_http_endpoint(const http_endpoint_t* endpoint)
{
    if (!server || !endpoint) {
        return;
    }

    httpd_method_t method = HTTP_GET;
    switch (endpoint->method) {
        case HTTP_METHOD_POST: method = HTTP_POST; break;
        case HTTP_METHOD_PUT: method = HTTP_PUT; break;
        case HTTP_METHOD_DELETE: method = HTTP_DELETE; break;
        default: method = HTTP_GET; break;
    }

    esp_err_t err = httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri = endpoint->path,
        .method = method,
        .handler = endpoint->handler,
        .user_ctx = NULL
    });
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register endpoint %s: %s", endpoint->path, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Registered endpoint %s", endpoint->path);
    }
}

void unregister_http_endpoint(const char* path)
{
    if (!server || !path) {
        return;
    }
    
    esp_err_t err = httpd_unregister_uri(server, path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister endpoint %s: %s", path, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Unregistered endpoint %s", path);
    }
}