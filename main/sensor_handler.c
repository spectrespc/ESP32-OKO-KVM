// main/sensor_handler.c
#include "sensor_handler.h"
#include "config.h"
#include "sd_handler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "driver/i2c_master.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdatomic.h>

static const char* TAG = "sensor_handler";

// --- I2C bus + device handles ------------------------------------------------
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_sht21 = NULL;
static i2c_master_dev_handle_t s_ina219 = NULL;
static i2c_master_dev_handle_t s_ina226 = NULL;
static bool sensors_initialized = false;
static bool s_log_task_started = false;

// MQTT client handle (NULL until init_mqtt() starts it successfully). Accessed
// atomically: the sensor task reads it while init_mqtt() may publish it.
static _Atomic(esp_mqtt_client_handle_t) s_mqtt = NULL;

#define SENSOR_LOG_INTERVAL_MS 10000

#define I2C_SPEED_HZ   100000
#define I2C_XFER_MS    100

// SHT21 fixed 7-bit address.
#define SHT21_ADDR     0x40
#define SHT21_CMD_TEMP_HOLD 0xF3  // measure temperature, hold master
#define SHT21_CMD_RH_HOLD   0xF5  // measure relative humidity, hold master
#define SHT21_CMD_SOFT_RESET 0xFE

// Register map shared by INA219 and INA226.
#define INA_REG_CONFIG  0x00
#define INA_REG_SHUNTV  0x01
#define INA_REG_BUSV    0x02
#define INA_REG_POWER   0x03
#define INA_REG_CURRENT 0x04
#define INA_REG_CAL     0x05

// INA219: 32 V bus range, ±320 mV shunt, 12-bit ADC, continuous.
#define INA219_CONFIG   0x399F
// Calibration 4096 -> Current_LSB = 0.04096 / (4096 * 0.1) = 100 µA.
#define INA219_CAL      0x1000

// INA226: 4 averages, 1.1 ms conversions, continuous shunt+bus.
#define INA226_CONFIG   0x0527
// Calibration 2560 -> Current_LSB = 0.00512 / (2560 * 0.1) = 20 µA.
#define INA226_CAL      0x0A00

// --- Low-level I2C helpers ---------------------------------------------------
static i2c_master_dev_handle_t add_i2c_device(uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &dev) != ESP_OK) {
        return NULL;
    }
    return dev;
}

static bool i2c_write_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_XFER_MS) == ESP_OK;
}

static bool i2c_read_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t* out)
{
    uint8_t buf[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, buf, sizeof(buf), I2C_XFER_MS) != ESP_OK) {
        return false;
    }
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

// --- SHT21 (temperature + humidity) ------------------------------------------
// Hold-master measurement: the sensor stretches SCL until the conversion is
// done, so a generous transfer timeout covers the ~85 ms worst case.
static bool sht21_measure(i2c_master_dev_handle_t dev, uint8_t cmd, uint8_t out[3])
{
    return i2c_master_transmit_receive(dev, &cmd, 1, out, 3, 100) == ESP_OK;
}

static bool sht21_read(i2c_master_dev_handle_t dev, int16_t* temp10, uint16_t* rh10)
{
    uint8_t b[3];

    if (!sht21_measure(dev, SHT21_CMD_TEMP_HOLD, b)) {
        return false;
    }
    uint16_t traw = ((uint16_t)b[0] << 8) | (b[1] & 0xFC);
    // T(0.1 °C) = -468 + 1757 * raw / 65536
    *temp10 = (int16_t)(-468 + (int32_t)((1757LL * traw) / 65536));

    if (!sht21_measure(dev, SHT21_CMD_RH_HOLD, b)) {
        return false;
    }
    uint16_t rraw = ((uint16_t)b[0] << 8) | (b[1] & 0xFC);
    // RH(0.1 %) = -60 + 1250 * raw / 65536, clamped to [0, 1000]
    int32_t rh = -60 + (int32_t)((1250LL * rraw) / 65536);
    if (rh < 0) rh = 0;
    if (rh > 1000) rh = 1000;
    *rh10 = (uint16_t)rh;
    return true;
}

