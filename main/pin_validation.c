// main/pin_validation.c
#include "pin_validation.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "pin_validation";

// ESP32-C6 IO MUX capability table for the GPIOs exposed on the module
// (GPIO0-GPIO22). USB pins (12/13) are reserved when the USB Serial/JTAG
// console is active; strapping pins (2/8/9) may affect boot and only warn.
static const pin_info_t s_pin_info[PIN_MAX_GPIO + 1] = {
    [0]  = { GPIO_NUM_0,  true, true, true,  false, false },
    [1]  = { GPIO_NUM_1,  true, true, true,  false, false },
    [2]  = { GPIO_NUM_2,  true, true, true,  false, true  },
    [3]  = { GPIO_NUM_3,  true, true, true,  false, false },
    [4]  = { GPIO_NUM_4,  true, true, true,  false, false },
    [5]  = { GPIO_NUM_5,  true, true, true,  false, false },
    [6]  = { GPIO_NUM_6,  true, true, true,  false, false },
    [7]  = { GPIO_NUM_7,  true, true, true,  false, false },
    [8]  = { GPIO_NUM_8,  true, true, true,  false, true  },
    [9]  = { GPIO_NUM_9,  true, true, true,  false, true  },
    [10] = { GPIO_NUM_10, true, true, true,  false, false },
    [11] = { GPIO_NUM_11, true, true, true,  false, false },
    [12] = { GPIO_NUM_12, true, true, true,  true,  false },
    [13] = { GPIO_NUM_13, true, true, true,  true,  false },
    [14] = { GPIO_NUM_14, true, true, true,  false, false },
    [15] = { GPIO_NUM_15, true, true, true,  false, false },
    [16] = { GPIO_NUM_16, true, true, true,  false, false },
    [17] = { GPIO_NUM_17, true, true, true,  false, false },
    [18] = { GPIO_NUM_18, true, true, true,  false, false },
    [19] = { GPIO_NUM_19, true, true, true,  false, false },
    [20] = { GPIO_NUM_20, true, true, true,  false, false },
    [21] = { GPIO_NUM_21, true, true, true,  false, false },
    [22] = { GPIO_NUM_22, true, true, true,  false, false },
};

const pin_info_t* get_pin_info(gpio_num_t pin)
{
    if (pin < 0 || pin > PIN_MAX_GPIO) {
        return NULL;
    }
    return &s_pin_info[pin];
}

typedef struct {
    const char* name;
    gpio_num_t pin;
    bool need_in;
    bool need_out;
} role_req_t;

bool validate_pin_config(const pin_config_t* pins, char* err_buf, size_t err_buf_len)
{
    if (!pins) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "PIN_INVALID: null config");
        return false;
    }

    const role_req_t roles[] = {
        { "uart0_tx",   pins->uart0_tx,   false, true  },
        { "uart0_rx",   pins->uart0_rx,   true,  false },
        { "uart1_tx",   pins->uart1_tx,   false, true  },
        { "uart1_rx",   pins->uart1_rx,   true,  false },
        { "sdspi_cs",   pins->sdspi_cs,   false, true  },
        { "sdspi_mosi", pins->sdspi_mosi, false, true  },
        { "sdspi_miso", pins->sdspi_miso, true,  false },
        { "sdspi_sclk", pins->sdspi_sclk, false, true  },
        { "i2c_sda",    pins->i2c_sda,    true,  true  },
        { "i2c_scl",    pins->i2c_scl,    true,  true  },
        { "sensor_1",   pins->sensor_1,   true,  false },
        { "sensor_2",   pins->sensor_2,   true,  false },
        { "led_red",    pins->led_red,    false, true  },
        { "led_green",  pins->led_green,  false, true  },
        { "ext_pwr",    pins->ext_pwr,    false, true  },
        { "ext_rst",    pins->ext_rst,    false, true  },
    };
    const size_t n = sizeof(roles) / sizeof(roles[0]);

    for (size_t i = 0; i < n; i++) {
        const pin_info_t* info = get_pin_info(roles[i].pin);
        if (!info) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len, "PIN_INVALID: %s GPIO%d out of range (0-%d)",
                         roles[i].name, (int)roles[i].pin, PIN_MAX_GPIO);
            }
            return false;
        }

        if (roles[i].need_out && !info->output) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len, "PIN_NO_CAPABILITY: %s GPIO%d cannot drive output",
                         roles[i].name, (int)roles[i].pin);
            }
            return false;
        }
        if (roles[i].need_in && !info->input) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len, "PIN_NO_CAPABILITY: %s GPIO%d cannot read input",
                         roles[i].name, (int)roles[i].pin);
            }
            return false;
        }
        if ((strcmp(roles[i].name, "i2c_sda") == 0 || strcmp(roles[i].name, "i2c_scl") == 0) &&
            !info->open_drain) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len, "PIN_NO_CAPABILITY: %s GPIO%d has no open-drain",
                         roles[i].name, (int)roles[i].pin);
            }
            return false;
        }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        if (info->usb_reserved) {
            if (err_buf && err_buf_len) {
                snprintf(err_buf, err_buf_len,
                         "PIN_USB_CONFLICT: %s GPIO%d reserved for USB Serial/JTAG console",
                         roles[i].name, (int)roles[i].pin);
            }
            return false;
        }
#endif

        if (info->strapping) {
            ESP_LOGW(TAG, "GPIO%d (%s) is a strapping pin; boot behaviour may change",
                     (int)info->pin, roles[i].name);
        }
    }

    // I2C SDA and SCL must differ (also covered by the duplicate check below).
    if (pins->i2c_sda == pins->i2c_scl) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "PIN_I2C_CONFLICT: I2C SDA and SCL are both GPIO%d",
                     (int)pins->i2c_sda);
        }
        return false;
    }

    // Every subsystem pin must be unique.
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (roles[i].pin == roles[j].pin) {
                if (err_buf && err_buf_len) {
                    snprintf(err_buf, err_buf_len,
                             "PIN_DUPLICATE: GPIO%d assigned to both %s and %s",
                             (int)roles[i].pin, roles[i].name, roles[j].name);
                }
                return false;
            }
        }
    }

    return true;
}