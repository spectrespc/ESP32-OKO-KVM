# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.5.0] — 2026-11-10

### Added
- The Update tab now lists both OTA slots (`ota_0`/`ota_1`) with their version,
  state and the active slot, and can switch the boot slot. New API:
  `GET /ota/slots` and `POST /ota/slot`. A switch is rejected for the running or
  a non-bootable slot and while an OTA operation is in progress; a failed boot
  after the switch rolls back.

## [1.4.3] — 2026-11-09

### Fixed
- OTA updates can be installed repeatedly: the partition table now uses two
  equal OTA slots (`ota_0` + `ota_1`) instead of `factory` + `ota_0`. With the
  old layout a device booted from `ota_0` failed with
  `ESP_ERR_OTA_PARTITION_CONFLICT`. Such devices must be reflashed once over USB
  (`./build_and_flash.sh flash`); NVS settings survive.
- Typed characters no longer appear twice in `telnet <gateway> <port>` sessions.
  The bridges now perform a minimal Telnet handshake (`WILL ECHO` / `WILL SGA`),
  strip IAC sequences from the client stream and double `0xFF` in the
  UART→client stream. Raw TCP clients such as `nc` see the 6 welcome bytes and
  doubled `0xFF` in binary traffic.

## [1.4.2] — 2026-11-09

### Security
- First boot no longer creates the well-known `admin`/`admin` account: a random
  24-character password is generated and logged once on the USB Serial/JTAG
  console. Devices that still use the old default get a warning in the log.
- `/auth/login` is rate-limited (5 failed attempts → 60 s lockout with
  `Retry-After`); usernames and password hashes are compared in constant time.
- Sessions expire after 24 hours; the cookie is `SameSite=Strict` and carries
  `Max-Age`. `Secure` is intentionally not set because the device serves plain
  HTTP and the cookie would never be sent back.
- `POST /config` and `POST /sd/write` reject cross-origin browser requests.
- Request bodies larger than the handler buffer are rejected instead of being
  silently truncated (`413` on `POST /sd/write`); httpd purges the unread
  remainder when the handler returns.
- SD path validation is now component-based: `.`/`..`, empty components, `\`,
  `:` and control bytes are rejected. SD write/read/rotate resolve paths through
  the same validator instead of concatenating `"/sdcard/%s"`.
- `ota_url` must be an absolute `http://` or `https://` URL without userinfo,
  whitespace or control characters. HTTPS requests verify the server against the
  bundled CA store, and automatic redirects are disabled (no https→http
  downgrade).

### Added
- `X-Content-Type-Options: nosniff` on JSON responses from the REST API.

### Changed
- Telnet bridges now listen on all interfaces (`0.0.0.0`) instead of only the
  SoftAP address, so they are reachable from the LAN through the STA IP as well
  as from the SoftAP. Single-client relay behaviour, `SO_SNDTIMEO` and the 5 s
  bind retry interval are unchanged. The bridge no longer waits for the SoftAP
  to obtain an IP address.

## [1.4.1] — 2026-10-31

### Fixed
- Wi-Fi STA no longer gives up after 5 failures: reconnection is retried
  indefinitely with an exponential 1–60 s backoff.
- A station stuck in `connecting` because DHCP never answered now times out
  after 25 s and reconnects instead of staying there forever.
- `ASSOC_FAIL`, `HANDSHAKE_TIMEOUT` and `4WAY_HANDSHAKE_TIMEOUT` are no longer
  treated as fatal.
- WiFi link state, credentials and counters are now guarded by a mutex, so
  concurrent HTTP requests and driver events cannot race.
- A lost scan after a radio restart no longer leaves `/wifi/scan/results`
  stuck on `{"status":"scanning"}`.

### Changed
- Wi-Fi modem sleep is disabled (`WIFI_PS_NONE`): in the reported log the
  station slept 94.5 % of the time and hit `BEACON_TIMEOUT` (reason 200) at
  −34 dBm RSSI after ~34 hours.
- After 10 minutes of continuous failures the radio is restarted softly
  (`esp_wifi_stop` → 200 ms → `esp_wifi_start`), at most once every 10 minutes.
  The SoftAP is unavailable for 1–2 s; the device is not rebooted.
- `POST /wifi/disconnect` still disables automatic reconnection until the next
  `POST /wifi/connect`.

### Added
- Diagnostic fields in `GET /wifi/status`: `attempts`, `fail_streak`,
  `last_disconnect_reason`, `last_disconnect_reason_name`,
  `last_disconnect_age_s`, `next_retry_in_ms`, `radio_restarts`, `power_save`.
- `wifi_sup` supervisor task (4096 bytes, priority 4) and corresponding web UI
  rows for the new status fields.