// --- INA219 / INA226 current + power monitors --------------------------------
static bool ina219_read(i2c_master_dev_handle_t dev, sensor_data_t* d)
{
    uint16_t bus, shunt, cur, pwr;
    if (!i2c_read_reg16(dev, INA_REG_BUSV, &bus) ||
        !i2c_read_reg16(dev, INA_REG_SHUNTV, &shunt) ||
        !i2c_read_reg16(dev, INA_REG_CURRENT, &cur) ||
        !i2c_read_reg16(dev, INA_REG_POWER, &pwr)) {
        return false;
    }

    d->bus_voltage   = (int32_t)(bus >> 3) * 4;          // 4 mV LSB
    d->shunt_voltage = (int32_t)(int16_t)shunt * 10;     // 10 µV LSB
    d->current       = (int32_t)(int16_t)cur / 10;       // 100 µA LSB -> 0.1 mA units
    d->power         = (int32_t)pwr * 2;                 // 20 * 100 µA = 2 mW LSB
    return true;
}

static bool ina226_read(i2c_master_dev_handle_t dev, sensor_data_t* d)
{
    uint16_t bus, shunt, cur, pwr;
    if (!i2c_read_reg16(dev, INA_REG_BUSV, &bus) ||
        !i2c_read_reg16(dev, INA_REG_SHUNTV, &shunt) ||
        !i2c_read_reg16(dev, INA_REG_CURRENT, &cur) ||
        !i2c_read_reg16(dev, INA_REG_POWER, &pwr)) {
        return false;
    }

    d->bus_voltage   = (int32_t)bus * 1250 / 1000;             // 1.25 mV LSB
    d->shunt_voltage = (int32_t)(int16_t)shunt * 5 / 2;        // 2.5 µV LSB
    d->current       = (int32_t)(int16_t)cur * 20 / 1000;      // 20 µA LSB -> mA
    d->power         = (int32_t)pwr / 2;                       // 25 * 20 µA = 500 µW LSB
    return true;
}

// --- Periodic sensor logging to SD + MQTT publishing -------------------------
// Runs for the lifetime of the device (started once by init_sensor_handler).
// Every SENSOR_LOG_INTERVAL_MS it reads the sensors and, while enabled, writes
// one CSV row per configured sensor file and publishes every MQTT topic. The
// enable flags are re-read each iteration, so changes apply without a reboot.
// MQTT publishing is independent of the per-sensor SD-log flags.

// Write a CSV header once per boot for the given filename.
static void ensure_csv_header(const char* filename, const char* header)
{
    static bool hdr_sht21 = false, hdr_ina219 = false;
    static bool hdr_ina226 = false, hdr_env = false;
    bool* flag = NULL;
    if (strcmp(filename, "sht21.csv") == 0)   flag = &hdr_sht21;
    if (strcmp(filename, "ina219.csv") == 0)  flag = &hdr_ina219;
    if (strcmp(filename, "ina226.csv") == 0)  flag = &hdr_ina226;
    if (strcmp(filename, "env.csv") == 0)     flag = &hdr_env;
    if (flag && !*flag) {
        if (sd_write_rotate(filename, (const uint8_t*)header, strlen(header)) == 0) {
            *flag = true;
        }
    }
}

// Publish one MQTT payload and log failures, throttled so a sustained broker
// outage (the task publishes 4 topics every 10 s) cannot flood the log ring.
static void mqtt_publish(esp_mqtt_client_handle_t mqtt, const char* topic, const char* json)
{
    static uint32_t fail_count = 0;
    int msg_id = esp_mqtt_client_publish(mqtt, topic, json, 0, 0, 0);
    if (msg_id < 0) {
        if ((fail_count++ % 60) == 0) {
            ESP_LOGW(TAG, "MQTT publish to %s failed: msg_id=%d", topic, msg_id);
        }
    }
}

