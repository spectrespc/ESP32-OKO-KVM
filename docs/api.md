# Справочник REST API

Подробный справочник HTTP REST API ESP32-C6 Gateway. Все эндпоинты отвечают
JSON, если не указано иное.

- **Базовый URL:** `http://192.168.4.1` (адрес SoftAP по умолчанию)
- **Порт:** 80
- **Аутентификация:** отсутствует. Устройство рассчитано на доверенную
  локальную сеть (SoftAP или ваша собственная сеть Wi-Fi). **Не выставляйте
  устройство в открытый интернет.**
- **CSRF:** разрушающие эндпоинты `/system/*` проверяют заголовок `Origin` и
  отклоняют кросс-доменные запросы (`403`). Запросы без `Origin` (например
  `curl`) разрешены.

## Сводная таблица эндпоинтов

| Метод | Путь | Успех | Краткое описание |
|-------|------|-------|------------------|
| GET | `/` | 200 | Веб-интерфейс (SPA из SPIFFS) |
| GET | `/config` | 200 | Текущая конфигурация |
| POST | `/config` | 200 | Частичное обновление конфигурации |
| GET | `/sensor` | 200 | Текущие показания датчиков |
| POST | `/sensor` | 200 | Принять нагрузку датчика (отбрасывается) |
| GET | `/sd/status` | 200 | Статус SD-карты |
| POST | `/sd/write` | 200 | Записать тело запроса на SD-карту |
| GET | `/logs` | 200 | Последние строки лога |
| GET | `/wifi/scan` | 202 | Запустить асинхронное сканирование |
| GET | `/wifi/scan/results` | 200 | Результаты сканирования |
| POST | `/wifi/connect` | 200 | Сохранить учётные данные STA и подключиться |
| GET | `/wifi/status` | 200 | Состояние подключения STA |
| POST | `/wifi/disconnect` | 200 | Разорвать связь STA и забыть учётные данные |
| POST | `/system/reboot` | 200 | Перезагрузить устройство |
| POST | `/system/reset` | 200 | Стереть NVS и перезагрузить |
| GET | `/*` | 200 | Отдача статики из SPIFFS (catch-all) |

## Коды ошибок и статусы

### HTTP-статусы

| Статус | Когда возвращается |
|--------|--------------------|
| `200 OK` | Успех |
| `202 Accepted` | Асинхронная операция запущена (сканирование Wi-Fi) |
| `400 Bad Request` | Некорректный JSON, недопустимые значения, пустое тело |
| `403 Forbidden` | Кросс-доменный запрос к `/system/*` |
| `404 Not Found` | Ресурс не найден (статика; корень с `Accept: application/json`) |
| `500 Internal Server Error` | Сбой ввода-вывода / сериализации |
| `503 Service Unavailable` | Подсистема отключена (например SD) или SPIFFS не смонтирован |

### Машиночитаемые коды в поле `"error"`

Ошибки валидации `POST /config` возвращают `{"error":"<код>: описание"}`:

| Код | Значение |
|-----|----------|
| `PIN_INVALID` | Номер GPIO вне диапазона (0–22) или пустая конфигурация |
| `PIN_NO_CAPABILITY` | GPIO не поддерживает требуемое направление / open-drain |
| `PIN_USB_CONFLICT` | GPIO 12/13 зарезервированы под консоль USB Serial/JTAG |
| `PIN_DUPLICATE` | Один GPIO назначен двум ролям |
| `PIN_I2C_CONFLICT` | I2C SDA и SCL — один и тот же GPIO |
| `I2C_CONFLICT` | Одновременно включены INA219 и INA226, либо SHT21 и INA на `0x40` |
| `ADDR_INVALID` | Адрес INA вне диапазона `0x40`–`0x4F` |
| `PORT_INVALID` | Порт telnet вне диапазона `1`–`65535` |
| `PORT_CONFLICT` | Оба telnet-моста включены на одном порту |
| `UART_INVALID` | Telnet-мост включён при выключенном UART |
| `MQTT_INVALID` | MQTT включён без непустого URL брокера (≤255 символов) |

Помимо кодов, `POST /config` может вернуть `400` с текстовым `"error"`:
`Invalid JSON`, `Failed to read request body`, `No valid configuration fields
provided`, `Numeric value out of range`, `String too long (...)`.

---

## `GET /config`

