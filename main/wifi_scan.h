// main/wifi_scan.h
#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include "esp_err.h"
#include "esp_http_server.h"

// Brings up the STA side of the AP+STA radio: registers STA/IP event handlers,
// creates the station netif and reconnects using the credentials previously
// saved in NVS (keys sta_ssid/sta_password). Call once after wifi_init().
void init_wifi_sta(void);

// Handles GET /wifi/scan: starts a non-blocking scan and returns immediately
// with 202 {"status":"scanning"}.
esp_err_t handle_wifi_scan(httpd_req_t *req);

// Handles GET /wifi/scan/results: returns {"status":"scanning"} while a scan is
// running, otherwise {"networks":[{"ssid","rssi","authmode","channel"},...]}.
esp_err_t handle_wifi_scan_results(httpd_req_t *req);

// Handles POST /wifi/connect: {"ssid":"...","password":"..."} (password may be
// empty for open networks). Saves the credentials and starts the connection.
esp_err_t handle_wifi_connect(httpd_req_t *req);

// Handles GET /wifi/status:
// {"connected":bool,"ssid":"...","ip":"...","rssi":-45,"state":"connected"}
esp_err_t handle_wifi_status(httpd_req_t *req);

// Handles POST /wifi/disconnect: drops the STA link and forgets the saved
// credentials.
esp_err_t handle_wifi_disconnect(httpd_req_t *req);

#endif // WIFI_SCAN_H