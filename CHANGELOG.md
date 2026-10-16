# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.2.1] — 2026-10-16

### Fixed
- **Socket exhaustion (accept error 23 / ENFILE):** увеличен пул сокетов
  (`CONFIG_LWIP_MAX_SOCKETS` 10→16 в `sdkconfig.defaults`), включено
  LRU-вытеснение в httpd (`lru_purge_enable = true`) и поднят
  `max_open_sockets` httpd с 7 до 10, чтобы расширенный пул был реально
  задействован. Исправлена ситуация, когда httpd (8 сокетов) + два
  telnet-listen (2 сокета) насыщали пул из 10 сокетов, вызывая отказ в
  обслуживании HTTP-клиентов.

## [1.2.0] — 2026-10-16

### Added
- External device power/reset control. Two configurable GPIOs (`ext_pwr`,
  `ext_rst`; default GPIO20/21) drive the attached device's power rail and reset
  line, each with its own enable flag (`ext_pwr_enable`, `ext_rst_enable`),
  polarity (`ext_pwr_pol`, `ext_rst_pol`) and — for reset — a configurable pulse
  width (`ext_rst_pulse_ms`, 10-5000 ms). The power state persists to NVS
  (`ext_pwr_st`) and is restored on boot with a glitch-safe pin init.
- New REST endpoints `GET /ext/status`, `POST /ext/power` and `POST /ext/reset`;
  the two POST endpoints use the shared same-origin CSRF guard and reset is
  throttled to one pulse every 2 seconds.
- New web UI: an "External" tab (power toggle + reset with confirmation), a
  Dashboard "External device" card and Configuration fields for the enable
  flags, polarity, pulse width and GPIOs.

## [1.1.0] — 2026-10-15

### Added
- Dual telnet bridges: independent TCP→UART relays for UART0 and UART1, each
  with its own port (`telnet0_port`, `telnet1_port`) and enable flag.
- Optional SD-card logging: `uart0_sd_log` / `uart1_sd_log` append raw UART
  traffic to `/sdcard/uar0.log` / `/sdcard/uar1.log`, and the per-sensor flags
  `sht21_sd_log` / `ina219_sd_log` / `ina226_sd_log` / `env_sd_log` append
  periodic rows to `/sdcard/sht21.csv` / `ina219.csv` / `ina226.csv` / `env.csv`.
- Log-file rotation: files are capped at 1 MB with up to 10 rotated copies
  (`sd_write_rotate()`).
- MQTT publishing of sensor data (`mqtt_enable`, `mqtt_broker_url`,
  `mqtt_topic_prefix`): one JSON topic per sensor (`/sht21`, `/ina219`,
  `/ina226`, `/env`), QoS 0.
- exFAT support in FatFs (enabled by patching `fatfs/src/ffconf.h` from
  `main/CMakeLists.txt`, since IDF v6.0.2 exposes no Kconfig option).

### Changed
- Replaced the single `telnet_enable`/`telnet_port`/`telnet_uart` config with
  per-UART `telnet0_*`/`telnet1_*` fields; legacy NVS keys are migrated once on
  load.
- Replaced the single `sensor_sd_log` flag with per-sensor CSV log flags.
- esp-mqtt is pulled from the ESP Component Registry (`espressif/mqtt`) because
  IDF v6 moved it out of core.

### Fixed
- CRITICAL: Default SDSPI MISO/SCLK pins (GPIO12/13) were rejected by the
  USB-reserved validator, blocking any config save from the web UI. Changed the
  defaults to GPIO18/19.
- HIGH: use-after-free window on the MQTT client handle (`s_mqtt`) when
  `esp_mqtt_client_start` fails; the handle is now a C11 atomic with
  release/acquire ordering and is published only after a successful start.
- MEDIUM: Telnet bridge double-close race in the `stop_bridge` timeout path;
  socket fds are now invalidated atomically before `close()`.
- MEDIUM: Telnet bridge partial `send()` silently discarded the remaining UART
  data and did not tear down a stalled connection.
- MEDIUM: CSV header was not rewritten after the first `sd_write_rotate` failed;
  the per-file header flag is now set only on a successful write.
- MEDIUM: Log rotation ignored rename errors, allowing unbounded file growth and
  silent history loss; rotation now checks every rename and returns an error
  instead of appending past the size cap.
- MEDIUM: NVS telnet migration erased the legacy keys before confirming the new
  keys were durable; it now uses a two-phase commit with error checks and writes
  the sentinel key last.
- LOW: removed the dead `ctx->port` field, marked `ctx->task` volatile, made
  `load_config` free its strings before overwriting, made the JSON config
  helpers reject non-integer values, added a range check to `get_json_pin`, and
  made `GET /sensor` return simulated data when the sensor subsystem is disabled.

## [1.0.0] — 2026-10-14

### Added
- USB Serial/JTAG console and UART0/UART1 bridges
- SD card support with FAT filesystem (SPI interface)
- HTTP REST API server on port 80
- WiFi SoftAP + STA mode with network scanning
- I2C sensor drivers: SHT21 (temperature/humidity), INA219/INA226 (voltage/current/power)
- Web UI Single Page Application (SPIFFS, 6 tabs)
- Configuration management with NVS persistent storage
- GPIO pin management with runtime reassignment and validation
- Telnet TCP bridge to UART1
- Self-test framework with HTTP endpoint validation
- System log buffer with `GET /logs` REST endpoint
- `build_and_flash.sh` — unified build/flash/monitor/test script