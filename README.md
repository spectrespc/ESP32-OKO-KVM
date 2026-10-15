# ESP32-C6 Gateway Firmware

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.2-blue)](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/)
[![Target](https://img.shields.io/badge/target-ESP32--C6-red)](https://www.espressif.com/en/products/socs/esp32-c6)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)

A self-contained IoT gateway firmware for the ESP32-C6. It exposes a REST API
and a single-page web console over Wi-Fi, bridges a UART sensor link to TCP,
writes to an SD card, and reads I2C environmental / power-monitoring sensors.

## Features

- **Dual UART** — UART0 console over native USB Serial/JTAG (115200), UART1
  sensor bridge (9600).
- **SD card** over SPI with a FAT filesystem (read/write/list).
- **Wi-Fi AP + STA** — built-in SoftAP for local access plus a station that can
  scan and connect to an external network.
- **HTTP REST API** on port 80 with JSON payloads.
- **Web UI** — a single-page app served from an on-board SPIFFS partition
  (no SD card required), with 6 tabs: Dashboard, Configuration, WiFi, Sensors,
  SD Card and System Log.
- **I2C sensors** — SHT21 (temperature / humidity) and INA219 / INA226
  (bus voltage, shunt voltage, current, power).
- **Persistent configuration** in NVS with partial-update semantics.
- **Runtime GPIO reassignment** with capability and conflict validation.
- **Dual telnet bridges** relaying TCP to UART0 and UART1, each on its own port.
- **Optional SD-card logging** of UART traffic (`uar0.log` / `uar1.log`) and
  per-sensor readings (`sht21.csv` / `ina219.csv` / `ina226.csv` / `env.csv`),
  with 1 MB rotation, on an exFAT-capable FatFs.
- **MQTT publishing** of sensor readings (one JSON topic per sensor, QoS 0).
- **System log ring buffer** exposed over `GET /logs`.
- **Self-test framework** that validates the HTTP endpoints on boot.
- **Unified `build_and_flash.sh`** for build / flash / monitor / test / clean.

## Hardware Requirements

- An ESP32-C6 development board with at least 4 MB flash (e.g. ESP32-C6FH4).
- **Optional:**
  - microSD card breakout on SPI.
  - SHT21 temperature/humidity sensor on I2C.
  - INA219 or INA226 voltage/current/power monitor on I2C.

Default wiring uses the pins in the [GPIO Pinout](#gpio-pinout) table; every pin
is reconfigurable at runtime.

## Quick Start

### Prerequisites

- [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/)
  installed and its toolchains available.
- Python 3 with the ESP-IDF Python environment.

### Build

```bash
idf.py set-target esp32c6
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

### Monitor

```bash
idf.py -p /dev/ttyACM0 monitor
```

### Using `build_and_flash.sh`

The shell script configures the ESP-IDF environment itself (no `source
export.sh`) and wraps the common tasks:

```bash
./build_and_flash.sh all      # build + flash + monitor
./build_and_flash.sh build
./build_and_flash.sh flash
./build_and_flash.sh monitor
./build_and_flash.sh test     # run the HTTP endpoint matrix
./build_and_flash.sh clean
./build_and_flash.sh env
./build_and_flash.sh help
```

Environment overrides: `IDF_PATH_OVERRIDE`, `IDF_TOOLS_PATH`, `PORT`,
`TEST_HOST`.

## Project Structure

```
gateway/
├── CMakeLists.txt          # Top-level project definition
├── partitions.csv          # Custom 4 MB layout (2.5M app + 1.5M SPIFFS)
├── sdkconfig.defaults      # SDK defaults (target, console, SPIFFS, ...)
├── build_and_flash.sh      # Canonical build/flash/monitor/test script
├── main/
│   ├── main.c              # app_main(): subsystem init order
│   ├── pins.[ch]           # Factory-default GPIO map + pin init
│   ├── config.[ch]         # app_config_t, NVS load/save/validate
│   ├── pin_validation.[ch] # ESP32-C6 GPIO capability table + checks
│   ├── uart_handler.[ch]   # UART0 console + UART1 bridge
│   ├── sd_handler.[ch]     # SD/SPI + FAT filesystem
│   ├── sensor_handler.[ch] # SHT21 / INA219 / INA226 readers
│   ├── http_server.[ch]    # REST server + endpoint registration
│   ├── web_handlers.[ch]   # SPIFFS mount + static catch-all
│   ├── log_handler.[ch]    # In-RAM log ring buffer + GET /logs
│   ├── wifi_ap.[ch]        # Wi-Fi init (AP+STA mode, SoftAP)
│   ├── wifi_scan.[ch]      # STA scan/connect/status/disconnect
│   ├── system_handler.[ch] # Reboot / factory-reset handlers
│   ├── self_test.[ch]      # Boot-time HTTP endpoint self-test
│   └── telnet_bridge.[ch]  # TCP -> UART0/UART1 relays (+ optional SD log)
├── web/
│   └── index.html          # Self-contained SPA (inline CSS + JS)
├── tests/
│   └── http_endpoints.csv  # Shared HTTP expectation matrix
└── tools/
    └── gen_http_tests.py   # CSV -> C header generator for the self-test
```

## REST API Reference

All endpoints return JSON unless noted. There is no authentication; the device
is intended for a trusted local network (the SoftAP or your own Wi-Fi).

| Method | Path                 | Description                                    | Success |
|--------|----------------------|------------------------------------------------|---------|
| GET    | `/`                  | Web UI (SPA from SPIFFS)                       | 200     |
| GET    | `/config`            | Current configuration                          | 200     |
| POST   | `/config`            | Partial configuration update                   | 200     |
| GET    | `/sensor`            | Current sensor readings                        | 200     |
| POST   | `/sensor`            | Accept a sensor payload (currently discarded)  | 200     |
| GET    | `/sd/status`         | SD card status                                 | 200     |
| POST   | `/sd/write`          | Write the request body to the SD card          | 200     |
| GET    | `/logs`              | Recent log lines                               | 200     |
| GET    | `/wifi/scan`         | Start an asynchronous Wi-Fi scan               | 202     |
| GET    | `/wifi/scan/results` | Scan results (or `scanning`)                   | 200     |
| POST   | `/wifi/connect`      | Save STA credentials and connect               | 200     |
| GET    | `/wifi/status`       | STA connection state                           | 200     |
| POST   | `/wifi/disconnect`   | Drop the STA link and forget credentials       | 200     |
| POST   | `/system/reboot`     | Acknowledge, then reboot the device            | 200     |
| POST   | `/system/reset`      | Acknowledge, erase NVS, then reboot            | 200     |

Error responses use `400 Bad Request` (malformed payload / invalid values),
`500 Internal Server Error` (I/O failure) or `503 Service Unavailable`
(subsystem disabled, e.g. SD).

### `GET /config`

```bash
curl http://192.168.4.1/config
```

```json
{
  "baud_rate": 115200,
  "uart0_enable": 1,
  "uart1_enable": 1,
  "sd_enable": 1,
  "sensor_enable": 1,
  "led_enable": 1,
  "telnet0_enable": 0,
  "telnet0_port": 2323,
  "telnet1_enable": 0,
  "telnet1_port": 23,
  "uart0_sd_log": 0,
  "uart1_sd_log": 0,
  "sht21_sd_log": 0,
  "ina219_sd_log": 0,
  "ina226_sd_log": 0,
  "env_sd_log": 0,
  "i2c_enable": 0,
  "sht21_enable": 0,
  "ina219_enable": 0,
  "ina226_enable": 0,
  "ina219_addr": 64,
  "ina226_addr": 64,
  "wifi_ssid": "",
  "server_url": "",
  "mqtt_enable": 0,
  "mqtt_broker_url": "",
  "mqtt_topic_prefix": "",
  "pins": {
    "uart0_tx": 16, "uart0_rx": 17,
    "uart1_tx": 4,  "uart1_rx": 5,
    "sdspi_cs": 10, "sdspi_mosi": 11, "sdspi_miso": 12, "sdspi_sclk": 13,
    "i2c_sda": 6,   "i2c_scl": 7,
    "sensor_1": 8,  "sensor_2": 9,
    "led_red": 14,  "led_green": 15
  }
}
```

Addresses are returned in decimal (`64` = `0x40`). The AP password
(`wifi_password`) is deliberately not exposed by `GET /config`.

### `POST /config`

Partial update: only the fields present in the JSON body are changed, and the
new configuration is persisted to NVS. The response is always
`{"message":"Configuration saved. Reboot to apply."}` on success — changes take
effect after the next reboot.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"i2c_enable":1,"sht21_enable":1,"ina219_enable":1,"ina219_addr":64}'
```

Enable flags are `0`/`1`. INA addresses must be in the `0x40`-`0x4F` range
(`64`-`79` decimal). Nested pin updates go under `"pins"`:

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"pins":{"led_red":20,"led_green":21}}'
```

Validation failures return `400` with a machine-readable code in `"error"`:

| Code | Meaning |
|------|---------|
| `PIN_INVALID` / `PIN_RANGE` | GPIO number out of range for the ESP32-C6 |
| `PIN_DIRECTION` | GPIO cannot act in the requested direction |
| `PIN_USB_CONFLICT` | GPIO 12/13 conflict with the USB Serial/JTAG console |
| `PIN_DUPLICATE` | Two roles assigned to the same GPIO |
| `PIN_OPEN_DRAIN` | I2C pin cannot be configured open-drain |
| `I2C_CONFLICT` | SDA == SCL, both INA monitors enabled, or SHT21/INA address clash |
| `ADDR_INVALID` | INA address outside `0x40`-`0x4F` |
| `PORT_INVALID` | `telnet0_port` / `telnet1_port` outside `1`-`65535` |
| `PORT_CONFLICT` | Both telnet bridges enabled on the same port |
| `UART_INVALID` | A telnet bridge enabled while its UART is disabled |
| `MQTT_INVALID` | MQTT enabled without a (non-empty, ≤255 char) broker URL |

### `GET /sensor`

```json
{
  "temperature": 253,
  "humidity": 605,
  "pressure": 1013,
  "light_level": 500,
  "bus_voltage": 3300,
  "shunt_voltage": 1250,
  "current": 150,
  "power": 495
}
```

Values are integer fixed-point so they serialize losslessly:
temperature is 0.1 °C, humidity 0.1 %RH, bus voltage mV, shunt voltage µV,
current mA and power mW (`253` = 25.3 °C, `3300` = 3.300 V). Fields whose
sensor is disabled fall back to simulated values.

### `GET /logs`

```json
{ "logs": ["I (123) main: Starting ESP32-C6 Gateway Firmware", "..."] }
```

Chronological, in-RAM ring buffer of the most recent ESP-IDF log lines.

### WiFi

```bash
# 1. Trigger a scan, then poll for results
curl http://192.168.4.1/wifi/scan            # -> 202 {"status":"scanning"}
curl http://192.168.4.1/wifi/scan/results    # -> {"networks":[{"ssid":"...","rssi":-45,"authmode":3,"channel":6}, ...]}

# 2. Connect to a network
curl -X POST http://192.168.4.1/wifi/connect \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"MyNetwork","password":"secret"}'

# 3. Check state / disconnect
curl http://192.168.4.1/wifi/status
curl -X POST http://192.168.4.1/wifi/disconnect
```

`GET /wifi/status` returns
`{"connected":true,"ssid":"MyNetwork","ip":"192.168.1.42","rssi":-51,"state":"connected"}`.
`POST /wifi/connect` with a missing `ssid` returns `400`.

### System

```bash
curl -X POST http://192.168.4.1/system/reboot   # -> 200, then reboot
curl -X POST http://192.168.4.1/system/reset    # -> 200, erase NVS, reboot
```

Both handlers send the HTTP response **before** restarting so the client always
receives a `200`.

## GPIO Pinout

Factory defaults; every assignment is reconfigurable via `POST /config` and
persisted to NVS.

| Function    | Default GPIO | Direction | Notes                              |
|-------------|--------------|-----------|------------------------------------|
| UART0 TX    | 16           | OUT       | USB Serial/JTAG console (115200)   |
| UART0 RX    | 17           | IN        | USB Serial/JTAG console            |
| UART1 TX    | 4            | OUT       | Sensor bridge (9600)               |
| UART1 RX    | 5            | IN        | Sensor bridge                      |
| SDSPI CS    | 10           | OUT       | SD chip select                     |
| SDSPI MOSI  | 11           | OUT       | SD data out                        |
| SDSPI MISO  | 12           | IN        | USB D- (**conflicts with USB console**) |
| SDSPI SCLK  | 13           | OUT       | USB D+ (**conflicts with USB console**) |
| I2C SDA     | 6            | OD        | Sensor bus data                    |
| I2C SCL     | 7            | OD        | Sensor bus clock                   |
| Sensor 1    | 8            | IN/OUT    | Generic GPIO (strapping pin)       |
| Sensor 2    | 9            | IN/OUT    | Generic GPIO (strapping pin)       |
| LED red     | 14           | OUT       | Status LED                         |
| LED green   | 15           | OUT       | Status LED                         |

> **USB console note:** GPIO 12 and 13 are the ESP32-C6 native USB D-/D+ lines.
> While `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (the default), the SD card's
> MISO/SCLK are not initialized on those pins, the SD card is unavailable, and
> pin validation rejects any assignment to them (`PIN_USB_CONFLICT`). GPIO 2, 8
> and 9 are strapping pins and only produce a warning.

## I2C Sensor Support

The I2C bus is disabled by default. Enable it and select the populated sensors
in the Configuration tab or via `POST /config`:

| Sensor  | NVS flag     | Address                        | Measurements                          |
|---------|--------------|--------------------------------|---------------------------------------|
| SHT21   | `sht21_en`   | fixed `0x40`                   | temperature (0.1 °C), humidity (0.1 %) |
| INA219  | `ina219_en`  | `ina219_adr` (default `0x40`)  | bus V (mV), shunt V (µV), I (mA), P (mW) |
| INA226  | `ina226_en`  | `ina226_adr` (default `0x40`)  | bus V (mV), shunt V (µV), I (mA), P (mW) |

Wiring uses the `i2c_sda` / `i2c_scl` pins (default GPIO 6 / 7). Because SHT21
and the INA devices share the `0x40` default, addresses must be set manually.
Validation rejects a configuration where:

- two roles share a GPIO (`PIN_DUPLICATE`),
- both INA219 and INA226 are enabled (single sensor data model), or
- an enabled INA device collides with an enabled SHT21 at `0x40`.

## WiFi

- **AP mode (always on):** SoftAP with SSID `ESP32-Gateway` and password
  `gateway123` (WPA2-PSK, channel 1, up to 4 clients). These are the fallbacks
  used when no `wifi_ssid` / `wifi_password` are configured; the AP can be
  re-provisioned through those config fields. An empty password makes the AP
  open. Local access is always available at **http://192.168.4.1**.
- **STA mode:** the station side runs alongside the AP. Scan for networks in
  the WiFi tab, then connect. STA credentials are stored separately from the AP
  credentials in NVS (`sta_ssid` / `sta_password`) and are restored on boot.

## Web Interface

The SPA is a single self-contained `web/index.html` (inline CSS + JS) baked into
the SPIFFS partition at build time and served by the HTTP server's catch-all
handler. Open **http://192.168.4.1** after joining the AP. It has six tabs:
Dashboard, Configuration, WiFi, Sensors, SD Card and System Log. The
Configuration tab renders GPIO dropdowns client-side from the ESP32-C6 pin table
and disables pins already assigned to another role.

## Configuration

Configuration lives in the NVS `storage` namespace. Keys (≤15 chars):

| Key             | Type | Default      | Notes                              |
|-----------------|------|--------------|------------------------------------|
| `baud_rate`     | u32  | `115200`     | UART0 console baud                  |
| `uart0_enable`  | u8   | `1`          | UART0 console on/off                |
| `uart1_enable`  | u8   | `1`          | UART1 sensor bridge on/off          |
| `sd_enable`     | u8   | `1`          | SD card subsystem on/off            |
| `sensor_enable` | u8   | `1`          | Sensor subsystem on/off             |
| `led_enable`    | u8   | `1`          | Status LEDs on/off                  |
| `telnet0_en`    | u8   | `0`          | Telnet → UART0 bridge on/off        |
| `telnet0_port`  | u16  | `2323`       | UART0 bridge TCP port (1-65535)     |
| `telnet1_en`    | u8   | `0`          | Telnet → UART1 bridge on/off        |
| `telnet1_port`  | u16  | `23`         | UART1 bridge TCP port (1-65535)     |
| `u0_sd_log`     | u8   | `0`          | Log UART0 traffic to `/sdcard/uar0.log`  |
| `u1_sd_log`     | u8   | `0`          | Log UART1 traffic to `/sdcard/uar1.log`  |
| `sht21_sd`      | u8   | `0`          | Log SHT21 rows to `/sdcard/sht21.csv`    |
| `ina219_sd`     | u8   | `0`          | Log INA219 rows to `/sdcard/ina219.csv`  |
| `ina226_sd`     | u8   | `0`          | Log INA226 rows to `/sdcard/ina226.csv`  |
| `env_sd`        | u8   | `0`          | Log pressure/light to `/sdcard/env.csv`  |
| `i2c_en`        | u8   | `0`          | I2C bus on/off                      |
| `sht21_en`      | u8   | `0`          | SHT21 on/off                        |
| `ina219_en`     | u8   | `0`          | INA219 on/off                       |
| `ina226_en`     | u8   | `0`          | INA226 on/off                       |
| `ina219_adr`    | u8   | `0x40`       | INA219 I2C address (0x40-0x4F)      |
| `ina226_adr`    | u8   | `0x40`       | INA226 I2C address (0x40-0x4F)      |
| `pin_<role>`    | u32  | see pinout   | One key per GPIO role               |
| `wifi_ssid`     | str  | (unset)      | SoftAP SSID override                |
| `wifi_password` | str  | (unset)      | SoftAP password override            |
| `server_url`    | str  | (unset)      | Optional upstream server URL        |
| `mqtt_en`       | u8   | `0`          | MQTT publishing on/off              |
| `mqtt_url`      | str  | (unset)      | Broker URL (`mqtt://` or `mqtts://`)|
| `mqtt_topic`    | str  | (unset)      | Topic prefix (default `gateway`)    |
| `sta_ssid`      | str  | (unset)      | Saved station SSID (from WiFi tab)  |
| `sta_password`  | str  | (unset)      | Saved station password              |

`POST /config` changes are written to NVS immediately but **require a reboot to
apply**. Use `POST /system/reboot` (or `POST /system/reset` for factory
defaults).

## Build Scripts

- **`build_and_flash.sh`** — the canonical wrapper. Commands: `build`, `flash`,
  `monitor`, `all`, `test`, `clean`, `env`, `help`. It configures the ESP-IDF
  path, toolchains and Python venv manually, so it works where `idf.py` is not
  already on `PATH`.
- **`tools/gen_http_tests.py`** — generates the on-device self-test expectation
  table from `tests/http_endpoints.csv`. It is invoked automatically by CMake
  during the build.

## Testing

- **Self-test:** runs automatically on boot over the internal AP and logs the
  results to the console. The expectation table is generated from
  `tests/http_endpoints.csv`.
- **HTTP endpoint matrix:** `./build_and_flash.sh test` runs every row of
  `tests/http_endpoints.csv` with `curl` against `TEST_HOST`
  (default `192.168.4.1`). The host must be joined to the SoftAP.

Both consumers share the same CSV, so the on-device and host-side tests stay in
sync.

## License

Licensed under the [Apache License 2.0](LICENSE).