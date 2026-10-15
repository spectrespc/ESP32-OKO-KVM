// main/pins.c
#include "pins.h"
#include "config.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "pins";

static void pin_out(gpio_num_t pin, int level)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void pin_in(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

static void pin_od(gpio_num_t pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(pin, 1);
}

// GPIO12/GPIO13 are the ESP32-C6 native USB D-/D+ lines. When the USB
// Serial/JTAG console is enabled they must not be driven as GPIO, otherwise
// the USB device detaches and the console/monitor is lost.
static bool pin_skip_usb(gpio_num_t pin)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (pin == GPIO_NUM_12 || pin == GPIO_NUM_13) {
        ESP_LOGW(TAG, "Skipping GPIO%d: USB Serial/JTAG console in use", (int)pin);
        return true;
    }
#else
    (void)pin;
#endif
    return false;
}

void init_pins(void)
{
    const app_config_t* cfg = get_config();
    const pin_config_t* p = &cfg->pins;

    // Configure UART0 pins (console)
    if (cfg->uart0_enable) {
        pin_out(p->uart0_tx, 0);
        pin_in(p->uart0_rx);
    } else {
        ESP_LOGW(TAG, "UART0 disabled; skipping pin init");
    }

    // Configure UART1 pins
    if (cfg->uart1_enable) {
        pin_out(p->uart1_tx, 0);
        pin_in(p->uart1_rx);
    } else {
        ESP_LOGW(TAG, "UART1 disabled; skipping pin init");
    }

    // Configure SDSPI pins
    if (cfg->sd_enable) {
        if (!pin_skip_usb(p->sdspi_cs))   { pin_out(p->sdspi_cs, 1); }
        if (!pin_skip_usb(p->sdspi_mosi)) { pin_out(p->sdspi_mosi, 0); }
        if (!pin_skip_usb(p->sdspi_miso)) { pin_in(p->sdspi_miso); }
        if (!pin_skip_usb(p->sdspi_sclk)) { pin_out(p->sdspi_sclk, 0); }
    } else {
        ESP_LOGW(TAG, "SD disabled; skipping pin init");
    }

    // Configure I2C pins (always configured; idle open-drain lines)
    pin_od(p->i2c_sda);
    pin_od(p->i2c_scl);

    // Configure sensor pins
    if (cfg->sensor_enable) {
        pin_in(p->sensor_1);
        pin_in(p->sensor_2);
    } else {
        ESP_LOGW(TAG, "Sensors disabled; skipping pin init");
    }

    // Configure LED pins
    if (cfg->led_enable) {
        pin_out(p->led_red, 0);
        pin_out(p->led_green, 0);
    } else {
        ESP_LOGW(TAG, "LEDs disabled; skipping pin init");
    }
}