// main/auth_handler.h
#ifndef AUTH_HANDLER_H
#define AUTH_HANDLER_H

#include <stdbool.h>
#include "esp_http_server.h"

// Maximum number of concurrent sessions held in RAM (lost on reboot).
#define AUTH_MAX_SESSIONS 3

// Loads the credentials from the "auth" NVS namespace, creating the default
// admin/admin pair (password stored as a SHA-256 hex digest) on first boot.
// Call once before init_http_server().
void auth_init(void);

// Returns true when the request carries a valid session cookie, or when it
// originates from one of the device's own IP addresses (loopback bypass, so
// the on-device HTTP self-test keeps working without credentials).
bool auth_check(httpd_req_t *req);

// POST /auth/login   {"username","password"}
esp_err_t handle_auth_login(httpd_req_t *req);
// POST /auth/logout
esp_err_t handle_auth_logout(httpd_req_t *req);
// GET /auth/status
esp_err_t handle_auth_status(httpd_req_t *req);
// POST /auth/password {"current","new"[,"username"]}
esp_err_t handle_auth_password(httpd_req_t *req);

#endif // AUTH_HANDLER_H