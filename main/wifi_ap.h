// main/wifi_ap.h
#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"

// Initializes the WiFi radio in AP+STA mode and starts the built-in SoftAP.
// The STA side is brought up separately by init_wifi_sta().
esp_err_t wifi_init(void);

#endif // WIFI_AP_H