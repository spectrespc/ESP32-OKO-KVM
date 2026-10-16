// main/system_handler.h
#ifndef SYSTEM_HANDLER_H
#define SYSTEM_HANDLER_H

#include "esp_http_server.h"
#include <stdbool.h>

// Basic CSRF guard shared by the destructive POST endpoints: returns true when
// the request has no Origin header (e.g. curl) or its Origin host matches the
// request Host (port ignored). Returns false for cross-origin browser requests.
bool request_is_same_origin(httpd_req_t *req);

// Handles POST /system/reboot: acknowledges with HTTP 200, then restarts the
// device so any saved configuration is applied.
esp_err_t handle_system_reboot(httpd_req_t *req);

// Handles POST /system/reset: acknowledges with HTTP 200, erases the NVS
// partition (factory defaults) and restarts the device.
esp_err_t handle_system_reset(httpd_req_t *req);

#endif // SYSTEM_HANDLER_H