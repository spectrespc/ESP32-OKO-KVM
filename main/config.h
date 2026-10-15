// main/config.h
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "driver/gpio.h"

// Reconfigurable GPIO assignments. Every subsystem pin can be overridden via
// POST /config and is persisted to NVS. Defaults come from PIN_DEFAULT_* in
// pins.h (see config.c).
typedef struct {
    gpio_num_t uart0_tx;
    gpio_num_t uart0_rx;
    gpio_num_t uart1_tx;
    gpio_num_t uart1_rx;
    gpio_num_t sdspi_cs;
    gpio_num_t sdspi_mosi;
    gpio_num_t sdspi_miso;
    gpio_num_t sdspi_sclk;
    gpio_num_t i2c_sda;
    gpio_num_t i2c_scl;
    gpio_num_t sensor_1;
    gpio_num_t sensor_2;
    gpio_num_t led_red;
    gpio_num_t led_green;
} pin_config_t;

// Configuration structure
typedef struct {
    uint32_t baud_rate;
    uint8_t uart0_enable;
    uint8_t uart1_enable;
    uint8_t sd_enable;
    uint8_t sensor_enable;
    uint8_t led_enable;
    // Independent telnet bridges for each UART. telnet0_* relays UART0 and
    // telnet1_* relays UART1; each has its own TCP listen port.
    uint8_t  telnet0_enable;   // 0=off, 1=on (default: 0)
    uint16_t telnet0_port;     // TCP port for UART0 (default: 2323)
    uint8_t  telnet1_enable;   // 0=off, 1=on (default: 0)
    uint16_t telnet1_port;     // TCP port for UART1 (default: 23)
    // Optional SD-card logging. These are independent of sd_enable: when no card
    // is mounted the writes are silently skipped.
    uint8_t  uart0_sd_log;     // log UART0 traffic to /sdcard/uar0.log (0/1)
    uint8_t  uart1_sd_log;     // log UART1 traffic to /sdcard/uar1.log (0/1)
    // Per-sensor CSV logging; each has its own file and enable flag.
    uint8_t  sht21_sd_log;     // SHT21 temperature + humidity   -> /sdcard/sht21.csv
    uint8_t  ina219_sd_log;    // INA219 voltage + current       -> /sdcard/ina219.csv
    uint8_t  ina226_sd_log;    // INA226 voltage + current       -> /sdcard/ina226.csv
    uint8_t  env_sd_log;       // simulated pressure + light     -> /sdcard/env.csv
    // I2C bus + onboard/optional sensor population (see sensor_handler.c).
    // Addresses use the INA219/INA226 7-bit range 0x40-0x4F.
    uint8_t i2c_enable;
    uint8_t sht21_enable;
    uint8_t ina219_enable;
    uint8_t ina226_enable;
    uint8_t ina219_addr;
    uint8_t ina226_addr;
    pin_config_t pins;
    char* wifi_ssid;
    char* wifi_password;
    char* server_url;
    // MQTT publishing of sensor data (one JSON topic per sensor, QoS 0).
    uint8_t mqtt_enable;        // 0=off, 1=on (default: 0)
    char* mqtt_broker_url;      // e.g. "mqtt://192.168.1.100:1883" (NULL if unset)
    char* mqtt_topic_prefix;    // e.g. "gateway" -> topics: gateway/sht21, ...
} app_config_t;

// Configuration functions
void init_config(void);
app_config_t* get_config(void);
char* get_config_json(void);
void set_config(app_config_t* config);
void save_config(void);
void load_config(void);

// Validates the candidate pins (capability/duplicate/USB-conflict) and the
// telnet port, and on success applies + persists it to NVS. On failure it
// returns false and writes a human-readable reason to err_buf.
bool config_validate_and_set(app_config_t* candidate, char* err_buf, size_t err_buf_len);

#endif // CONFIG_H