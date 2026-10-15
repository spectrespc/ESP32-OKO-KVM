// main/uart_handler.h
#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include "driver/uart.h"
#include "pins.h"

// UART configuration for UART0 (console)
#define UART0_CONFIG_BAUD_RATE 115200
#define UART0_CONFIG_DATA_BITS UART_DATA_8_BITS
#define UART0_CONFIG_PARITY UART_PARITY_DISABLE
#define UART0_CONFIG_STOP_BITS UART_STOP_BITS_1
#define UART0_CONFIG_FLOW_CTRL UART_HW_FLOWCTRL_DISABLE

// UART configuration for UART1
#define UART1_CONFIG_BAUD_RATE 9600
#define UART1_CONFIG_DATA_BITS UART_DATA_8_BITS
#define UART1_CONFIG_PARITY UART_PARITY_DISABLE
#define UART1_CONFIG_STOP_BITS UART_STOP_BITS_1
#define UART1_CONFIG_FLOW_CTRL UART_HW_FLOWCTRL_DISABLE

void init_uart0(void);
void init_uart1(void);
void uart0_send_data(const uint8_t* data, size_t len);
void uart1_send_data(const uint8_t* data, size_t len);

#endif // UART_HANDLER_H