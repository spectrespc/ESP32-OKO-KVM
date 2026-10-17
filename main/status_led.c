// main/status_led.c
#include "status_led.h"
#include "pins.h"
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "status_led";

// Keep the WS2812 dim: at full brightness a single addressable LED is
// uncomfortably bright as an indicator (~25% duty).
#define LED_DIM 64

typedef struct {
    uint8_t r, g, b;
    uint16_t on_ms;   // "on" phase duration
    uint16_t off_ms;  // 0 => steady (no off phase)
} led_pattern_t;

static const led_pattern_t s_patterns[] = {
    [STATUS_LED_INIT]              = { 0,      0,      LED_DIM, 0,   0   },
    [STATUS_LED_WIFI_DISCONNECTED] = { LED_DIM, LED_DIM, 0,     500, 500 },
    [STATUS_LED_WIFI_CONNECTING]   = { LED_DIM, LED_DIM, 0,     125, 125 },
    [STATUS_LED_READY]             = { 0,      LED_DIM, 0,     0,   0   },
    [STATUS_LED_ERROR]             = { LED_DIM, 0,      0,     125, 125 },
    [STATUS_LED_AP_MODE]           = { 0,      LED_DIM, LED_DIM, 0,   0   },
};

static led_strip_handle_t s_strip = NULL;
static QueueHandle_t s_state_queue = NULL;

static void set_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    if (led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK) {
        led_strip_refresh(s_strip);
    }
}

static void status_led_task(void* arg)
{
    (void)arg;
    status_led_state_t state = STATUS_LED_INIT;

    for (;;) {
        const led_pattern_t* p = &s_patterns[state];

        if (p->off_ms == 0) {
            // Steady: hold the colour until a new state arrives.
            set_pixel(p->r, p->g, p->b);
            status_led_state_t next;
            if (xQueueReceive(s_state_queue, &next, portMAX_DELAY) == pdTRUE) {
                state = next;
            }
            continue;
        }

        set_pixel(p->r, p->g, p->b);
        status_led_state_t next;
        if (xQueueReceive(s_state_queue, &next, pdMS_TO_TICKS(p->on_ms)) == pdTRUE) {
            state = next;
            continue;
        }

        set_pixel(0, 0, 0);
        if (xQueueReceive(s_state_queue, &next, pdMS_TO_TICKS(p->off_ms)) == pdTRUE) {
            state = next;
        }
    }
}

void status_led_init(void)
{
    if (s_strip) {
        return;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_WS2812,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK || s_strip == NULL) {
        ESP_LOGE(TAG, "Failed to create WS2812 strip: %s", esp_err_to_name(err));
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "WS2812 status LED initialized on GPIO%d", (int)PIN_WS2812);

    // Queue length 1 so status_led_set_state() can use xQueueOverwrite() to
    // replace the pending state instead of queueing every intermediate value.
    s_state_queue = xQueueCreate(1, sizeof(status_led_state_t));
    if (!s_state_queue) {
        ESP_LOGE(TAG, "Failed to create status LED queue");
        return;
    }

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start status LED task");
    }
}

void status_led_set_state(status_led_state_t state)
{
    if (!s_state_queue) {
        return;
    }
    xQueueOverwrite(s_state_queue, &state);
}