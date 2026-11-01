# Справочник REST API

Подробный справочник HTTP REST API ESP32-C6 Gateway. Все эндпоинты отвечают
JSON, если не указано иное.

- **Базовый URL:** `http://192.168.4.1` (адрес SoftAP по умолчанию)
- **Порт:** 80
- **Аутентификация:** cookie-сессии (`POST /auth/login`, по умолчанию
  `admin` / `admin`). Все эндпоинты, кроме `/auth/*` и статики, требуют сессию
  и отвечают `401` без неё. Устройство рассчитано на доверенную локальную сеть
  (SoftAP или ваша собственная сеть Wi-Fi). **Не выставляйте устройство в
  открытый интернет.**
- **CSRF:** разрушающие эндпоинты `/system/*`, `/ext/power`, `/ext/reset`,
  `/ota/check` и `/ota/update` проверяют заголовок `Origin` и отклоняют
  кросс-доменные запросы (`403`). Запросы без `Origin` (например `curl`)
  разрешены.

## Сводная таблица эндпоинтов

| Метод | Путь | Успех | Краткое описание |
|-------|------|-------|------------------|
| GET | `/` | 200 | Веб-интерфейс (SPA из SPIFFS) |
| GET | `/config` | 200 | Текущая конфигурация |
| POST | `/config` | 200 | Частичное обновление конфигурации |
| GET | `/config/export` | 200 | Экспорт настроек в JSON-файл (attachment) |
| GET | `/sensor` | 200 | Текущие показания датчиков |
| POST | `/sensor` | 200 | Принять нагрузку датчика (отбрасывается) |
| GET | `/sd/status` | 200 | Статус SD-карты |
| POST | `/sd/write` | 200 | Записать тело запроса на SD-карту |
| GET | `/sd/list?path=` | 200 | Содержимое каталога SD-карты |
| GET | `/sd/download?path=` | 200 | Скачать файл с SD-карты |
| GET | `/ota/status` | 200 | Состояние OTA и версии прошивки |
| POST | `/ota/check` | 202 | Немедленная проверка манифеста |
| POST | `/ota/update` | 202 | Скачать и установить обновление |
| GET | `/logs` | 200 | Последние строки лога |
| GET | `/wifi/scan` | 202 | Запустить асинхронное сканирование |
| GET | `/wifi/scan/results` | 200 | Результаты сканирования |
| POST | `/wifi/connect` | 200 | Сохранить учётные данные STA и подключиться |
| GET | `/wifi/status` | 200 | Состояние подключения STA |
| POST | `/wifi/disconnect` | 200 | Разорвать связь STA и забыть учётные данные |
| POST | `/system/reboot` | 200 | Перезагрузить устройство |
| POST | `/system/reset` | 200 | Стереть NVS и перезагрузить |
| GET | `/ext/status` | 200 | Состояние внешнего устройства |
| POST | `/ext/power` | 200 | Включить/выключить питание внешнего устройства |
| POST | `/ext/reset` | 200 | Импульс сброса внешнего устройства |
| GET | `/*` | 200 | Отдача статики из SPIFFS (catch-all) |

## Коды ошибок и статусы

### HTTP-статусы

| Статус | Когда возвращается |
|--------|--------------------|
| `200 OK` | Успех |
| `202 Accepted` | Асинхронная операция запущена (сканирование Wi-Fi, проверка/установка OTA) |
| `400 Bad Request` | Некорректный JSON, недопустимые значения, пустое тело, path traversal |
| `401 Unauthorized` | Нет действительной сессии |
| `403 Forbidden` | Кросс-доменный запрос к защищённому POST-эндпоинту |
| `404 Not Found` | Ресурс не найден (статика, файл/каталог SD, корень с `Accept: application/json`) |
| `429 Too Many Requests` | Троттлинг сброса внешнего устройства (реже одного импульса в 2 с) |
| `500 Internal Server Error` | Сбой ввода-вывода / сериализации |
| `503 Service Unavailable` | Подсистема отключена (например SD или внешнее управление) либо SPIFFS не смонтирован |

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
| `EXT_INVALID` | `ext_rst_pulse_ms` вне диапазона 10–5000; `ext_pwr_pol` / `ext_rst_pol` не `0`/`1` |
| `OTA_INVALID` | `ota_check_interval_min` вне диапазона 5–10080 или `ota_url` длиннее 255 символов |

