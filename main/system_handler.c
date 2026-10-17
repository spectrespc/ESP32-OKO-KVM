// main/system_handler.c
#include "system_handler.h"
#include "auth_handler.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "system_handler";

// Give the HTTP stack a moment to flush the response to the client before the
// radio restarts and the connection drops.
#define REBOOT_FLUSH_MS 300

// Rejects cross-origin browser requests to the destructive endpoints (a basic
// CSRF guard). Requests without an Origin header (e.g. curl) are allowed, since
// the endpoint is only reachable on the local SoftAP/STA network.
bool request_is_same_origin(httpd_req_t* req)
{
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (olen == 0) {
        return true;
    }
    char origin[128];
    if (olen >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return false;
    }
    // Compare only the host[:port] portion of "scheme://host[:port]".
    const char* ohost = strstr(origin, "://");
    ohost = ohost ? ohost + 3 : origin;

    size_t hlen = httpd_req_get_hdr_value_len(req, "Host");
    char hosthdr[128];
    if (hlen == 0 || hlen >= sizeof(hosthdr) ||
        httpd_req_get_hdr_value_str(req, "Host", hosthdr, sizeof(hosthdr)) != ESP_OK) {
        return false;
    }

    // Ignore any port when comparing, so "host" and "host:80" are equivalent.
    size_t oname_len = strcspn(ohost, ":");
    size_t hname_len = strcspn(hosthdr, ":");
    return oname_len > 0 && oname_len == hname_len &&
           strncmp(ohost, hosthdr, oname_len) == 0;
}

static esp_err_t reject_cross_origin(httpd_req_t* req)
{
    ESP_LOGW(TAG, "Rejected cross-origin request to %s", req->uri);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Cross-origin request rejected\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t reject_unauthorized(httpd_req_t* req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void reply_and_wait(httpd_req_t* req, const char* body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_MS));
}

esp_err_t handle_system_reboot(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    if (!request_is_same_origin(req)) {
        return reject_cross_origin(req);
    }
    ESP_LOGW(TAG, "Reboot requested via web UI");
    reply_and_wait(req, "{\"status\":\"rebooting\"}");
    esp_restart();
    return ESP_OK; // not reached
}

esp_err_t handle_system_reset(httpd_req_t* req)
{
    if (!auth_check(req)) {
        return reject_unauthorized(req);
    }
    if (!request_is_same_origin(req)) {
        return reject_cross_origin(req);
    }
    ESP_LOGW(TAG, "Factory reset requested via web UI");
    reply_and_wait(req, "{\"status\":\"resetting\"}");
    ESP_LOGW(TAG, "Erasing NVS partition and restarting");
    nvs_flash_erase();
    esp_restart();
    return ESP_OK; // not reached
}