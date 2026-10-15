# Справочник конфигурации

Вся конфигурация хранится в энергонезависимой памяти NVS в пространстве имён
`storage`. Документ описывает параметры, формат JSON для `POST /config`,
семантику частичного обновления, переназначение GPIO, валидацию, миграцию
устаревших ключей и сброс к заводским настройкам.

## Полная таблица ключей NVS

Ключи ограничены 15 символами.

| Ключ | Тип | По умолчанию | JSON-поле | Примечания |
|------|-----|--------------|-----------|------------|
| `baud_rate` | u32 | `115200` | `baud_rate` | Скорость консоли UART0 |
| `uart0_enable` | u8 | `1` | `uart0_enable` | Консоль UART0 вкл/выкл |
| `uart1_enable` | u8 | `1` | `uart1_enable` | Мост датчиков UART1 вкл/выкл |
| `sd_enable` | u8 | `1` | `sd_enable` | Подсистема SD вкл/выкл |
| `sensor_enable` | u8 | `1` | `sensor_enable` | Подсистема датчиков вкл/выкл |
| `led_enable` | u8 | `1` | `led_enable` | Статусные светодиоды вкл/выкл |
| `telnet0_en` | u8 | `0` | `telnet0_enable` | Telnet→UART0 вкл/выкл |
| `telnet0_port` | u16 | `2323` | `telnet0_port` | Порт моста UART0 (1–65535) |
| `telnet1_en` | u8 | `0` | `telnet1_enable` | Telnet→UART1 вкл/выкл |
| `telnet1_port` | u16 | `23` | `telnet1_port` | Порт моста UART1 (1–65535) |
| `u0_sd_log` | u8 | `0` | `uart0_sd_log` | Лог трафика UART0 → `/sdcard/uar0.log` |
| `u1_sd_log` | u8 | `0` | `uart1_sd_log` | Лог трафика UART1 → `/sdcard/uar1.log` |
| `sht21_sd` | u8 | `0` | `sht21_sd_log` | CSV SHT21 → `/sdcard/sht21.csv` |
| `ina219_sd` | u8 | `0` | `ina219_sd_log` | CSV INA219 → `/sdcard/ina219.csv` |
| `ina226_sd` | u8 | `0` | `ina226_sd_log` | CSV INA226 → `/sdcard/ina226.csv` |
| `env_sd` | u8 | `0` | `env_sd_log` | CSV окружения → `/sdcard/env.csv` |
| `i2c_en` | u8 | `0` | `i2c_enable` | Шина I2C вкл/выкл |
| `sht21_en` | u8 | `0` | `sht21_enable` | SHT21 вкл/выкл |
| `ina219_en` | u8 | `0` | `ina219_enable` | INA219 вкл/выкл |
| `ina226_en` | u8 | `0` | `ina226_enable` | INA226 вкл/выкл |
| `ina219_adr` | u8 | `0x40` | `ina219_addr` | I2C-адрес INA219 (0x40–0x4F) |
| `ina226_adr` | u8 | `0x40` | `ina226_addr` | I2C-адрес INA226 (0x40–0x4F) |
| `pin_uart0_tx` | u32 | `16` | `pins.uart0_tx` | Пин UART0 TX |
| `pin_uart0_rx` | u32 | `17` | `pins.uart0_rx` | Пин UART0 RX |
| `pin_uart1_tx` | u32 | `4` | `pins.uart1_tx` | Пин UART1 TX |
| `pin_uart1_rx` | u32 | `5` | `pins.uart1_rx` | Пин UART1 RX |
| `pin_sdspi_cs` | u32 | `10` | `pins.sdspi_cs` | SD chip select |
| `pin_sdspi_mosi` | u32 | `11` | `pins.sdspi_mosi` | SD MOSI |
| `pin_sdspi_miso` | u32 | `18` | `pins.sdspi_miso` | SD MISO |
| `pin_sdspi_sclk` | u32 | `19` | `pins.sdspi_sclk` | SD SCLK |
| `pin_i2c_sda` | u32 | `6` | `pins.i2c_sda` | I2C SDA |
| `pin_i2c_scl` | u32 | `7` | `pins.i2c_scl` | I2C SCL |
| `pin_sensor_1` | u32 | `8` | `pins.sensor_1` | Универсальный GPIO 1 |
| `pin_sensor_2` | u32 | `9` | `pins.sensor_2` | Универсальный GPIO 2 |
| `pin_led_red` | u32 | `14` | `pins.led_red` | Красный LED |
| `pin_led_green` | u32 | `15` | `pins.led_green` | Зелёный LED |
| `wifi_ssid` | str | (нет) | `wifi_ssid` | Переопределение SSID SoftAP (≤31) |
| `wifi_password` | str | (нет) | `wifi_password` | Переопределение пароля SoftAP (≤63) |
| `server_url` | str | (нет) | `server_url` | URL вышестоящего сервера (≤255) |
| `mqtt_en` | u8 | `0` | `mqtt_enable` | Публикация MQTT вкл/выкл |
| `mqtt_url` | str | (нет) | `mqtt_broker_url` | URL брокера (≤255) |
| `mqtt_topic` | str | (нет) | `mqtt_topic_prefix` | Префикс топика (≤64, по умолчанию `gateway`) |
| `sta_ssid` | str | (нет) | — | SSID станции (сохраняется через `POST /wifi/connect`) |
| `sta_password` | str | (нет) | — | Пароль станции (сохраняется через `POST /wifi/connect`) |