static void sensor_log_task(void* arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_LOG_INTERVAL_MS));

        const app_config_t* cfg = get_config();
        sensor_data_t data;
        if (read_sensor_data(&data) != SENSOR_STATUS_OK) {
            continue;
        }

        int64_t now = esp_timer_get_time() / 1000LL;
        char line[128];

        // SHT21: temperature + humidity.
        if (cfg->sht21_sd_log) {
            static const char hdr[] = "ts_ms,temp_dC,hum_dRH\n";
            ensure_csv_header("sht21.csv", hdr);
            int n = snprintf(line, sizeof(line), "%lld,%d,%u\n",
                             (long long)now, (int)data.temperature,
                             (unsigned)data.humidity);
            if (n > 0 && n < (int)sizeof(line)) {
                sd_write_rotate("sht21.csv", (const uint8_t*)line, (size_t)n);
            }
        }

        // INA219: voltage + current + power.
        if (cfg->ina219_sd_log) {
            static const char hdr[] = "ts_ms,bus_mv,shunt_uv,cur_ma,pwr_mw\n";
            ensure_csv_header("ina219.csv", hdr);
            int n = snprintf(line, sizeof(line),
                             "%lld,%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 "\n",
                             (long long)now, data.bus_voltage, data.shunt_voltage,
                             data.current, data.power);
            if (n > 0 && n < (int)sizeof(line)) {
                sd_write_rotate("ina219.csv", (const uint8_t*)line, (size_t)n);
            }
        }

        // INA226 (mutually exclusive with INA219 per config validation).
        if (cfg->ina226_sd_log) {
            static const char hdr[] = "ts_ms,bus_mv,shunt_uv,cur_ma,pwr_mw\n";
            ensure_csv_header("ina226.csv", hdr);
            int n = snprintf(line, sizeof(line),
                             "%lld,%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 "\n",
                             (long long)now, data.bus_voltage, data.shunt_voltage,
                             data.current, data.power);
            if (n > 0 && n < (int)sizeof(line)) {
                sd_write_rotate("ina226.csv", (const uint8_t*)line, (size_t)n);
            }
        }

        // Environment: pressure + light (simulated, always available).
        if (cfg->env_sd_log) {
            static const char hdr[] = "ts_ms,press_hPa,light_lux\n";
            ensure_csv_header("env.csv", hdr);
            int n = snprintf(line, sizeof(line), "%lld,%u,%u\n",
                             (long long)now, (unsigned)data.pressure,
                             (unsigned)data.light_level);
            if (n > 0 && n < (int)sizeof(line)) {
                sd_write_rotate("env.csv", (const uint8_t*)line, (size_t)n);
            }
        }

        // MQTT: one JSON topic per sensor while enabled.
        esp_mqtt_client_handle_t mqtt = atomic_load_explicit(&s_mqtt, memory_order_acquire);
        if (cfg->mqtt_enable && mqtt) {
            const char* prefix = (cfg->mqtt_topic_prefix && cfg->mqtt_topic_prefix[0])
                                 ? cfg->mqtt_topic_prefix : "gateway";
            char topic[96];
            char json[160];

            snprintf(topic, sizeof(topic), "%s/sht21", prefix);
            int n = snprintf(json, sizeof(json),
                             "{\"ts\":%lld,\"temp_dC\":%d,\"hum_dRH\":%u}",
                             (long long)now, (int)data.temperature,
                             (unsigned)data.humidity);
            if (n > 0 && n < (int)sizeof(json)) {
                mqtt_publish(mqtt, topic, json);
            }

            snprintf(topic, sizeof(topic), "%s/ina219", prefix);
            n = snprintf(json, sizeof(json),
                         "{\"ts\":%lld,\"bus_mv\":%" PRId32 ",\"shunt_uv\":%" PRId32
                         ",\"cur_ma\":%" PRId32 ",\"pwr_mw\":%" PRId32 "}",
                         (long long)now, data.bus_voltage, data.shunt_voltage,
                         data.current, data.power);
            if (n > 0 && n < (int)sizeof(json)) {
                mqtt_publish(mqtt, topic, json);
            }

            snprintf(topic, sizeof(topic), "%s/ina226", prefix);
            n = snprintf(json, sizeof(json),
                         "{\"ts\":%lld,\"bus_mv\":%" PRId32 ",\"shunt_uv\":%" PRId32
                         ",\"cur_ma\":%" PRId32 ",\"pwr_mw\":%" PRId32 "}",
                         (long long)now, data.bus_voltage, data.shunt_voltage,
                         data.current, data.power);
            if (n > 0 && n < (int)sizeof(json)) {
                mqtt_publish(mqtt, topic, json);
            }

            snprintf(topic, sizeof(topic), "%s/env", prefix);
            n = snprintf(json, sizeof(json),
                         "{\"ts\":%lld,\"press_hPa\":%u,\"light_lux\":%u}",
                         (long long)now, (unsigned)data.pressure,
                         (unsigned)data.light_level);
            if (n > 0 && n < (int)sizeof(json)) {
                mqtt_publish(mqtt, topic, json);
            }
        }
    }
}

