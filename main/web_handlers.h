// main/web_handlers.h
#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "esp_http_server.h"

// Catch-all GET handler that serves the SPA embedded in the application image
// (EMBED_FILES). "/" maps to index.html. Requests for "/" that advertise
// "Accept: application/json" are answered with a JSON 404 so REST clients do
// not receive HTML. Unknown paths return a plain 404.
esp_err_t handle_web_static(httpd_req_t *req);

#endif // WEB_HANDLERS_H