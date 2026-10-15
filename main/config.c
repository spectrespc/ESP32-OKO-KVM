// main/config.c
#include "config.h"
#include "pins.h"
#include "pin_validation.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* TAG = "config";
static app_config_t app_config;
static bool config_loaded = false;
static nvs_handle_t s_nvs_handle = 0;

// NVS keys are limited to 15 characters; all pin keys stay within that.
static void save_pin(nvs_handle_t h, const char* key, gpio_num_t pin)
{
    esp_err_t err = nvs_set_u32(h, key, (uint32_t)pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
    }
}

static void save_u8(nvs_handle_t h, const char* key, uint8_t value)
{
    esp_err_t err = nvs_set_u8(h, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
    }
}

static gpio_num_t load_pin(nvs_handle_t h, const char* key, gpio_num_t def)
{
    uint32_t value = 0;
    if (nvs_get_u32(h, key, &value) == ESP_OK) {
        return (gpio_num_t)value;
    }
    return def;
}

static void set_pin_defaults(pin_config_t* p)
{
    p->uart0_tx   = PIN_DEFAULT_UART0_TX;
    p->uart0_rx   = PIN_DEFAULT_UART0_RX;
    p->uart1_tx   = PIN_DEFAULT_UART1_TX;
    p->uart1_rx   = PIN_DEFAULT_UART1_RX;
    p->sdspi_cs   = PIN_DEFAULT_SDSPI_CS;
    p->sdspi_mosi = PIN_DEFAULT_SDSPI_MOSI;
    p->sdspi_miso = PIN_DEFAULT_SDSPI_MISO;
    p->sdspi_sclk = PIN_DEFAULT_SDSPI_SCLK;
    p->i2c_sda    = PIN_DEFAULT_I2C_SDA;
    p->i2c_scl    = PIN_DEFAULT_I2C_SCL;
    p->sensor_1   = PIN_DEFAULT_SENSOR_1;
    p->sensor_2   = PIN_DEFAULT_SENSOR_2;
    p->led_red    = PIN_DEFAULT_LED_RED;
    p->led_green  = PIN_DEFAULT_LED_GREEN;
}

void init_config(void)
{
    // Note: nvs_flash_init() is called in main.c already
    // We should not call it again here to avoid redundancy

    esp_err_t err = nvs_open("storage", NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        set_pin_defaults(&app_config.pins);
        return;
    }

    load_config();
    ESP_LOGI(TAG, "Configuration initialized");
}

app_config_t* get_config(void)
{
    return &app_config;
}