void init_mqtt(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg->mqtt_enable || !cfg->mqtt_broker_url || cfg->mqtt_broker_url[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled or no broker URL");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt_broker_url,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        // Destroy before publishing the handle so the sensor task never sees a
        // partially started (and then freed) client.
        esp_mqtt_client_destroy(client);
        return;
    }
    atomic_store_explicit(&s_mqtt, client, memory_order_release);
    ESP_LOGI(TAG, "MQTT client started: %s", cfg->mqtt_broker_url);
}

// --- Public API --------------------------------------------------------------
void init_sensor_handler(void)
{
    if (sensors_initialized) {
        ESP_LOGW(TAG, "Sensor handler already initialized");
        return;
    }

    // Start the sensor task regardless of I2C state: readings fall back to
    // simulated values, so the per-sensor CSV logs and MQTT topics still
    // populate without hardware.
    if (!s_log_task_started) {
        s_log_task_started = true;
        if (xTaskCreate(sensor_log_task, "sensor_log", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start sensor log task");
            s_log_task_started = false;
        }
    }

    const app_config_t* cfg = get_config();

    if (!cfg->i2c_enable) {
        ESP_LOGW(TAG, "I2C bus disabled; serving simulated readings only");
        sensors_initialized = true;
        return;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->pins.i2c_sda,
        .scl_io_num = cfg->pins.i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C master bus (sda=%d scl=%d)",
                 (int)cfg->pins.i2c_sda, (int)cfg->pins.i2c_scl);
        s_bus = NULL;
        sensors_initialized = true;
        return;
    }

    ESP_LOGI(TAG, "I2C bus up (sda=%d scl=%d, 100 kHz)",
             (int)cfg->pins.i2c_sda, (int)cfg->pins.i2c_scl);

    if (cfg->sht21_enable) {
        if (i2c_master_probe(s_bus, SHT21_ADDR, I2C_XFER_MS) == ESP_OK) {
            s_sht21 = add_i2c_device(SHT21_ADDR);
            if (s_sht21) {
                uint8_t reset = SHT21_CMD_SOFT_RESET;
                i2c_master_transmit(s_sht21, &reset, 1, I2C_XFER_MS);
                vTaskDelay(pdMS_TO_TICKS(20));
                ESP_LOGI(TAG, "SHT21 detected at 0x%02X", SHT21_ADDR);
            }
        } else {
            ESP_LOGW(TAG, "sht21_enable set but no device at 0x%02X", SHT21_ADDR);
        }
    }

    if (cfg->ina219_enable) {
        if (i2c_master_probe(s_bus, cfg->ina219_addr, I2C_XFER_MS) == ESP_OK) {
            s_ina219 = add_i2c_device(cfg->ina219_addr);
            if (s_ina219) {
                i2c_write_reg16(s_ina219, INA_REG_CONFIG, INA219_CONFIG);
                i2c_write_reg16(s_ina219, INA_REG_CAL, INA219_CAL);
                ESP_LOGI(TAG, "INA219 detected at 0x%02X", cfg->ina219_addr);
            }
        } else {
            ESP_LOGW(TAG, "ina219_enable set but no device at 0x%02X", cfg->ina219_addr);
        }
    }

    if (cfg->ina226_enable) {
        if (i2c_master_probe(s_bus, cfg->ina226_addr, I2C_XFER_MS) == ESP_OK) {
            s_ina226 = add_i2c_device(cfg->ina226_addr);
            if (s_ina226) {
                i2c_write_reg16(s_ina226, INA_REG_CONFIG, INA226_CONFIG);
                i2c_write_reg16(s_ina226, INA_REG_CAL, INA226_CAL);
                ESP_LOGI(TAG, "INA226 detected at 0x%02X", cfg->ina226_addr);
            }
        } else {
            ESP_LOGW(TAG, "ina226_enable set but no device at 0x%02X", cfg->ina226_addr);
        }
    }

    sensors_initialized = true;
}

