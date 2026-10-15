// main/web_handlers.h
#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "esp_http_server.h"

// Mounts the SPIFFS partition that holds the SPA (index.html). Safe to call
// once at boot before init_http_server(). Logs and returns on failure.
void init_spiffs(void);

// Returns true when the SPIFFS filesystem is mounted and usable.
bool spiffs_is_mounted(void);

// Catch-all GET handler that serves files from the mounted SPIFFS image.
// "/" maps to "/index.html". Requests for "/" that advertise
// "Accept: application/json" are answered with a JSON 404 so REST clients do
// not receive HTML. Unknown paths return a plain 404.
esp_err_t handle_web_static(httpd_req_t *req);

#endif // WEB_HANDLERS_H