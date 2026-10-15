// main/wifi_ap.c
#include "wifi_ap.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "config.h"
#include <string.h>

static const char* TAG = "wifi_ap";

#define WIFI_AP_SSID     "ESP32-Gateway"
#define WIFI_AP_PASSWORD "gateway123"
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_STA  4

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info.ip));
            }
        }
    }
}

esp_err_t wifi_init(void)
{
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi AP netif");
        return ESP_FAIL;
    }

    // The STA netif must exist before esp_wifi_start(): creating it afterwards
    // leaves the LwIP DHCP client unarmed, so the first successful association
    // fails with "dhcp client start failed" and crashes. Both default netifs
    // for the AP+STA radio are created up front.
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
            return ESP_FAIL;
        }
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_ap_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // AP+STA so the built-in SoftAP stays available while the station also
    // connects out to an external network.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // Credentials come from NVS-backed configuration when provisioned,
    // otherwise fall back to the built-in defaults.
    app_config_t* config = get_config();
    const char* ssid = (config && config->wifi_ssid && config->wifi_ssid[0])
                           ? config->wifi_ssid
                           : WIFI_AP_SSID;
    const char* password = (config && config->wifi_password && config->wifi_password[0])
                               ? config->wifi_password
                               : WIFI_AP_PASSWORD;

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = WIFI_AP_MAX_STA,
        },
    };
    strlcpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    // Derive the length from the (possibly truncated) buffer, never from the
    // source string, so ssid_len can never exceed what was actually copied.
    ap_cfg.ap.ssid_len = strnlen((char*)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char*)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password));
    if (strlen(password) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi AP started SSID:%s", ssid);
    return ESP_OK;
}