// main/web_handlers.c
#include "web_handlers.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>

// The SPA is embedded in the application image (EMBED_FILES in
// main/CMakeLists.txt). This keeps the web UI and the REST API in one OTA
// artifact, so a network update always replaces both and no separate SPIFFS
// image has to be flashed over USB.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static const char* TAG = "web_handlers";

#define WEB_PATH_MAX 256

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

    // The SPA uses hash-based routing, so only the root document exists.
    if (strcmp(path, "/") == 0) {
        if (client_wants_json(req)) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Not Found\"}", -1);
            return ESP_OK;
        }
    } else if (strcmp(path, "/index.html") != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "404 Not Found", -1);
        return ESP_OK;
    }

    size_t len = (size_t)(index_html_end - index_html_start);
    ESP_LOGD(TAG, "Serving embedded index.html (%u bytes)", (unsigned)len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)index_html_start, len);
}