Возвращает текущую конфигурацию. Дополнительные сведения — в
[`configuration.md`](configuration.md).

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
    "sdspi_cs": 10, "sdspi_mosi": 11, "sdspi_miso": 18, "sdspi_sclk": 19,
    "i2c_sda": 6,   "i2c_scl": 7,
    "sensor_1": 8,  "sensor_2": 9,
    "led_red": 14,  "led_green": 15
  }
}
```

- Адреса INA возвращаются в десятичном виде (`64` = `0x40`).
- Пароль AP (`wifi_password`) намеренно не выдаётся.

## `POST /config`

Частичное обновление: изменяются только поля, присутствующие в теле запроса.
Новая конфигурация сохраняется в NVS и вступает в силу **после перезагрузки**.

- `Content-Type: application/json`
- Максимальный размер тела: 4096 байт.

Успех (`200 OK`):

```json
{"message":"Configuration saved. Reboot to apply."}
```

Пример: включить I2C и датчики.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"i2c_enable":1,"sht21_enable":1,"ina219_enable":1,"ina219_addr":64}'
```

Пример: переназначить пины.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"pins":{"led_red":20,"led_green":21}}'
```

Пример: включить MQTT.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"mqtt_enable":1,"mqtt_broker_url":"mqtt://192.168.1.10:1883","mqtt_topic_prefix":"gateway"}'
```

Ошибка валидации:

```json
{"error":"PIN_USB_CONFLICT: sdspi_miso GPIO12 reserved for USB Serial/JTAG console"}
```

Правила длины строк: `wifi_ssid` ≤ 31, `wifi_password` ≤ 63, `server_url` ≤ 255,
`mqtt_broker_url` ≤ 255, `mqtt_topic_prefix` ≤ 64.

## `GET /sensor`

Текущие показания датчиков. Значения — целочисленные с фиксированной запятой;
поля без датчика заполняются симулированными значениями.

```bash
curl http://192.168.4.1/sensor
```

```json
{
  "temperature": 253,
  "humidity": 605,
  "pressure": 1013,
  "light_level": 500,
  "bus_voltage_mv": 3300,
  "shunt_voltage_uv": 1250,
  "current_ma": 150,
  "power_mw": 495
}
```

| Поле | Единица | Множитель | Источник |
|------|---------|-----------|----------|
| `temperature` | 0.1 °C | `253` = 25.3 °C | SHT21 |
| `humidity` | 0.1 %RH | `605` = 60.5 % | SHT21 |
| `pressure` | гПа | симулировано | — |
| `light_level` | люкс | симулировано | — |
| `bus_voltage_mv` | мВ | `3300` = 3.300 В | INA219/INA226 |
| `shunt_voltage_uv` | мкВ | `1250` = 1.250 мВ | INA219/INA226 |
| `current_ma` | мА | — | INA219/INA226 |
| `power_mw` | мВт | — | INA219/INA226 |

## `POST /sensor`

Принимает полезную нагрузку датчика (до 512 байт) и **отбрасывает** её —
реального конвейера приёма данных пока нет.

```bash
curl -X POST http://192.168.4.1/sensor \
  -H 'Content-Type: application/json' \
  -d '{"temperature":250}'
```

```json
{"received":true}
```

## `GET /sd/status`

Статус SD-карты.

```bash
curl http://192.168.4.1/sd/status
```

Успех:

```json
{"status":"ready"}
```

Если подсистема SD отключена или карта не смонтирована — `503`:

```json
{"error":"SD disabled"}
```

## `POST /sd/write`

Записывает тело запроса (до 512 байт) в `/sdcard/upload.bin` в режиме
дописывания.

```bash
curl -X POST http://192.168.4.1/sd/write --data-binary 'hello'
```

```json
{"written":5}
```

Возможные ответы:

| Статус | Тело | Причина |
|--------|------|---------|
| `200` | `{"written":N}` | Успешно записано N байт |
| `400` | `{"error":"Empty body"}` | Пустое тело запроса |
| `500` | `{"error":"SD write failed"}` | Ошибка записи |
| `503` | `{"error":"SD disabled"}` | SD отключена или не смонтирована |

## `GET /logs`

Кольцевой буфер последних строк лога ESP-IDF (до 128 строк) в хронологическом
порядке.

```bash
curl http://192.168.4.1/logs
```

```json
{ "logs": ["I (123) main: Starting ESP32-C6 Gateway Firmware", "..."] }
```

## `GET /wifi/scan`

Запускает неблокирующее сканирование сетей и немедленно возвращает `202`. Если
сканирование уже идёт, возвращается тот же ответ.

