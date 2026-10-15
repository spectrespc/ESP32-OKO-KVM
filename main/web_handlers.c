// main/web_handlers.c
#include "web_handlers.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>

static const char* TAG = "web_handlers";

#define SPIFFS_BASE_PATH   "/spiffs"
#define SPIFFS_PARTITION   "spiffs"
#define WEB_PATH_MAX       256
#define WEB_FULL_PATH_MAX  (WEB_PATH_MAX + 16)

static bool s_mounted = false;

void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = SPIFFS_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s): %s",
                 SPIFFS_PARTITION, esp_err_to_name(err));
        return;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(SPIFFS_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s (%u/%u bytes used)",
                 SPIFFS_BASE_PATH, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at %s", SPIFFS_BASE_PATH);
    }
    s_mounted = true;
}

bool spiffs_is_mounted(void)
{
    return s_mounted;
}

static const char* content_type_for(const char* path)
{
    const char* dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".css") == 0)  return "text/css";
    if (strcasecmp(dot, ".js") == 0)   return "application/javascript";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcasecmp(dot, ".png") == 0)  return "image/png";
    if (strcasecmp(dot, ".ico") == 0)  return "image/x-icon";
    if (strcasecmp(dot, ".txt") == 0)  return "text/plain";
    return "application/octet-stream";
}

// Returns true when the client's Accept header prefers JSON (so the root URL
// is answered with a JSON 404 instead of the HTML SPA).
static bool client_wants_json(httpd_req_t* req)
{
    char accept[128];
    size_t len = httpd_req_get_hdr_value_len(req, "Accept");
    if (len == 0 || len >= sizeof(accept)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) != ESP_OK) {
        return false;
    }
    return strstr(accept, "application/json") != NULL;
}

esp_err_t handle_web_static(httpd_req_t* req)
{
    // Strip any query string from the request URI.
    const char* uri = req->uri;
    const char* query = strchr(uri, '?');
    size_t uri_len = query ? (size_t)(query - uri) : strlen(uri);

    char path[WEB_PATH_MAX];
    if (uri_len >= sizeof(path)) {
        uri_len = sizeof(path) - 1;
    }
    memcpy(path, uri, uri_len);
    path[uri_len] = '\0';

    // Reject path traversal attempts outright.
    if (strstr(path, "..")) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Bad Request", -1);
        return ESP_OK;
    }

    if (strcmp(path, "/") == 0) {
        if (client_wants_json(req)) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Not Found\"}", -1);
            return ESP_OK;
        }
        strlcpy(path, "/index.html", sizeof(path));
    }

    if (!spiffs_is_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Web UI storage not mounted", -1);
        return ESP_OK;
    }

    char full_path[WEB_FULL_PATH_MAX];
    snprintf(full_path, sizeof(full_path), SPIFFS_BASE_PATH "%s", path);

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "404 Not Found", -1);
        return ESP_OK;
    }

    FILE* f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Failed to open file", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type_for(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char buf[512];
    size_t read;
    esp_err_t err = ESP_OK;
    while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);

    // Terminate the chunked response (also on the error path, to close the
    // transfer cleanly before the socket is reused).
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}