char* get_config_json(void)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "baud_rate", app_config.baud_rate);
    cJSON_AddNumberToObject(root, "uart0_enable", app_config.uart0_enable);
    cJSON_AddNumberToObject(root, "uart1_enable", app_config.uart1_enable);
    cJSON_AddNumberToObject(root, "sd_enable", app_config.sd_enable);
    cJSON_AddNumberToObject(root, "sensor_enable", app_config.sensor_enable);
    cJSON_AddNumberToObject(root, "led_enable", app_config.led_enable);
    cJSON_AddNumberToObject(root, "telnet0_enable", app_config.telnet0_enable);
    cJSON_AddNumberToObject(root, "telnet0_port", app_config.telnet0_port);
    cJSON_AddNumberToObject(root, "telnet1_enable", app_config.telnet1_enable);
    cJSON_AddNumberToObject(root, "telnet1_port", app_config.telnet1_port);
    cJSON_AddNumberToObject(root, "uart0_sd_log", app_config.uart0_sd_log);
    cJSON_AddNumberToObject(root, "uart1_sd_log", app_config.uart1_sd_log);
    cJSON_AddNumberToObject(root, "sht21_sd_log", app_config.sht21_sd_log);
    cJSON_AddNumberToObject(root, "ina219_sd_log", app_config.ina219_sd_log);
    cJSON_AddNumberToObject(root, "ina226_sd_log", app_config.ina226_sd_log);
    cJSON_AddNumberToObject(root, "env_sd_log", app_config.env_sd_log);
    cJSON_AddNumberToObject(root, "i2c_enable", app_config.i2c_enable);
    cJSON_AddNumberToObject(root, "sht21_enable", app_config.sht21_enable);
    cJSON_AddNumberToObject(root, "ina219_enable", app_config.ina219_enable);
    cJSON_AddNumberToObject(root, "ina226_enable", app_config.ina226_enable);
    cJSON_AddNumberToObject(root, "ina219_addr", app_config.ina219_addr);
    cJSON_AddNumberToObject(root, "ina226_addr", app_config.ina226_addr);
    cJSON_AddStringToObject(root, "wifi_ssid",
                            app_config.wifi_ssid ? app_config.wifi_ssid : "");
    cJSON_AddStringToObject(root, "server_url",
                            app_config.server_url ? app_config.server_url : "");
    cJSON_AddNumberToObject(root, "mqtt_enable", app_config.mqtt_enable);
    cJSON_AddStringToObject(root, "mqtt_broker_url",
                            app_config.mqtt_broker_url ? app_config.mqtt_broker_url : "");
    cJSON_AddStringToObject(root, "mqtt_topic_prefix",
                            app_config.mqtt_topic_prefix ? app_config.mqtt_topic_prefix : "");

    cJSON* pins = cJSON_CreateObject();
    if (pins) {
        cJSON_AddNumberToObject(pins, "uart0_tx", app_config.pins.uart0_tx);
        cJSON_AddNumberToObject(pins, "uart0_rx", app_config.pins.uart0_rx);
        cJSON_AddNumberToObject(pins, "uart1_tx", app_config.pins.uart1_tx);
        cJSON_AddNumberToObject(pins, "uart1_rx", app_config.pins.uart1_rx);
        cJSON_AddNumberToObject(pins, "sdspi_cs", app_config.pins.sdspi_cs);
        cJSON_AddNumberToObject(pins, "sdspi_mosi", app_config.pins.sdspi_mosi);
        cJSON_AddNumberToObject(pins, "sdspi_miso", app_config.pins.sdspi_miso);
        cJSON_AddNumberToObject(pins, "sdspi_sclk", app_config.pins.sdspi_sclk);
        cJSON_AddNumberToObject(pins, "i2c_sda", app_config.pins.i2c_sda);
        cJSON_AddNumberToObject(pins, "i2c_scl", app_config.pins.i2c_scl);
        cJSON_AddNumberToObject(pins, "sensor_1", app_config.pins.sensor_1);
        cJSON_AddNumberToObject(pins, "sensor_2", app_config.pins.sensor_2);
        cJSON_AddNumberToObject(pins, "led_red", app_config.pins.led_red);
        cJSON_AddNumberToObject(pins, "led_green", app_config.pins.led_green);
        cJSON_AddItemToObject(root, "pins", pins);
    }

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void set_config(app_config_t* config)
{
    if (!config) {
        return;
    }

    // Duplicate the caller's strings before releasing the current ones so a
    // caller passing get_config() (self-assignment) stays safe, then adopt the
    // copies. This also fixes the previous leak of wifi_ssid/wifi_password/
    // server_url, which were overwritten without being freed.
    char* ssid = config->wifi_ssid ? strdup(config->wifi_ssid) : NULL;
    char* password = config->wifi_password ? strdup(config->wifi_password) : NULL;
    char* url = config->server_url ? strdup(config->server_url) : NULL;
    char* mqtt_url = config->mqtt_broker_url ? strdup(config->mqtt_broker_url) : NULL;
    char* mqtt_topic = config->mqtt_topic_prefix ? strdup(config->mqtt_topic_prefix) : NULL;

    free(app_config.wifi_ssid);
    free(app_config.wifi_password);
    free(app_config.server_url);
    free(app_config.mqtt_broker_url);
    free(app_config.mqtt_topic_prefix);

    app_config = *config;
    app_config.wifi_ssid = ssid;
    app_config.wifi_password = password;
    app_config.server_url = url;
    app_config.mqtt_broker_url = mqtt_url;
    app_config.mqtt_topic_prefix = mqtt_topic;

    save_config();
}

