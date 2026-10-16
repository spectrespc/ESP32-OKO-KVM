// main/self_test.c
#include "self_test.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "self_test";

typedef struct {
    const char* method;
    const char* path;
    int expected_status;
} self_test_case_t;

// Expected responses come from tests/http_endpoints.csv via the generated
// header, which is the single source of truth shared with build_and_flash.sh.
#include "http_tests_generated.h"

static int http_request(const char* base, const char* method, const char* path)
{
    char url[128];
    snprintf(url, sizeof(url), "%s%s", base, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    if (strcmp(method, "POST") == 0) {
        cfg.method = HTTP_METHOD_POST;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -1;
    }

    if (cfg.method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, "{}", 2);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

static void self_test_task(void* arg)
{
    (void)arg;

    // Give the AP + DHCP server + HTTP server time to settle.
    vTaskDelay(pdMS_TO_TICKS(3000));

    bool have_ip = false;
    char base[64] = "http://192.168.4.1";
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(base, sizeof(base), "http://" IPSTR, IP2STR(&ip_info.ip));
            have_ip = true;
        }
    }

    if (!have_ip) {
        ESP_LOGW(TAG, "SELF-TEST SKIPPED: SoftAP has no IP address");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "==== HTTP self-test over internal AP (%s) ====", base);
    int pass = 0;
    int fail = 0;
    for (size_t i = 0; i < sizeof(s_tests) / sizeof(s_tests[0]); i++) {
        // This self-test runs on every boot, so it must not exercise the
        // destructive system endpoints (reboot/reset) or the external-device
        // reset (which would pulse the attached device on every boot): doing so
        // would restart hardware mid-test. build_and_flash.sh still covers them
        // over the network.
        if (strncmp(s_tests[i].path, "/system/", 8) == 0 ||
            strcmp(s_tests[i].path, "/ext/reset") == 0) {
            ESP_LOGW(TAG, "SELF-TEST SKIP %s %s (destructive)",
                     s_tests[i].method, s_tests[i].path);
            continue;
        }
        int code = http_request(base, s_tests[i].method, s_tests[i].path);
        if (code == s_tests[i].expected_status) {
            ESP_LOGI(TAG, "SELF-TEST PASS %s %s -> %d",
                     s_tests[i].method, s_tests[i].path, code);
            pass++;
        } else {
            ESP_LOGE(TAG, "SELF-TEST FAIL %s %s -> %d (expected %d)",
                     s_tests[i].method, s_tests[i].path, code, s_tests[i].expected_status);
            fail++;
        }
    }
    ESP_LOGI(TAG, "==== HTTP self-test summary: %d passed, %d failed ====", pass, fail);

    vTaskDelete(NULL);
}

void start_self_test(void)
{
    if (xTaskCreate(self_test_task, "self_test", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start self-test task");
    }
}