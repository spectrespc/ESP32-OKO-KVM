// main/main.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "pins.h"
#include "uart_handler.h"
#include "http_server.h"
#include "sd_handler.h"
#include "sensor_handler.h"
#include "config.h"
#include "wifi_ap.h"
#include "wifi_scan.h"
#include "self_test.h"
#include "telnet_bridge.h"
#include "web_handlers.h"
#include "log_handler.h"
#include "ext_control.h"

static const char* TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-C6 Gateway Firmware");
    
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return;
    }

    // Capture logs into the in-RAM ring buffer as early as possible so the
    // web UI can display boot-time messages.
    init_log_handler();

    // Initialize configuration (must precede pin init so configured pins and
    // enable flags are available).
    ESP_LOGI(TAG, "Initializing configuration");
    init_config();
    app_config_t* cfg = get_config();

    // Initialize pins
    ESP_LOGI(TAG, "Initializing pins");
    init_pins();

    // Initialize external device power/reset control (configures its GPIOs and
    // restores the persisted power state; must follow init_pins()).
    ESP_LOGI(TAG, "Initializing external device control");
    init_ext_control();

    // Initialize UART (UART0 console; UART0 and UART1 available as bridge targets)
    ESP_LOGI(TAG, "Initializing UART");
    if (cfg->uart0_enable) init_uart0();
    if (cfg->uart1_enable) init_uart1();

    // Initialize sensors (I2C sensors are set up here too, so run this when
    // either the sensor subsystem or the I2C bus is enabled, or any SD sensor
    // log / MQTT publishing is requested).
    ESP_LOGI(TAG, "Initializing sensors");
    if (cfg->sensor_enable || cfg->i2c_enable || cfg->sht21_sd_log ||
        cfg->ina219_sd_log || cfg->ina226_sd_log || cfg->env_sd_log ||
        cfg->mqtt_enable) {
        init_sensor_handler();
    }
    
    // Ensure I2C driver is properly initialized before sensor operations
    if (cfg->i2c_enable) {
        ESP_LOGI(TAG, "I2C bus initialized for sensor operations");
    }

    // Initialize SD card
    ESP_LOGI(TAG, "Initializing SD card");
    if (cfg->sd_enable) init_sd_handler();

    // Initialize WiFi AP (core infrastructure)
    ESP_LOGI(TAG, "Initializing WiFi AP");
    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi AP: %s", esp_err_to_name(err));
    }

    // Bring up the STA side (reconnects to a previously saved network).
    init_wifi_sta();

    // Start MQTT publishing once the network stack is available.
    init_mqtt();

    // Start the telnet bridges when enabled and their UART is available.
    if (cfg->telnet0_enable) {
        if (cfg->uart0_enable) {
            ESP_LOGI(TAG, "Starting telnet bridge UART0 (port %u)", (unsigned)cfg->telnet0_port);
            init_telnet_bridge0();
        } else {
            ESP_LOGW(TAG, "telnet0_enable set but uart0_enable=0; UART0 bridge not started");
        }
    }
    if (cfg->telnet1_enable) {
        if (cfg->uart1_enable) {
            ESP_LOGI(TAG, "Starting telnet bridge UART1 (port %u)", (unsigned)cfg->telnet1_port);
            init_telnet_bridge1();
        } else {
            ESP_LOGW(TAG, "telnet1_enable set but uart1_enable=0; UART1 bridge not started");
        }
    }

    // Mount the SPIFFS web assets before starting the HTTP server.
    ESP_LOGI(TAG, "Mounting SPIFFS web assets");
    init_spiffs();

    // Initialize HTTP server (core infrastructure)
    ESP_LOGI(TAG, "Starting HTTP server");
    init_http_server();

    ESP_LOGI(TAG, "All components initialized successfully");

    // Run the HTTP endpoint self-test over the internal AP (results on console)
    start_self_test();

    // Main loop - just keep the task alive
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}