// main/wifi_scan.h
#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include "esp_err.h"
#include "esp_http_server.h"

// Brings up the STA side of the AP+STA radio: registers STA/IP event handlers,
// creates the station netif, starts the wifi_sup supervisor task and connects
// using the credentials previously saved in NVS (keys sta_ssid/sta_password).
// The supervisor reconnects indefinitely with 1-60 s backoff and performs a
// soft radio restart after 10 minutes of continuous failures. Call once after
// wifi_init().
void init_wifi_sta(void);

// Handles GET /wifi/scan: starts a non-blocking scan and returns immediately
// with 202 {"status":"scanning"}.
esp_err_t handle_wifi_scan(httpd_req_t *req);

// Handles GET /wifi/scan/results: returns {"status":"scanning"} while a scan is
// running, otherwise {"networks":[{"ssid","rssi","authmode","channel"},...]}.
esp_err_t handle_wifi_scan_results(httpd_req_t *req);

// Handles POST /wifi/connect: {"ssid":"...","password":"..."} (password may be
// empty for open networks). Saves the credentials, resets the failure counters
// and starts the connection; the supervisor then retries indefinitely.
esp_err_t handle_wifi_connect(httpd_req_t *req);

// Handles GET /wifi/status:
// {"connected":bool,"ssid":"...","ip":"...","rssi":-45,"state":"connected",
//  "attempts":3,"fail_streak":0,"last_disconnect_reason":200,
//  "last_disconnect_reason_name":"BEACON_TIMEOUT","last_disconnect_age_s":42,
//  "next_retry_in_ms":0,"radio_restarts":0,"power_save":"none"}
esp_err_t handle_wifi_status(httpd_req_t *req);

// Handles POST /wifi/disconnect: drops the STA link, forgets the saved
// credentials and stops automatic reconnection until the next /wifi/connect.
esp_err_t handle_wifi_disconnect(httpd_req_t *req);

#endif // WIFI_SCAN_H