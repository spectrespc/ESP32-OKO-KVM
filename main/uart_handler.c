// main/uart_handler.c
#include "uart_handler.h"
#include "config.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "pins.h"

static const char* TAG = "uart_handler";

void init_uart0(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg->uart0_enable) {
        ESP_LOGW(TAG, "UART0 disabled; skipping init");
        return;
    }

    uart_config_t uart0_config = {
        .baud_rate = UART0_CONFIG_BAUD_RATE,
        .data_bits = UART0_CONFIG_DATA_BITS,
        .parity = UART0_CONFIG_PARITY,
        .stop_bits = UART0_CONFIG_STOP_BITS,
        .flow_ctrl = UART0_CONFIG_FLOW_CTRL,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver for UART0
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart0_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, cfg->pins.uart0_tx, cfg->pins.uart0_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 4096, 0, NULL, 0));
    
    ESP_LOGI(TAG, "UART0 initialized (tx=%d rx=%d)",
             (int)cfg->pins.uart0_tx, (int)cfg->pins.uart0_rx);
}

void init_uart1(void)
{
    const app_config_t* cfg = get_config();
    if (!cfg->uart1_enable) {
        ESP_LOGW(TAG, "UART1 disabled; skipping init");
        return;
    }

    uart_config_t uart1_config = {
        .baud_rate = UART1_CONFIG_BAUD_RATE,
        .data_bits = UART1_CONFIG_DATA_BITS,
        .parity = UART1_CONFIG_PARITY,
        .stop_bits = UART1_CONFIG_STOP_BITS,
        .flow_ctrl = UART1_CONFIG_FLOW_CTRL,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver for UART1
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart1_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, cfg->pins.uart1_tx, cfg->pins.uart1_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 4096, 4096, 0, NULL, 0));
    
    ESP_LOGI(TAG, "UART1 initialized (tx=%d rx=%d)",
             (int)cfg->pins.uart1_tx, (int)cfg->pins.uart1_rx);
}

void uart0_send_data(const uint8_t* data, size_t len)
{
    if (data && len > 0) {
        uart_write_bytes(UART_NUM_0, (const char*)data, len);
    }
}

void uart1_send_data(const uint8_t* data, size_t len)
{
    if (data && len > 0) {
        uart_write_bytes(UART_NUM_1, (const char*)data, len);
    }
}