bool config_validate_and_set(app_config_t* candidate, char* err_buf, size_t err_buf_len)
{
    if (!candidate) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "PIN_INVALID: null configuration");
        }
        return false;
    }

    if (!validate_pin_config(&candidate->pins, err_buf, err_buf_len)) {
        return false;
    }

    // Ports are uint16_t, so only 0 is out of the 1-65535 range.
    if (candidate->telnet0_port == 0) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "PORT_INVALID: telnet0_port must be 1-65535");
        }
        return false;
    }
    if (candidate->telnet1_port == 0) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "PORT_INVALID: telnet1_port must be 1-65535");
        }
        return false;
    }

    // Two bridges on the same port cannot both bind.
    if (candidate->telnet0_enable && candidate->telnet1_enable &&
        candidate->telnet0_port == candidate->telnet1_port) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len,
                     "PORT_CONFLICT: telnet ports must differ when both bridges enabled");
        }
        return false;
    }

    // Enabling a bridge on a disabled UART would silently never start it.
    if (candidate->telnet0_enable && !candidate->uart0_enable) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "UART_INVALID: telnet0_enable requires uart0_enable");
        }
        return false;
    }
    if (candidate->telnet1_enable && !candidate->uart1_enable) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "UART_INVALID: telnet1_enable requires uart1_enable");
        }
        return false;
    }

    // MQTT requires a non-empty broker URL (<=255 chars) when enabled.
    if (candidate->mqtt_enable) {
        if (!candidate->mqtt_broker_url || candidate->mqtt_broker_url[0] == '\0') {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len,
                         "MQTT_INVALID: broker URL required when MQTT is enabled");
            }
            return false;
        }
        if (strlen(candidate->mqtt_broker_url) > 255) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len,
                         "MQTT_INVALID: broker URL must be <=255 characters");
            }
            return false;
        }
    }

    // INA219/INA226 addresses live in the 7-bit range 0x40-0x4F.
    if (candidate->ina219_addr < 0x40 || candidate->ina219_addr > 0x4F) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "ADDR_INVALID: ina219_addr must be 0x40-0x4F");
        }
        return false;
    }
    if (candidate->ina226_addr < 0x40 || candidate->ina226_addr > 0x4F) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "ADDR_INVALID: ina226_addr must be 0x40-0x4F");
        }
        return false;
    }

    // The sensor data model exposes a single set of bus/current/power fields,
    // so only one current/power monitor can be enabled at a time.
    if (candidate->ina219_enable && candidate->ina226_enable) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len,
                     "I2C_CONFLICT: enable only one of INA219 or INA226");
        }
        return false;
    }

    // SHT21 is fixed at 0x40; an enabled INA device must not collide with it.
    bool sht21_conflict = candidate->sht21_enable &&
                          ((candidate->ina219_enable && candidate->ina219_addr == 0x40) ||
                           (candidate->ina226_enable && candidate->ina226_addr == 0x40));
    if (sht21_conflict) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len,
                     "I2C_CONFLICT: SHT21 and INA device both use address 0x40");
        }
        return false;
    }

    set_config(candidate);
    return true;
}