sensor_status_t read_sensor_data(sensor_data_t* data)
{
    if (!sensors_initialized || !data) {
        return SENSOR_STATUS_ERROR;
    }

    memset(data, 0, sizeof(*data));

    // Simulated fallbacks, used when the matching sensor is absent or the I2C
    // bus is disabled.
    data->temperature = 253;   // 25.3 °C
    data->humidity    = 605;   // 60.5 %
    data->pressure    = 1013;  // hPa
    data->light_level = 500;   // lux

    if (s_sht21) {
        int16_t t10 = 0;
        uint16_t rh10 = 0;
        if (sht21_read(s_sht21, &t10, &rh10)) {
            data->temperature = t10;
            data->humidity = rh10;
        } else {
            ESP_LOGW(TAG, "SHT21 read failed; using simulated values");
        }
    }

    if (s_ina219) {
        if (!ina219_read(s_ina219, data)) {
            ESP_LOGW(TAG, "INA219 read failed");
        }
    } else if (s_ina226) {
        if (!ina226_read(s_ina226, data)) {
            ESP_LOGW(TAG, "INA226 read failed");
        }
    }

    return SENSOR_STATUS_OK;
}

char* get_sensor_data_json(void)
{
    sensor_data_t data;
    if (!sensors_initialized) {
        // Sensor subsystem disabled: return the same simulated fallbacks that
        // read_sensor_data() applies, so GET /sensor stays 200 instead of 500.
        memset(&data, 0, sizeof(data));
        data.temperature = 253;   // 25.3 °C
        data.humidity    = 605;   // 60.5 %
        data.pressure    = 1013;  // hPa
        data.light_level = 500;   // lux
    } else if (read_sensor_data(&data) != SENSOR_STATUS_OK) {
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "temperature", data.temperature);
    cJSON_AddNumberToObject(root, "humidity", data.humidity);
    cJSON_AddNumberToObject(root, "pressure", data.pressure);
    cJSON_AddNumberToObject(root, "light_level", data.light_level);
    cJSON_AddNumberToObject(root, "bus_voltage_mv", data.bus_voltage);
    cJSON_AddNumberToObject(root, "shunt_voltage_uv", data.shunt_voltage);
    cJSON_AddNumberToObject(root, "current_ma", data.current);
    cJSON_AddNumberToObject(root, "power_mw", data.power);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void sensor_cleanup(void)
{
    if (sensors_initialized) {
        sensors_initialized = false;
        ESP_LOGI(TAG, "Sensors cleaned up");
    }
}