> `GET /config` намеренно **не возвращает** `wifi_password` и `sta_password`.

## Формат JSON для `POST /config`

- `Content-Type: application/json`, тело ≤ 4096 байт.
- Флаги включения — целое `0` или `1`.
- Пины — целые в диапазоне `0`–`22`, вложенные в объект `"pins"`.
- Адреса INA — целые `0x40`–`0x4F` (`64`–`79`).
- Строки ограничены по длине (см. таблицу выше).

Успех всегда:

```json
{"message":"Configuration saved. Reboot to apply."}
```

## Частичное обновление конфигурации

`POST /config` — это **частичное** обновление: изменяются только поля,
присутствующие в теле запроса; остальные сохраняют текущие значения. Если тело
пустое или не содержит ни одного распознанного поля, возвращается `400`:

```json
{"error":"No valid configuration fields provided"}
```

Пример комбинированного обновления:

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{
        "i2c_enable": 1,
        "sht21_enable": 1,
        "ina219_enable": 1,
        "ina219_addr": 65,
        "sht21_sd_log": 1,
        "mqtt_enable": 1,
        "mqtt_broker_url": "mqtt://192.168.1.10:1883",
        "mqtt_topic_prefix": "gateway"
      }'
```

> Изменения сохраняются в NVS немедленно, но **применяются после перезагрузки**.
> Используйте `POST /system/reboot`.

### Числовые значения вне диапазона

Нецелые значения или значения вне допустимого диапазона для числового поля
приводят к `400`:

```json
{"error":"Numeric value out of range"}
```

### Слишком длинные строки

```json
{"error":"String too long (wifi_ssid<=31, wifi_password<=63, server_url<=255, mqtt_broker_url<=255, mqtt_topic_prefix<=64)"}
```

## Переназначение GPIO на лету

Любую роль GPIO можно переназначить через объект `"pins"`. Заводские значения —
лишь стартовая точка.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"pins":{"led_red":20,"led_green":21}}'
```

Правила:

- Каждый пин должен быть уникален (кроме петель — см. валидацию).
- Пин должен поддерживать требуемое направление.
- I2C-пины должны поддерживать open-drain.
- GPIO 12/13 нельзя назначать при активной USB-консоли.
- Изменения вступают в силу после перезагрузки.

