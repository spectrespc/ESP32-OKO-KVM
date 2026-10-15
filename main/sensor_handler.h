// main/sensor_handler.h
#ifndef SENSOR_HANDLER_H
#define SENSOR_HANDLER_H

#include <stdint.h>
#include <stddef.h>

// All fields are fixed-point integers so the values serialize losslessly to
// JSON as plain integers (see the multiplier noted on each field).
typedef struct {
    int16_t  temperature;   // 0.1 °C   (253 = 25.3 °C)   [SHT21]
    uint16_t humidity;      // 0.1 %RH  (605 = 60.5 %)    [SHT21]
    uint16_t pressure;      // hPa      (simulated)
    uint16_t light_level;   // lux      (simulated)
    int32_t  bus_voltage;   // mV       (3300 = 3.300 V)  [INA219/INA226]
    int32_t  shunt_voltage; // µV       (1250 = 1.250 mV) [INA219/INA226]
    int32_t  current;       // mA       (150 = 150 mA)    [INA219/INA226]
    int32_t  power;         // mW       (495 = 495 mW)    [INA219/INA226]
} sensor_data_t;

typedef enum {
    SENSOR_STATUS_OK,
    SENSOR_STATUS_ERROR,
    SENSOR_STATUS_TIMEOUT,
    SENSOR_STATUS_UNKNOWN
} sensor_status_t;

void init_sensor_handler(void);
sensor_status_t read_sensor_data(sensor_data_t* data);
char* get_sensor_data_json(void);
void sensor_cleanup(void);

// Starts the MQTT client when mqtt_enable is set and a broker URL is
// configured. Call after the STA interface is up. Safe to call when MQTT is
// disabled (no-op).
void init_mqtt(void);

#endif // SENSOR_HANDLER_H