Помимо кодов, `POST /config` может вернуть `400` с текстовым `"error"`:
`Invalid JSON`, `Failed to read request body`, `No valid configuration fields
provided`, `Numeric value out of range`, `String too long (...)`.

---

## Аутентификация

Получите сессионную cookie и передавайте её во всех запросах:

```bash
curl -c cookies.txt -X POST http://192.168.4.1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<пароль из журнала>"}'
curl -b cookies.txt http://192.168.4.1/config
```

Пароль по умолчанию не фиксирован: при первой загрузке генерируется случайный
24-символьный пароль и один раз выводится в консоль USB Serial/JTAG. Сессии
живут 24 часа, cookie помечена `HttpOnly` и `SameSite=Strict`, после 5 неудачных
попыток вход блокируется на 60 секунд (`429` + `Retry-After`). Запросы с
IP-адресов самого устройства (on-boot self-test) считаются доверенными.

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
  "ota_enable": 0,
  "ota_url": "",
  "ota_check_interval_min": 360,
  "ext_pwr_enable": 0,
  "ext_rst_enable": 0,
  "ext_rst_pulse_ms": 100,
  "ext_pwr_pol": 1,
  "ext_rst_pol": 0,
  "ext_pwr_state": 0,
  "pins": {
    "uart0_tx": 16, "uart0_rx": 17,
    "uart1_tx": 4,  "uart1_rx": 5,
    "sdspi_cs": 18, "sdspi_mosi": 1,  "sdspi_miso": 3,  "sdspi_sclk": 0,
    "i2c_sda": 6,   "i2c_scl": 7,
    "sensor_1": -1, "sensor_2": -1,
    "led_red": 14,  "led_green": 15,
    "ext_pwr": 19,  "ext_rst": 20
  }
}
```

- Адреса INA возвращаются в десятичном виде (`64` = `0x40`).
- Пароль AP (`wifi_password`) намеренно не выдаётся.
- Поле `ext_pwr_state` доступно только для чтения: оно меняется лишь через
  `POST /ext/power`.

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

Пример: настроить OTA-сервер.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"ota_enable":1,"ota_url":"http://192.168.1.10:8000","ota_check_interval_min":360}'
```

Правила длины строк: `wifi_ssid` ≤ 31, `wifi_password` ≤ 63, `server_url` ≤ 255,
`mqtt_broker_url` ≤ 255, `mqtt_topic_prefix` ≤ 64, `ota_url` ≤ 255.

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

## `GET /sd/list`

Содержимое одного каталога SD-карты. `path` — путь относительно корня карты
(пусто или `/` — корень). Ответ — JSON-массив; каталоги идут первыми. Вывод
ограничен 128 записями на каталог (в лог пишется предупреждение при усечении).

```bash
curl -b cookies.txt 'http://192.168.4.1/sd/list?path=logs'
```

```json
[
  {"name":"sht21.csv","path":"logs/sht21.csv","is_dir":false,"size":4096},
  {"name":"archive","path":"logs/archive","is_dir":true,"size":0}
]
```