void save_config(void)
{
    if (!s_nvs_handle) {
        return;
    }

    // Store configuration values
    esp_err_t err = nvs_set_u32(s_nvs_handle, "baud_rate", app_config.baud_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save baud_rate: %s", esp_err_to_name(err));
    }

    err = nvs_set_u8(s_nvs_handle, "uart0_enable", app_config.uart0_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save uart0_enable: %s", esp_err_to_name(err));
    }

    err = nvs_set_u8(s_nvs_handle, "uart1_enable", app_config.uart1_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save uart1_enable: %s", esp_err_to_name(err));
    }

    err = nvs_set_u8(s_nvs_handle, "sd_enable", app_config.sd_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save sd_enable: %s", esp_err_to_name(err));
    }

    err = nvs_set_u8(s_nvs_handle, "sensor_enable", app_config.sensor_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save sensor_enable: %s", esp_err_to_name(err));
    }

    err = nvs_set_u8(s_nvs_handle, "led_enable", app_config.led_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save led_enable: %s", esp_err_to_name(err));
    }

    save_u8(s_nvs_handle, "telnet0_en", app_config.telnet0_enable);

    err = nvs_set_u16(s_nvs_handle, "telnet0_port", app_config.telnet0_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save telnet0_port: %s", esp_err_to_name(err));
    }

    save_u8(s_nvs_handle, "telnet1_en", app_config.telnet1_enable);

    err = nvs_set_u16(s_nvs_handle, "telnet1_port", app_config.telnet1_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save telnet1_port: %s", esp_err_to_name(err));
    }

    // Optional SD logging flags.
    save_u8(s_nvs_handle, "u0_sd_log", app_config.uart0_sd_log);
    save_u8(s_nvs_handle, "u1_sd_log", app_config.uart1_sd_log);
    save_u8(s_nvs_handle, "sht21_sd", app_config.sht21_sd_log);
    save_u8(s_nvs_handle, "ina219_sd", app_config.ina219_sd_log);
    save_u8(s_nvs_handle, "ina226_sd", app_config.ina226_sd_log);
    save_u8(s_nvs_handle, "env_sd", app_config.env_sd_log);

    // I2C bus and sensor flags/addresses
    save_u8(s_nvs_handle, "i2c_en", app_config.i2c_enable);
    save_u8(s_nvs_handle, "sht21_en", app_config.sht21_enable);
    save_u8(s_nvs_handle, "ina219_en", app_config.ina219_enable);
    save_u8(s_nvs_handle, "ina226_en", app_config.ina226_enable);
    save_u8(s_nvs_handle, "ina219_adr", app_config.ina219_addr);
    save_u8(s_nvs_handle, "ina226_adr", app_config.ina226_addr);

    // Pin assignments
    save_pin(s_nvs_handle, "pin_uart0_tx", app_config.pins.uart0_tx);
    save_pin(s_nvs_handle, "pin_uart0_rx", app_config.pins.uart0_rx);
    save_pin(s_nvs_handle, "pin_uart1_tx", app_config.pins.uart1_tx);
    save_pin(s_nvs_handle, "pin_uart1_rx", app_config.pins.uart1_rx);
    save_pin(s_nvs_handle, "pin_sdspi_cs", app_config.pins.sdspi_cs);
    save_pin(s_nvs_handle, "pin_sdspi_mosi", app_config.pins.sdspi_mosi);
    save_pin(s_nvs_handle, "pin_sdspi_miso", app_config.pins.sdspi_miso);
    save_pin(s_nvs_handle, "pin_sdspi_sclk", app_config.pins.sdspi_sclk);
    save_pin(s_nvs_handle, "pin_i2c_sda", app_config.pins.i2c_sda);
    save_pin(s_nvs_handle, "pin_i2c_scl", app_config.pins.i2c_scl);
    save_pin(s_nvs_handle, "pin_sensor_1", app_config.pins.sensor_1);
    save_pin(s_nvs_handle, "pin_sensor_2", app_config.pins.sensor_2);
    save_pin(s_nvs_handle, "pin_led_red", app_config.pins.led_red);
    save_pin(s_nvs_handle, "pin_led_green", app_config.pins.led_green);

    // Save strings - they need to be handled differently
    if (app_config.wifi_ssid) {
        err = nvs_set_str(s_nvs_handle, "wifi_ssid", app_config.wifi_ssid);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save wifi_ssid: %s", esp_err_to_name(err));
        }
    }

    if (app_config.wifi_password) {
        err = nvs_set_str(s_nvs_handle, "wifi_password", app_config.wifi_password);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save wifi_password: %s", esp_err_to_name(err));
        }
    }

    if (app_config.server_url) {
        err = nvs_set_str(s_nvs_handle, "server_url", app_config.server_url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save server_url: %s", esp_err_to_name(err));
        }
    }

    // MQTT publishing.
    save_u8(s_nvs_handle, "mqtt_en", app_config.mqtt_enable);

    if (app_config.mqtt_broker_url) {
        err = nvs_set_str(s_nvs_handle, "mqtt_url", app_config.mqtt_broker_url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save mqtt_broker_url: %s", esp_err_to_name(err));
        }
    }

    if (app_config.mqtt_topic_prefix) {
        err = nvs_set_str(s_nvs_handle, "mqtt_topic", app_config.mqtt_topic_prefix);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save mqtt_topic_prefix: %s", esp_err_to_name(err));
        }
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Configuration saved");
}