## Валидация

Валидация выполняется в `config_validate_and_set()` (конфигурация + конфликты) и
`validate_pin_config()` (возможности пинов). При ошибке весь запрос отклоняется
с `400` и кодом в поле `"error"`.

### Коды ошибок пинов

| Код | Условие |
|-----|---------|
| `PIN_INVALID` | GPIO вне диапазона 0–22 (или `null`-конфигурация) |
| `PIN_NO_CAPABILITY` | GPIO не поддерживает требуемое направление или open-drain |
| `PIN_USB_CONFLICT` | GPIO 12/13 при активной консоли USB Serial/JTAG |
| `PIN_DUPLICATE` | Один GPIO назначен двум ролям |
| `PIN_I2C_CONFLICT` | I2C SDA и SCL — один и тот же GPIO |

### Коды ошибок конфигурации

| Код | Условие |
|-----|---------|
| `ADDR_INVALID` | `ina219_addr` или `ina226_addr` вне `0x40`–`0x4F` |
| `I2C_CONFLICT` | Включены оба INA219 и INA226; либо включённый INA использует `0x40` вместе с включённым SHT21 |
| `PORT_INVALID` | `telnet0_port` или `telnet1_port` равно `0` |
| `PORT_CONFLICT` | Оба telnet-моста включены на одном порту |
| `UART_INVALID` | Telnet-мост включён при выключенном соответствующем UART |
| `MQTT_INVALID` | MQTT включён без непустого URL брокера или URL длиннее 255 символов |

### Флаг strapping-пинов

GPIO 2, 8 и 9 можно назначать, но при этом пишется предупреждение в лог — их
состояние влияет на режим загрузки. Валидация их не отклоняет.

## Миграция устаревших ключей (telnet legacy)

Ранние версии использовали единый telnet-мост с ключами `telnet_enable`,
`telnet_port` и `telnet_uart` (0 = UART0, 1 = UART1). При загрузке выполняется
одноразовая миграция:

1. Если присутствует старый ключ `telnet_enable`, а нового `telnet0_en` нет,
   конфигурация «переселяется» на соответствующий UART:
   - `telnet_uart == 0` → значения уходят в `telnet0_enable` / `telnet0_port`;
   - иначе → значения уходят в `telnet1_enable` / `telnet1_port`.
2. Новые ключи пишутся и фиксируются; **ключ-маркер `telnet0_en` пишется
   последним**.
3. Только после успешной фиксации новых ключей стираются устаревшие
   (`telnet_enable`, `telnet_port`, `telnet_uart`).

Если запись новых ключей не удалась, устаревшие ключи сохраняются для следующей
загрузки (миграция повторится). Это гарантирует, что миграция выполнится ровно
один раз и без потери данных.

## Сброс к заводским настройкам

```bash
curl -X POST http://192.168.4.1/system/reset
```

Обработчик стирает раздел NVS (`nvs_flash_erase()`), что сбрасывает всю
конфигурацию к заводским значениям, и перезагружает устройство. Ответ
`{"status":"resetting"}` отправляется до перезапуска.

> Сброс удаляет также учётные данные станции (`sta_ssid` / `sta_password`).
> SoftAP вернётся к значениям по умолчанию (`ESP32-Gateway` / `gateway123`).

## Резервное копирование и восстановление

- **Экспорт:** `GET /config` отдаёт текущую конфигурацию (без паролей). Пины,
  флаги и адреса можно сохранить и повторно применить через `POST /config`.
- **Импорт:** передайте те же поля в `POST /config`. Отсутствующие поля не
  изменяются.
- `wifi_password` и `sta_password` в экспорт не попадают — задавайте их
  отдельно.

## См. также

- [`api.md`](api.md) — REST API
- [`hardware.md`](hardware.md) — распиновка и подключение
- [`architecture.md`](architecture.md) — устройство подсистем