| Статус | Причина |
|--------|---------|
| `200` | Список получен (пустой массив — каталог пуст) |
| `400` | Некорректный путь (`..`, `\`, управляющие символы) |
| `404` | Каталог не найден |
| `503` | SD отключена или не смонтирована |

## `GET /sd/download`

Потоковое скачивание файла (chunked, `application/octet-stream`) с заголовком
`Content-Disposition: attachment; filename="<имя>"`. Сессионная cookie
отправляется браузером автоматически при переходе по ссылке.

```bash
curl -b cookies.txt -OJ 'http://192.168.4.1/sd/download?path=logs/sht21.csv'
```

| Статус | Причина |
|--------|---------|
| `200` | Файл отдаётся потоком |
| `400` | Некорректный путь |
| `404` | Файл не найден (в том числе если `path` указывает на каталог) |
| `503` | SD отключена или не смонтирована |

## `GET /config/export`

Скачивает полный снимок настроек как файл `gateway-config.json`
(`Content-Disposition: attachment`). Секреты (`wifi_password`, `sta_password`)
не включаются — так же, как в `GET /config`. Часов реального времени на
устройстве нет, поэтому `exported_at` — секунды с момента запуска.

```bash
curl -b cookies.txt -OJ http://192.168.4.1/config/export
```

```json
{
  "format": 1,
  "firmware_version": "1.4.0",
  "exported_at": 1234,
  "config": { "baud_rate": 115200, "...": "..." }
}
```

## OTA-обновление

Обновления раздаются по HTTP или HTTPS (`ota_url` обязан начинаться с
`http://` или `https://`). Настройки: `ota_enable`, `ota_url`,
`ota_check_interval_min` (см. `POST /config`). Периодическая проверка только
сообщает о доступной версии; установка выполняется вручную через
`POST /ota/update`.

При `https://` сертификат сервера проверяется по встроенному CA-набору mbedTLS,
автоматические редиректы запрещены. При `http://` SHA-256 манифеста и образа
передаётся по тому же незащищённому каналу и не защищает от MITM — такой режим
допустим только в доверенной локальной сети.

Сервер обновлений должен отдавать манифест `GET {ota_url}/version.json`:

```json
{"version":"1.4.1","filename":"gateway-1.4.1.bin","sha256":"<64 hex>"}
```

Обновление устанавливается, только если версия манифеста **строго больше**
текущей. Перед записью образа проверяется SHA-256; при несовпадении слот не
переключается. Для раздачи манифеста и образа используйте
`tools/ota_server.py` (см. README).

### `GET /ota/status`

```bash
curl -b cookies.txt http://192.168.4.1/ota/status
```

```json
{
  "enabled": true,
  "url": "http://192.168.1.10:8000",
  "check_interval_min": 360,
  "current_version": "1.4.0",
  "available_version": "1.4.1",
  "last_check_s": 1234,
  "last_result": "available",
  "busy": false
}
```

- `available_version` — `null`, если обновлений нет или проверка ещё не
  выполнялась.
- `last_check_s` — время последней попытки в секундах с момента запуска
  (`0` — проверок не было).
- `last_result` — `never`, `up_to_date`, `available`, `updating`, `error` или
  `throttled` (проверки чаще одного раза в 5 секунд игнорируются).

### `POST /ota/check`

Запускает проверку манифеста в фоновой задаче. Требует `auth` и same-origin.

```bash
curl -b cookies.txt -X POST -H 'Content-Type: application/json' \
     -d '{}' http://192.168.4.1/ota/check
```

| Статус | Тело |
|--------|------|
| `202` | `{"message":"OTA check started"}` |
| `403` | cross-origin запрос |
| `503` | `ota_url` не задан |

### `POST /ota/update`

Скачивает и устанавливает более новую версию, затем перезагружается. Ответ
`202` приходит до завершения загрузки; прогресс и результат видны в
`GET /ota/status` (`last_result`: `updating` → перезагрузка). При ошибке
загрузки/SHA-256 текущая прошивка остаётся активной.

```bash
curl -b cookies.txt -X POST -H 'Content-Type: application/json' \
     -d '{}' http://192.168.4.1/ota/update
```

| Статус | Тело |
|--------|------|
| `202` | `{"message":"Update started"}` |
| `403` | cross-origin запрос |
| `503` | `ota_url` не задан |

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
  "state": "connected",
  "attempts": 3,
  "fail_streak": 0,
  "last_disconnect_reason": -1,
  "last_disconnect_reason_name": "none",
  "last_disconnect_age_s": -1,
  "next_retry_in_ms": 0,
  "radio_restarts": 0,
  "power_save": "none"
}
```

Поле `state` может принимать значения: `idle`, `connecting`, `connected`,
`disconnected`, `error`.

Диагностические поля:

| Поле | Описание |
|------|----------|
| `attempts` | Всего попыток подключения с момента загрузки. |
| `fail_streak` | Неудачных попыток подряд; сбрасывается после получения IP. |
| `last_disconnect_reason` | Числовой код причины последнего обрыва (`WIFI_REASON_*`), `-1` — обрывов не было. |
| `last_disconnect_reason_name` | Имя причины (`BEACON_TIMEOUT`, `NO_AP_FOUND`, …) или `none`. |
| `last_disconnect_age_s` | Секунд с последнего обрыва, `-1` — обрывов не было. |
| `next_retry_in_ms` | Миллисекунд до следующей попытки; `0` вне `disconnected`/`error`. |
| `radio_restarts` | Количество мягких рестартов радио (выполняются не чаще раза в 10 минут). |
| `power_save` | Фактический режим Wi-Fi power save: `none`, `min_modem` или `max_modem`. |

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

`/system/reboot`, `/system/reset`, `/ext/power`, `/ext/reset`, `/ota/check` и
`/ota/update` сравнивают хост из заголовка `Origin` с заголовком `Host`. При
несовпадении возвращается `403`:

```json
{"error":"Cross-origin request rejected"}
```

Запросы без заголовка `Origin` (например `curl`) разрешены, так как эндпоинт
доступен только в локальной сети.

## Управление внешним устройством

Три эндпоинта управляют линией питания и сброса подключённого внешнего
устройства (два GPIO, по умолчанию GPIO19/20). Подсистемы включаются отдельно
через `ext_pwr_enable` / `ext_rst_enable` в `POST /config`; по умолчанию обе
выключены.

### `GET /ext/status`

Текущее состояние подсистемы управления внешним устройством.

```bash
curl http://192.168.4.1/ext/status
```

**Ответ (200):**

```json
{
  "power_enabled": true,
  "reset_enabled": true,
  "power_state": false,
  "power_pin": 20,
  "reset_pin": 21,
  "power_polarity": "active-high",
  "reset_polarity": "active-low",
  "reset_pulse_ms": 100
}
```

- `power_polarity` / `reset_polarity` — строки `"active-high"` или
  `"active-low"`.
- `power_state` отражает состояние, сохранённое в NVS
  (`ext_pwr_state`), и всегда равно `false`, пока подсистема питания
  выключена.

### `POST /ext/power`

Переключает питание внешнего устройства. Новое состояние сразу пишется в NVS
(`ext_pwr_st`) и восстанавливается при следующей загрузке.

**Тело запроса:**

```json
{"on": true}
```

**Ответ (200):**

```json
{"power_state": true, "message": "Power ON"}
```

**Ошибки:**

- `400 Bad Request` — отсутствует поле `on` или оно не boolean
- `403 Forbidden` — cross-origin запрос (CSRF-защита)
- `503 Service Unavailable` — подсистема `ext_pwr_enable` выключена

### `POST /ext/reset`

Отправляет импульс сброса на внешнее устройство заданной длительности
(`ext_rst_pulse_ms`). Вызов блокирует HTTP-обработчик на время импульса
(до 5000 мс).

**Тело запроса:** игнорируется (допустимо `{}`)

```bash
curl -X POST http://192.168.4.1/ext/reset
```

**Ответ (200):**

```json
{"message": "Reset pulse sent (100ms)"}
```

**Ошибки:**

- `403 Forbidden` — cross-origin запрос
- `429 Too Many Requests` — троттлинг (менее 2 с с последнего сброса)
- `503 Service Unavailable` — подсистема `ext_rst_enable` выключена

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

- HTTP-сервер: 10 сокетов, backlog 16, recv/send таймауты 10 с. Пул сокетов
  LwIP поднят до 20 (HTTP + telnet + MQTT + OTA/SD-скачивание).
- Тело запроса `/config` ограничено 4096 байтами; превышение обрезается.
- При перезагрузке/сбросе перед `esp_restart()` выдерживается задержка 300 мс,
  чтобы ответ успел уйти клиенту.

## См. также

- [`configuration.md`](configuration.md) — параметры и валидация
- [`architecture.md`](architecture.md) — устройство подсистем
- [`hardware.md`](hardware.md) — распиновка и подключение