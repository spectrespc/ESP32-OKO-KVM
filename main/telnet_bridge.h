// main/telnet_bridge.h
#ifndef TELNET_BRIDGE_H
#define TELNET_BRIDGE_H

// Each bridge starts a FreeRTOS task that listens on its own raw TCP port and
// relays bytes between the connected TCP client and one UART (bridge 0 = UART0,
// bridge 1 = UART1). Ports come from config.telnet0_port / config.telnet1_port.
// The listener binds to 0.0.0.0, so it is reachable from the SoftAP and from
// the LAN through the STA address. A single client is served at a time per
// bridge. It performs a minimal Telnet handshake: WILL ECHO + WILL SGA on
// connect, IAC sequences are stripped client->UART and 0xFF is doubled
// UART->client. Raw TCP clients such as 'nc' will see the 6 welcome bytes.
// Only meaningful when the matching telnetN_enable and uartN_enable flags are
// set.
void init_telnet_bridge0(void);
void init_telnet_bridge1(void);

// Stops a bridge and releases its sockets. Provided for completeness; the
// apply-on-reboot model does not require these during normal operation.
void deinit_telnet_bridge0(void);
void deinit_telnet_bridge1(void);

#endif // TELNET_BRIDGE_H