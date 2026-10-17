// main/status_led.h
#ifndef STATUS_LED_H
#define STATUS_LED_H

// On-board WS2812 (addressable RGB) status LED on PIN_WS2812 (GPIO8). It is
// driven over RMT (espressif/led_strip) and is independent of the configurable
// led_enable (GPIO14/15) outputs.
typedef enum {
    STATUS_LED_INIT,              // blue, steady          — booting
    STATUS_LED_WIFI_DISCONNECTED, // yellow, slow blink    — no STA link
    STATUS_LED_WIFI_CONNECTING,   // yellow, fast blink    — associating
    STATUS_LED_READY,             // green, steady         — running normally
    STATUS_LED_ERROR,             // red, fast blink       — repeated failures
    STATUS_LED_AP_MODE,           // cyan, steady          — AP only (no STA creds)
} status_led_state_t;

// Creates the LED strip, the state queue and the driver task. Safe to call
// once at boot; a failure to create the strip disables the LED (set_state then
// becomes a no-op) but never blocks startup.
void status_led_init(void);

// Requests a new state. Non-blocking and safe to call from any task; the
// request only replaces the single pending state.
void status_led_set_state(status_led_state_t state);

#endif // STATUS_LED_H