```bash
curl -i http://192.168.4.1/wifi/scan
```

```http
HTTP/1.1 202 Accepted
```

```json
{"status":"scanning"}
```

При ошибке запуска — `500`:

```json
{"error":"Scan failed"}
```

## `GET /wifi/scan/results`

Результаты последнего сканирования (до 20 сетей, отсортированы по убыванию
RSSI). Во время сканирования возвращается статус `scanning`.

```bash
curl http://192.168.4.1/wifi/scan/results
```

```json
{
  "networks": [
    { "ssid": "MyNetwork", "rssi": -45, "authmode": 3, "channel": 6 },
    { "ssid": "Guest",     "rssi": -72, "authmode": 0, "channel": 1 }
  ]
}
```

Во время сканирования:

```json
{"status":"scanning"}
```

Если результатов ещё нет:

```json
{"networks":[]}
```

## `POST /wifi/connect`

Сохраняет учётные данные станции в NVS и запускает подключение. Пароль может
быть пустым для открытых сетей.

- `Content-Type: application/json`
- Максимальный размер тела: 256 байт.
- Ограничения: `ssid` 1–32 символа, `password` ≤ 64 символов.

```bash
curl -X POST http://192.168.4.1/wifi/connect \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"MyNetwork","password":"secret"}'
```

```json
{"status":"connecting"}
```

Ошибки:

| Статус | Тело | Причина |
|--------|------|---------|
| `400` | `{"error":"Empty body"}` | Пустое тело |
| `400` | `{"error":"Invalid JSON"}` | Некорректный JSON |
| `400` | `{"error":"Invalid SSID or password"}` | Отсутствует/пустой SSID или превышена длина |
| `500` | `{"error":"Failed to start connection"}` | Не удалось запустить подключение |

## `GET /wifi/status`

Текущее состояние станции.

```bash
curl http://192.168.4.1/wifi/status
```

```json
{
  "connected": true,
  "ssid": "MyNetwork",
  "ip": "192.168.1.42",
  "rssi": -51,
  "state": "connected"
}
```

Поле `state` может принимать значения: `idle`, `connecting`, `connected`,
`disconnected`, `error`.

## `POST /wifi/disconnect`

Разрывает соединение станции и удаляет сохранённые учётные данные из NVS.

```bash
curl -X POST http://192.168.4.1/wifi/disconnect
```

```json
{"status":"idle"}
```

## `POST /system/reboot`

Подтверждает и перезагружает устройство. Ответ отправляется **до** перезапуска.

```bash
curl -X POST http://192.168.4.1/system/reboot
```

```json
{"status":"rebooting"}
```

## `POST /system/reset`

Стирает раздел NVS (сброс к заводским настройкам) и перезагружается. Ответ
отправляется **до** перезапуска.

```bash
curl -X POST http://192.168.4.1/system/reset
```

```json
{"status":"resetting"}
```

### CSRF-защита

`/system/reboot` и `/system/reset` сравнивают хост из заголовка `Origin` с
заголовком `Host`. При несовпадении возвращается `403`:

```json
{"error":"Cross-origin request rejected"}
```

Запросы без заголовка `Origin` (например `curl`) разрешены, так как эндпоинт
доступен только в локальной сети.

## Статика `GET /*`

Обработчик catch-all отдаёт файлы из SPIFFS (`.html`, `.css`, `.js`, `.json`,
`.svg`, `.png`, `.ico`, `.txt`; иначе `application/octet-stream`), с заголовком
`Cache-Control: no-cache`.

- `GET /` отдаёт `index.html` (SPA), если клиент не просит JSON. С
  `Accept: application/json` корень возвращает `404` `{"error":"Not Found"}`.
- Попытки path traversal (`..`) отклоняются с `400`.
- Если SPIFFS не смонтирован — `503 Web UI storage not mounted`.
- Отсутствующий файл — `404`.

## Заметки о таймаутах и сокетах

- HTTP-сервер: 7 сокетов, backlog 16, recv/send таймауты 10 с.
- Тело запроса `/config` ограничено 4096 байтами; превышение обрезается.
- При перезагрузке/сбросе перед `esp_restart()` выдерживается задержка 300 мс,
  чтобы ответ успел уйти клиенту.

## См. также

- [`configuration.md`](configuration.md) — параметры и валидация
- [`architecture.md`](architecture.md) — устройство подсистем
- [`hardware.md`](hardware.md) — распиновка и подключение