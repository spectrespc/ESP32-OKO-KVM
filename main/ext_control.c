// main/ext_control.c
#include "ext_control.h"
#include "config.h"
#include "pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdlib.h>

static const char* TAG = "ext_control";

// Minimum spacing between two reset pulses (throttling). A reset is a slow,
// side-effecting operation; back-to-back requests are rejected.
#define EXT_RESET_MIN_INTERVAL_US (2LL * 1000 * 1000)

static bool s_initialized = false;
static bool s_power_on = false;
static int64_t s_last_reset_us = 0;

// Live power state. The power pin is configured output-only (see
// init_ext_pins()), and gpio_get_level() always returns 0 when the pad input is
// disabled, so we report the authoritative state tracked by
// init_ext_control()/ext_power_set() instead of reading the pin.
static bool current_power_state(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg->ext_pwr_enable) {
        return false;
    }
    return s_power_on;
}

void init_ext_control(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "External control already initialized");
        return;
    }

    init_ext_pins();

    const app_config_t* cfg = get_config();
    s_power_on = cfg->ext_pwr_state != 0;
    s_initialized = true;

    ESP_LOGI(TAG, "External control initialized (pwr=GPIO%d pol=%s, rst=GPIO%d pol=%s pulse=%ums)",
             (int)cfg->pins.ext_pwr, cfg->ext_pwr_pol ? "high" : "low",
             (int)cfg->pins.ext_rst, cfg->ext_rst_pol ? "high" : "low",
             (unsigned)cfg->ext_rst_pulse_ms);
    ESP_LOGI(TAG, "Restored power state: %s", s_power_on ? "ON" : "OFF");
}

bool ext_power_set(bool on)
{
    app_config_t* cfg = get_config();
    if (!s_initialized || !cfg->ext_pwr_enable) {
        ESP_LOGW(TAG, "External power control disabled");
        return false;
    }

    const int on_level = ext_power_on_level(cfg->ext_pwr_pol);
    const int off_level = ext_power_off_level(cfg->ext_pwr_pol);
    gpio_set_level(cfg->pins.ext_pwr, on ? on_level : off_level);

    s_power_on = on;
    config_set_ext_power_state(on ? 1 : 0);

    ESP_LOGI(TAG, "External power %s (GPIO%d=%d)",
             on ? "ON" : "OFF", (int)cfg->pins.ext_pwr, on ? on_level : off_level);
    return s_power_on;
}

bool ext_power_get(void)
{
    return current_power_state();
}

bool ext_reset_pulse(void)
{
    const app_config_t* cfg = get_config();
    if (!s_initialized || !cfg->ext_rst_enable) {
        ESP_LOGW(TAG, "External reset control disabled");
        return false;
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_reset_us != 0 && (now - s_last_reset_us) < EXT_RESET_MIN_INTERVAL_US) {
        ESP_LOGW(TAG, "External reset throttled (%lld ms since last pulse)",
                 (long long)((now - s_last_reset_us) / 1000));
        return false;
    }
    s_last_reset_us = now;

    const int active = ext_reset_active_level(cfg->ext_rst_pol);
    const int inactive = ext_reset_inactive_level(cfg->ext_rst_pol);

    gpio_set_level(cfg->pins.ext_rst, active);
    // Blocking by design (see the plan): run from the HTTP handler and delay for
    // the pulse width (<= 5000 ms). Offload to a FreeRTOS task if longer pulses
    // or concurrent request handling are ever required.
    vTaskDelay(pdMS_TO_TICKS(cfg->ext_rst_pulse_ms));
    gpio_set_level(cfg->pins.ext_rst, inactive);

    ESP_LOGI(TAG, "External reset pulse sent (%ums, GPIO%d)",
             (unsigned)cfg->ext_rst_pulse_ms, (int)cfg->pins.ext_rst);
    return true;
}

char* ext_get_status_json(void)
{
    const app_config_t* cfg = get_config();

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "power_enabled", cfg->ext_pwr_enable != 0);
    cJSON_AddBoolToObject(root, "reset_enabled", cfg->ext_rst_enable != 0);
    cJSON_AddBoolToObject(root, "power_state", current_power_state());
    cJSON_AddNumberToObject(root, "power_pin", cfg->pins.ext_pwr);
    cJSON_AddNumberToObject(root, "reset_pin", cfg->pins.ext_rst);
    cJSON_AddStringToObject(root, "power_polarity",
                            cfg->ext_pwr_pol ? "active-high" : "active-low");
    cJSON_AddStringToObject(root, "reset_polarity",
                            cfg->ext_rst_pol ? "active-high" : "active-low");
    cJSON_AddNumberToObject(root, "reset_pulse_ms", cfg->ext_rst_pulse_ms);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}