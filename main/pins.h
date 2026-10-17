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
// GPIO8 now carries the fixed WS2812 status LED, so the sensor_1 role defaults
// to unassigned. Existing devices whose NVS still maps sensor_1 to GPIO8 are
// migrated to GPIO_NUM_NC in load_config().
#define PIN_DEFAULT_SENSOR_1    GPIO_NUM_NC
#define PIN_DEFAULT_SENSOR_2    GPIO_NUM_9

// Fixed WS2812 status LED data line. Not configurable: reserved in
// pin_validation so no role may claim it.
#define PIN_WS2812              GPIO_NUM_8
#define PIN_DEFAULT_LED_RED     GPIO_NUM_14
#define PIN_DEFAULT_LED_GREEN   GPIO_NUM_15
// External device power/reset. Both default to currently-free GPIOs so the
// roles stay valid even while the subsystem is disabled (pin validation checks
// every role unconditionally).
#define PIN_DEFAULT_EXT_PWR     GPIO_NUM_20
#define PIN_DEFAULT_EXT_RST     GPIO_NUM_21

// Pin initialization (config-driven; skips disabled subsystems).
void init_pins(void);

// Configures the external power/reset outputs when their subsystems are
// enabled. The power pin is forced to its inactive level before the output
// driver is enabled and then restored to the persisted ext_pwr_state; the reset
// pin is left inactive. No-op for disabled subsystems. Called from
// init_ext_control() (not init_pins()) so the pins are never configured twice.
void init_ext_pins(void);

#endif // PINS_H