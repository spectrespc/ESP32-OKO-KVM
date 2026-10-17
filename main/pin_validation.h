// main/pin_validation.h
#ifndef PIN_VALIDATION_H
#define PIN_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// Highest GPIO number modelled by the ESP32-C6 capability table.
#define PIN_MAX_GPIO 22

// Per-pin electrical capabilities.
typedef struct {
    gpio_num_t pin;
    bool input;        // usable as a digital input
    bool output;       // usable as a digital output
    bool open_drain;   // supports open-drain (I2C) operation
    bool usb_reserved; // native USB D-/D+ on the ESP32-C6
    bool strapping;    // boot strapping pin (warning only)
    bool reserved;     // permanently occupied by on-board hardware (e.g. WS2812)
} pin_info_t;

// Returns the capability descriptor for a GPIO, or NULL when out of range.
const pin_info_t* get_pin_info(gpio_num_t pin);

// Validates a full pin_config_t: range, per-role direction capability,
// duplicate assignments, I2C SDA != SCL and USB-console conflicts. Returns
// true when the assignment is usable, otherwise false with a reason written
// to err_buf (e.g. "PIN_DUPLICATE: ...").
//
// The external-device roles (ext_pwr / ext_rst) are checked only when the
// matching subsystem is enabled, so a pre-existing custom pin map that happens
// to occupy the default ext GPIOs stays valid while the subsystem is off.
bool validate_pin_config(const pin_config_t* pins, bool ext_pwr_enable, bool ext_rst_enable,
                         char* err_buf, size_t err_buf_len);

#endif // PIN_VALIDATION_H
