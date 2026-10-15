#ifndef PINS_H
#define PINS_H

#include "driver/gpio.h"

// Factory-default GPIO assignments. These are only the defaults; the active
// values live in app_config_t.pins and can be overridden via POST /config.
#define PIN_DEFAULT_UART0_TX    GPIO_NUM_16
#define PIN_DEFAULT_UART0_RX    GPIO_NUM_17
#define PIN_DEFAULT_UART1_TX    GPIO_NUM_4
#define PIN_DEFAULT_UART1_RX    GPIO_NUM_5
#define PIN_DEFAULT_SDSPI_CS    GPIO_NUM_10
#define PIN_DEFAULT_SDSPI_MOSI  GPIO_NUM_11
#define PIN_DEFAULT_SDSPI_MISO  GPIO_NUM_18
#define PIN_DEFAULT_SDSPI_SCLK  GPIO_NUM_19
#define PIN_DEFAULT_I2C_SDA     GPIO_NUM_6
#define PIN_DEFAULT_I2C_SCL     GPIO_NUM_7
#define PIN_DEFAULT_SENSOR_1    GPIO_NUM_8
#define PIN_DEFAULT_SENSOR_2    GPIO_NUM_9
#define PIN_DEFAULT_LED_RED     GPIO_NUM_14
#define PIN_DEFAULT_LED_GREEN   GPIO_NUM_15

// Pin initialization (config-driven; skips disabled subsystems).
void init_pins(void);

#endif // PINS_H