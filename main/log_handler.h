// main/log_handler.h
#ifndef LOG_HANDLER_H
#define LOG_HANDLER_H

#include "esp_http_server.h"

// Installs a vprintf hook that mirrors every formatted ESP_LOG line into an
// in-RAM ring buffer, then exposes that buffer over GET /logs. Call once at
// boot (after NVS init) before other subsystems start logging.
void init_log_handler(void);

// Handles GET /logs: {"logs": ["line1", "line2", ...]} in chronological order.
esp_err_t handle_get_logs(httpd_req_t *req);

#endif // LOG_HANDLER_H