## [1.4.0] — 2026-10-21

### Added
- **OTA-обновления** (`ota_handler.[ch]`): манифест `{ota_url}/version.json`,
  сравнение semver, потоковая загрузка в свободный слот по HTTP, проверка
  SHA-256 через PSA Crypto, `esp_ota_set_boot_partition()` и перезагрузка.
  Периодическая проверка (`ota_enable`, `ota_check_interval_min`) только
  сообщает о версии; установка — вручную (`POST /ota/update`). Эндпоинты:
  `GET /ota/status`, `POST /ota/check`, `POST /ota/update`.
- **Откат прошивки** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): новый образ
  подтверждается только после успешного self-test
  (`esp_ota_mark_app_valid_cancel_rollback`), иначе bootloader возвращает
  предыдущий слот.
- **Файловый менеджер SD**: `GET /sd/list` (навигация по каталогам) и
  `GET /sd/download` (потоковое скачивание) с защитой от path traversal.
- **Экспорт настроек**: `GET /config/export` отдаёт `gateway-config.json`
  (`format`, `firmware_version`, `exported_at`, вложенный `config`) без
  `wifi_password` / `sta_password`.
- **`tools/ota_server.py`** — сервер манифеста и образа (только stdlib).
- **Веб-интерфейс:** вкладки **Files** и **Update**, поля OTA в Configuration,
  кнопка **Export settings**.
- Явная версия `1.4.0` (`project(gateway VERSION 1.4.0)`).

### Changed
- **Раскладка разделов:** `factory` 1.625 МБ + `ota_0` 1.625 МБ + SPIFFS 640 КБ
  (было `factory` 2.5 МБ + SPIFFS 1.375 МБ). `nvs`/`otadata` не смещались,
  настройки сохраняются при обычной прошивке.
- `CONFIG_LWIP_MAX_SOCKETS` поднят с 16 до 20; `max_uri_handlers` — с 24 до 32.
- CSV self-test поддерживает альтернативные ожидаемые статусы (`200|503`) для
  конфигурационно-зависимых эндпоинтов.

### Fixed
- **SHT21:** датчик не отдавал измерения (`SHT21 read failed`) из-за неверного
  режима: команды `0xF3/0xF5` — это *no hold master*, а результат читался сразу
  одним `transmit_receive`, как в hold-режиме, и сенсор отвечал NACK.
  Теперь команда отправляется отдельно, выдерживается пауза преобразования
  (85 мс для T, 30 мс для RH) и только затем читается результат.

### Migration
- Прошить обычным `./build_and_flash.sh flash` (без `erase-flash`). NVS и
  `otadata` остаются на прежних адресах, поэтому настройки и Wi-Fi-креды
  сохраняются. После прошивки устройство загружается с `factory`.

## [1.3.1] — 2026-10-21

### Changed
- **GPIO reassignment:** SD-карта перенесена на CS=18, MOSI=1, MISO=3, SCLK=0;
  внешнее реле (`ext_pwr`) на GPIO19, `ext_rst` на GPIO20. Sensor 1/2 = NC.
  Заводские значения в `pins.h` и документация приведены в соответствие с
  фактической распиновкой.

### Fixed
- Реле на GPIO19: кодовый дефолт `ext_pwr_pol = 1` (active-high) корректен.
  Модули реле с 5-В логикой не переключаются от 3.3-В уровня GPIO19 — им нужен
  драйвер (NPN/MOSFET) или 3.3-В-совместимый модуль/SSR; смена полярности в
  веб-интерфейсе сама по себе это не решает.

## [1.3.0] — 2026-10-17

### Added
- **WS2812 status LED** on GPIO8: RGB-индикатор состояния устройства (синий —
  загрузка, жёлтый — подключение к WiFi, зелёный — норма, красный — ошибка,
  голубой — только AP). Управляется через RMT (компонент `espressif/led_strip`).
- **Web UI authentication:** cookie-based сессии, эндпоинты `/auth/login`,
  `/auth/logout`, `/auth/status`, `/auth/password`. Все API-эндпоинты защищены.
  Дефолтные креды `admin` / `admin`, хранятся в NVS (SHA-256).
- **Account tab** в веб-интерфейсе: смена имени пользователя и пароля.

### Changed
- **GPIO8:** дефолт `sensor_1` снят (теперь `GPIO_NUM_NC`). Пин занят WS2812.
  Устройства со старым NVS (`sensor_1 = GPIO8`) мигрируются на `NC` при загрузке.

### Security
- HTTP API больше не общедоступен: все запросы кроме статики и `/auth/*`
  требуют валидной сессионной куки. Запросы с собственного IP устройства
  (on-boot self-test) пропускаются без auth.

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