void load_config(void)
{
    if (!s_nvs_handle) {
        set_pin_defaults(&app_config.pins);
        return;
    }

    // Release any strings from a previous load so load_config() is idempotent
    // and does not leak when called more than once.
    free(app_config.wifi_ssid);         app_config.wifi_ssid = NULL;
    free(app_config.wifi_password);     app_config.wifi_password = NULL;
    free(app_config.server_url);        app_config.server_url = NULL;
    free(app_config.mqtt_broker_url);   app_config.mqtt_broker_url = NULL;
    free(app_config.mqtt_topic_prefix); app_config.mqtt_topic_prefix = NULL;

    // Load configuration values
    esp_err_t err = nvs_get_u32(s_nvs_handle, "baud_rate", &app_config.baud_rate);
    if (err != ESP_OK) {
        app_config.baud_rate = 115200; // Default value
    }

    err = nvs_get_u8(s_nvs_handle, "uart0_enable", &app_config.uart0_enable);
    if (err != ESP_OK) {
        app_config.uart0_enable = 1; // Default value
    }

    err = nvs_get_u8(s_nvs_handle, "uart1_enable", &app_config.uart1_enable);
    if (err != ESP_OK) {
        app_config.uart1_enable = 1; // Default value
    }

    err = nvs_get_u8(s_nvs_handle, "sd_enable", &app_config.sd_enable);
    if (err != ESP_OK) {
        app_config.sd_enable = 1; // Default value
    }

    err = nvs_get_u8(s_nvs_handle, "sensor_enable", &app_config.sensor_enable);
    if (err != ESP_OK) {
        app_config.sensor_enable = 1; // Default value
    }

    err = nvs_get_u8(s_nvs_handle, "led_enable", &app_config.led_enable);
    if (err != ESP_OK) {
        app_config.led_enable = 1; // Default value
    }

    // Telnet bridges: migrate the legacy single-bridge keys once, then load the
    // per-UART fields. The migration is keyed on the absence of the new
    // "telnet0_en" key together with presence of the legacy "telnet_enable" key.
    {
        uint8_t old_enable = 0;
        uint16_t old_port = 23;
        uint8_t old_uart = 1;
        esp_err_t old_err = nvs_get_u8(s_nvs_handle, "telnet_enable", &old_enable);
        if (old_err == ESP_OK) {
            if (nvs_get_u16(s_nvs_handle, "telnet_port", &old_port) != ESP_OK ||
                old_port == 0) {
                old_port = 23;
            }
            if (nvs_get_u8(s_nvs_handle, "telnet_uart", &old_uart) != ESP_OK ||
                old_uart > 1) {
                old_uart = 1;
            }
        }

        uint8_t new_enable = 0;
        bool have_new = (nvs_get_u8(s_nvs_handle, "telnet0_en", &new_enable) == ESP_OK);

        if (!have_new && old_err == ESP_OK) {
            // Re-home the legacy bridge onto the UART it was configured for.
            if (old_uart == 0) {
                app_config.telnet0_enable = old_enable;
                app_config.telnet0_port   = old_port;
                app_config.telnet1_enable = 0;
                app_config.telnet1_port   = 23;
            } else {
                app_config.telnet0_enable = 0;
                app_config.telnet0_port   = 2323;
                app_config.telnet1_enable = old_enable;
                app_config.telnet1_port   = old_port;
            }
            // Phase 1: persist the new keys, checking every write. The
            // migration sentinel is "telnet0_en" (see have_new above), so both
            // port keys must be durable before it is written; write it last.
            esp_err_t w0 = nvs_set_u16(s_nvs_handle, "telnet0_port", app_config.telnet0_port);
            esp_err_t w1 = nvs_set_u16(s_nvs_handle, "telnet1_port", app_config.telnet1_port);
            esp_err_t w2 = nvs_set_u8(s_nvs_handle, "telnet1_en", app_config.telnet1_enable);
            esp_err_t w3 = nvs_set_u8(s_nvs_handle, "telnet0_en", app_config.telnet0_enable);

            if (w0 == ESP_OK && w1 == ESP_OK && w2 == ESP_OK && w3 == ESP_OK &&
                nvs_commit(s_nvs_handle) == ESP_OK) {
                // Phase 2: erase the legacy keys only once the new keys are
                // durable, so the migration runs exactly once.
                nvs_erase_key(s_nvs_handle, "telnet_enable");
                nvs_erase_key(s_nvs_handle, "telnet_port");
                nvs_erase_key(s_nvs_handle, "telnet_uart");
                nvs_commit(s_nvs_handle);
                ESP_LOGI(TAG, "Migrated legacy telnet config to UART%d bridge", (int)old_uart);
            } else {
                ESP_LOGE(TAG, "Migration write failed; legacy keys preserved for next boot");
            }
        } else {
            app_config.telnet0_enable = new_enable;
            if (nvs_get_u16(s_nvs_handle, "telnet0_port", &app_config.telnet0_port) != ESP_OK ||
                app_config.telnet0_port == 0) {
                app_config.telnet0_port = 2323;
            }
            if (nvs_get_u8(s_nvs_handle, "telnet1_en", &app_config.telnet1_enable) != ESP_OK) {
                app_config.telnet1_enable = 0;
            }
            if (nvs_get_u16(s_nvs_handle, "telnet1_port", &app_config.telnet1_port) != ESP_OK ||
                app_config.telnet1_port == 0) {
                app_config.telnet1_port = 23;
            }
        }
    }

    // Optional SD-card logging; off by default and independent of sd_enable.
    if (nvs_get_u8(s_nvs_handle, "u0_sd_log", &app_config.uart0_sd_log) != ESP_OK) {
        app_config.uart0_sd_log = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "u1_sd_log", &app_config.uart1_sd_log) != ESP_OK) {
        app_config.uart1_sd_log = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "sht21_sd", &app_config.sht21_sd_log) != ESP_OK) {
        app_config.sht21_sd_log = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "ina219_sd", &app_config.ina219_sd_log) != ESP_OK) {
        app_config.ina219_sd_log = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "ina226_sd", &app_config.ina226_sd_log) != ESP_OK) {
        app_config.ina226_sd_log = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "env_sd", &app_config.env_sd_log) != ESP_OK) {
        app_config.env_sd_log = 0;
    }

    // MQTT publishing: off by default; strings NULL when unset.
    if (nvs_get_u8(s_nvs_handle, "mqtt_en", &app_config.mqtt_enable) != ESP_OK) {
        app_config.mqtt_enable = 0;
    }

    // I2C bus and sensors: bus off and no sensors by default; INA addresses
    // default to 0x40.
    if (nvs_get_u8(s_nvs_handle, "i2c_en", &app_config.i2c_enable) != ESP_OK) {
        app_config.i2c_enable = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "sht21_en", &app_config.sht21_enable) != ESP_OK) {
        app_config.sht21_enable = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "ina219_en", &app_config.ina219_enable) != ESP_OK) {
        app_config.ina219_enable = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "ina226_en", &app_config.ina226_enable) != ESP_OK) {
        app_config.ina226_enable = 0;
    }
    if (nvs_get_u8(s_nvs_handle, "ina219_adr", &app_config.ina219_addr) != ESP_OK) {
        app_config.ina219_addr = 0x40;
    }
    if (nvs_get_u8(s_nvs_handle, "ina226_adr", &app_config.ina226_addr) != ESP_OK) {
        app_config.ina226_addr = 0x40;
    }

    // Load pin assignments with factory defaults as fallback
    app_config.pins.uart0_tx   = load_pin(s_nvs_handle, "pin_uart0_tx", PIN_DEFAULT_UART0_TX);
    app_config.pins.uart0_rx   = load_pin(s_nvs_handle, "pin_uart0_rx", PIN_DEFAULT_UART0_RX);
    app_config.pins.uart1_tx   = load_pin(s_nvs_handle, "pin_uart1_tx", PIN_DEFAULT_UART1_TX);
    app_config.pins.uart1_rx   = load_pin(s_nvs_handle, "pin_uart1_rx", PIN_DEFAULT_UART1_RX);
    app_config.pins.sdspi_cs   = load_pin(s_nvs_handle, "pin_sdspi_cs", PIN_DEFAULT_SDSPI_CS);
    app_config.pins.sdspi_mosi = load_pin(s_nvs_handle, "pin_sdspi_mosi", PIN_DEFAULT_SDSPI_MOSI);
    app_config.pins.sdspi_miso = load_pin(s_nvs_handle, "pin_sdspi_miso", PIN_DEFAULT_SDSPI_MISO);
    app_config.pins.sdspi_sclk = load_pin(s_nvs_handle, "pin_sdspi_sclk", PIN_DEFAULT_SDSPI_SCLK);
    app_config.pins.i2c_sda    = load_pin(s_nvs_handle, "pin_i2c_sda", PIN_DEFAULT_I2C_SDA);
    app_config.pins.i2c_scl    = load_pin(s_nvs_handle, "pin_i2c_scl", PIN_DEFAULT_I2C_SCL);
    app_config.pins.sensor_1   = load_pin(s_nvs_handle, "pin_sensor_1", PIN_DEFAULT_SENSOR_1);
    app_config.pins.sensor_2   = load_pin(s_nvs_handle, "pin_sensor_2", PIN_DEFAULT_SENSOR_2);
    app_config.pins.led_red    = load_pin(s_nvs_handle, "pin_led_red", PIN_DEFAULT_LED_RED);
    app_config.pins.led_green  = load_pin(s_nvs_handle, "pin_led_green", PIN_DEFAULT_LED_GREEN);

    // Load strings
    size_t ssid_len = 0;
    err = nvs_get_str(s_nvs_handle, "wifi_ssid", NULL, &ssid_len);
    if (err == ESP_OK && ssid_len > 0) {
        app_config.wifi_ssid = malloc(ssid_len);
        if (app_config.wifi_ssid) {
            nvs_get_str(s_nvs_handle, "wifi_ssid", app_config.wifi_ssid, &ssid_len);
        }
    } else {
        app_config.wifi_ssid = NULL;
    }

    size_t password_len = 0;
    err = nvs_get_str(s_nvs_handle, "wifi_password", NULL, &password_len);
    if (err == ESP_OK && password_len > 0) {
        app_config.wifi_password = malloc(password_len);
        if (app_config.wifi_password) {
            nvs_get_str(s_nvs_handle, "wifi_password", app_config.wifi_password, &password_len);
        }
    } else {
        app_config.wifi_password = NULL;
    }

    size_t url_len = 0;
    err = nvs_get_str(s_nvs_handle, "server_url", NULL, &url_len);
    if (err == ESP_OK && url_len > 0) {
        app_config.server_url = malloc(url_len);
        if (app_config.server_url) {
            nvs_get_str(s_nvs_handle, "server_url", app_config.server_url, &url_len);
        }
    } else {
        app_config.server_url = NULL;
    }

    size_t mqtt_url_len = 0;
    err = nvs_get_str(s_nvs_handle, "mqtt_url", NULL, &mqtt_url_len);
    if (err == ESP_OK && mqtt_url_len > 0) {
        app_config.mqtt_broker_url = malloc(mqtt_url_len);
        if (app_config.mqtt_broker_url) {
            nvs_get_str(s_nvs_handle, "mqtt_url", app_config.mqtt_broker_url, &mqtt_url_len);
        }
    } else {
        app_config.mqtt_broker_url = NULL;
    }

    size_t mqtt_topic_len = 0;
    err = nvs_get_str(s_nvs_handle, "mqtt_topic", NULL, &mqtt_topic_len);
    if (err == ESP_OK && mqtt_topic_len > 0) {
        app_config.mqtt_topic_prefix = malloc(mqtt_topic_len);
        if (app_config.mqtt_topic_prefix) {
            nvs_get_str(s_nvs_handle, "mqtt_topic", app_config.mqtt_topic_prefix, &mqtt_topic_len);
        }
    } else {
        app_config.mqtt_topic_prefix = NULL;
    }

    config_loaded = true;
    ESP_LOGI(TAG, "Configuration loaded");
}