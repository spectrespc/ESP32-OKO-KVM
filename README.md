# ESP32-C6 Gateway Firmware

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.2-blue)](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/)
[![Target](https://img.shields.io/badge/target-ESP32--C6-red)](https://www.espressif.com/en/products/socs/esp32-c6)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)

> **Язык / Language:** русская документация идёт первой, английская — после
> разделителя `---` (English documentation follows the `---` separator).

---

# ESP32-C6 Gateway — документация (RU)

Самодостаточная прошивка IoT-шлюза для ESP32-C6. Предоставляет REST API и
одностраничную веб-консоль по Wi-Fi, ретранслирует UART-линию датчиков в TCP,
пишет на SD-карту и опрашивает I2C-датчики температуры/влажности и контроля
питания.

Дополнительные руководства:

- [`docs/architecture.md`](docs/architecture.md) — архитектура системы
- [`docs/api.md`](docs/api.md) — подробный справочник REST API
- [`docs/hardware.md`](docs/hardware.md) — аппаратное обеспечение и распиновка
- [`docs/build.md`](docs/build.md) — сборка и прошивка
- [`docs/configuration.md`](docs/configuration.md) — справочник конфигурации
- [`CHANGELOG.ru.md`](CHANGELOG.ru.md) — история изменений
- [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md) — руководство для контрибьюторов

## Возможности

- **Два UART** — консоль UART0 через встроенный USB Serial/JTAG (115200) и
  мост датчиков UART1 (9600).
- **SD-карта** по SPI с файловой системой FAT (чтение / запись / список).
- **Wi-Fi AP + STA** — встроенная SoftAP для локального доступа плюс станция,
  которая умеет сканировать и подключаться к внешней сети.
- **HTTP REST API** на порту 80 с JSON-полезной нагрузкой.
- **Веб-интерфейс** — одностраничное приложение, отдаваемое из раздела SPIFFS
  (SD-карта не требуется), с 7 вкладками: Dashboard, Configuration, WiFi,
  Sensors, SD Card, External и System Log.
- **I2C-датчики** — SHT21 (температура / влажность) и INA219 / INA226
  (напряжение шины, напряжение шунта, ток, мощность).
- **Постоянная конфигурация** в NVS с семантикой частичного обновления.
- **Переназначение GPIO на лету** с проверкой возможностей пина и конфликтов.
- **Двойные telnet-мосты**, ретранслирующие TCP в UART0 и UART1, каждый на
  собственном порту.
- **Опциональное логирование на SD-карту**: трафик UART (`uar0.log` / `uar1.log`)
  и показания датчиков (`sht21.csv` / `ina219.csv` / `ina226.csv` / `env.csv`) с
  ротацией по 1 МБ на FatFs с поддержкой exFAT.
- **Публикация датчиков в MQTT** (один JSON-топик на датчик, QoS 0).
- **Управление внешним устройством** — два GPIO для питания (удерживаемый
  уровень) и сброса (импульс заданной длительности), настраиваемая полярность,
  персистентное состояние в NVS, веб-интерфейс с подтверждением операций.
- **Кольцевой буфер системного лога**, доступный через `GET /logs`.
- **Self-test**, проверяющий HTTP-эндпоинты при загрузке.
- **Единый `build_and_flash.sh`** для build / flash / monitor / test / clean.

## Аппаратные требования

- Плата разработки ESP32-C6 с флеш-памятью не менее 4 МБ (например ESP32-C6FH4).
- **Опционально:**
  - модуль microSD на SPI;
  - датчик температуры/влажности SHT21 на I2C;
  - монитор напряжения/тока/мощности INA219 или INA226 на I2C.

Распайка по умолчанию использует пины из таблицы [Распиновка GPIO](#распиновка-gpio);
любой пин можно переназначить во время работы. Полностью распиновка, схемы
подключения датчиков и ограничения описаны в [`docs/hardware.md`](docs/hardware.md).

## Быстрый старт

### Предварительные требования

- Установленный [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/)
  с доступными из `PATH` инструментальными цепочками.
- Python 3 с окружением ESP-IDF Python.

### Сборка

```bash
idf.py set-target esp32c6
idf.py build
```

### Прошивка

```bash
idf.py -p /dev/ttyACM0 flash
```

### Мониторинг

```bash
idf.py -p /dev/ttyACM0 monitor
```

### Использование `build_and_flash.sh`

Скрипт сам настраивает окружение ESP-IDF (без `source export.sh`) и оборачивает
типовые задачи:

```bash
./build_and_flash.sh all      # build + flash + monitor
./build_and_flash.sh build
./build_and_flash.sh flash
./build_and_flash.sh monitor
./build_and_flash.sh test     # запустить матрицу HTTP-эндпоинтов
./build_and_flash.sh clean
./build_and_flash.sh env
./build_and_flash.sh help
```

Переопределения окружения: `IDF_PATH_OVERRIDE`, `IDF_TOOLS_PATH`, `PORT`,
`TEST_HOST`.

## Структура проекта

```
gateway/
├── CMakeLists.txt          # Определение проекта верхнего уровня
├── partitions.csv          # Пользовательская раскладка 4 МБ (2.5M app + 1.5M SPIFFS)
├── sdkconfig.defaults      # Значения SDK по умолчанию (target, console, SPIFFS, ...)
├── build_and_flash.sh      # Канонический скрипт build/flash/monitor/test
├── main/
│   ├── main.c              # app_main(): порядок инициализации подсистем
│   ├── pins.[ch]           # Заводская карта GPIO по умолчанию + инициализация
│   ├── config.[ch]         # app_config_t, загрузка/сохранение/валидация NVS
│   ├── pin_validation.[ch] # Таблица возможностей GPIO ESP32-C6 + проверки
│   ├── uart_handler.[ch]   # Консоль UART0 + мост UART1
│   ├── sd_handler.[ch]     # SD/SPI + файловая система FAT
│   ├── sensor_handler.[ch] # Чтение SHT21 / INA219 / INA226, MQTT
│   ├── http_server.[ch]    # REST-сервер + регистрация эндпоинтов
│   ├── web_handlers.[ch]   # Монтирование SPIFFS + отдача статики
│   ├── log_handler.[ch]    # Кольцевой буфер лога в RAM + GET /logs
│   ├── wifi_ap.[ch]        # Инициализация Wi-Fi (режим AP+STA, SoftAP)
│   ├── wifi_scan.[ch]      # STA: сканирование/подключение/статус/отключение
│   ├── system_handler.[ch] # Обработчики перезагрузки / сброса к заводским
│   ├── self_test.[ch]      # HTTP self-test при загрузке
│   └── telnet_bridge.[ch]  # Ретрансляторы TCP -> UART0/UART1 (+ лог на SD)
├── web/
│   └── index.html          # Самодостаточное SPA (инлайн CSS + JS)
├── tests/
│   └── http_endpoints.csv  # Общая матрица ожиданий HTTP-эндпоинтов
└── tools/
    └── gen_http_tests.py   # Генератор C-заголовка из CSV для self-test
```

## Справочник REST API

Все эндпоинты возвращают JSON, если не указано иное. Аутентификации нет;
устройство рассчитано на доверенную локальную сеть (встроенную SoftAP или вашу
собственную сеть Wi-Fi).

| Метод | Путь                 | Описание                                        | Успех |
|-------|----------------------|-------------------------------------------------|-------|
| GET   | `/`                  | Веб-интерфейс (SPA из SPIFFS)                   | 200   |
| GET   | `/config`            | Текущая конфигурация                            | 200   |
| POST  | `/config`            | Частичное обновление конфигурации               | 200   |
| GET   | `/sensor`            | Текущие показания датчиков                      | 200   |
| POST  | `/sensor`            | Принять полезную нагрузку датчика (отбрасывается)| 200  |
| GET   | `/sd/status`         | Статус SD-карты                                 | 200   |
| POST  | `/sd/write`          | Записать тело запроса на SD-карту               | 200   |
| GET   | `/logs`              | Последние строки лога                           | 200   |
| GET   | `/wifi/scan`         | Запустить асинхронное сканирование Wi-Fi        | 202   |
| GET   | `/wifi/scan/results` | Результаты сканирования (или `scanning`)        | 200   |
| POST  | `/wifi/connect`      | Сохранить учётные данные STA и подключиться     | 200   |
| GET   | `/wifi/status`       | Состояние подключения STA                       | 200   |
| POST  | `/wifi/disconnect`   | Разорвать связь STA и забыть учётные данные     | 200   |
| POST  | `/system/reboot`     | Подтвердить, затем перезагрузить устройство     | 200   |
| POST  | `/system/reset`      | Подтвердить, стереть NVS, затем перезагрузить   | 200   |
| GET   | `/ext/status`        | Состояние внешнего устройства                   | 200   |
| POST  | `/ext/power`         | Включить/выключить питание (`{"on":bool}`)      | 200/503 |
| POST  | `/ext/reset`         | Отправить импульс сброса                        | 200/429/503 |

Ошибки возвращаются с кодами `400 Bad Request` (некорректная нагрузка /
недопустимые значения), `500 Internal Server Error` (сбой ввода-вывода) или
`503 Service Unavailable` (подсистема отключена, например SD).

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
    "sdspi_cs": 10, "sdspi_mosi": 11, "sdspi_miso": 18, "sdspi_sclk": 19,
    "i2c_sda": 6,   "i2c_scl": 7,
    "sensor_1": 8,  "sensor_2": 9,
    "led_red": 14,  "led_green": 15
  }
}
```

Адреса возвращаются в десятичном виде (`64` = `0x40`). Пароль AP
(`wifi_password`) намеренно не выдаётся в `GET /config`.

### `POST /config`

Частичное обновление: изменяются только поля, присутствующие в теле JSON, и
новая конфигурация сохраняется в NVS. При успехе ответ всегда
`{"message":"Configuration saved. Reboot to apply."}` — изменения вступают в силу
после следующей перезагрузки.

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"i2c_enable":1,"sht21_enable":1,"ina219_enable":1,"ina219_addr":64}'
```

Флаги включения — `0`/`1`. Адреса INA должны лежать в диапазоне `0x40`-`0x4F`
(`64`-`79` в десятичном виде). Вложенные обновления пинов передаются в `"pins"`:

```bash
curl -X POST http://192.168.4.1/config \
  -H 'Content-Type: application/json' \
  -d '{"pins":{"led_red":20,"led_green":21}}'
```

Ошибки валидации возвращают `400` с машиночитаемым кодом в поле `"error"`:

| Код | Значение |
|-----|----------|
| `PIN_INVALID` | Номер GPIO вне диапазона для ESP32-C6 (0–22) |
| `PIN_NO_CAPABILITY` | GPIO не может работать в требуемом направлении / нет open-drain |
| `PIN_USB_CONFLICT` | GPIO 12/13 конфликтует с консолью USB Serial/JTAG |
| `PIN_DUPLICATE` | Один GPIO назначен двум ролям |
| `PIN_I2C_CONFLICT` | Пины I2C SDA и SCL совпадают |
| `I2C_CONFLICT` | Одновременно включены оба монитора INA либо конфликт адреса SHT21/INA |
| `ADDR_INVALID` | Адрес INA вне `0x40`-`0x4F` |
| `PORT_INVALID` | `telnet0_port` / `telnet1_port` вне `1`-`65535` |
| `PORT_CONFLICT` | Оба telnet-моста включены на одном порту |
| `UART_INVALID` | Telnet-мост включён, а его UART выключен |
| `MQTT_INVALID` | MQTT включён без (непустого, ≤255 символов) URL брокера |

Помимо кодов выше `POST /config` возвращает `400` с текстовыми сообщениями:
`Invalid JSON`, `No valid configuration fields provided`, `Numeric value out of
range` и `String too long (...)` (см. [`docs/configuration.md`](docs/configuration.md)).

### `GET /sensor`

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

Значения — целочисленные с фиксированной запятой, чтобы сериализоваться без
потерь: температура в 0.1 °C, влажность в 0.1 %RH, напряжение шины в мВ,
напряжение шунта в мкВ, ток в мА и мощность в мВт (`253` = 25.3 °C,
`3300` = 3.300 В). Поля, датчик которых отключён, заполняются
симулированными значениями.

### `GET /logs`

```json
{ "logs": ["I (123) main: Starting ESP32-C6 Gateway Firmware", "..."] }
```

Хронологический кольцевой буфер последних строк лога ESP-IDF в RAM.

### Wi-Fi

```bash
# 1. Запустить сканирование, затем опросить результаты
curl http://192.168.4.1/wifi/scan            # -> 202 {"status":"scanning"}
curl http://192.168.4.1/wifi/scan/results    # -> {"networks":[{"ssid":"...","rssi":-45,"authmode":3,"channel":6}, ...]}

# 2. Подключиться к сети
curl -X POST http://192.168.4.1/wifi/connect \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"MyNetwork","password":"secret"}'

# 3. Проверить состояние / отключиться
curl http://192.168.4.1/wifi/status
curl -X POST http://192.168.4.1/wifi/disconnect
```

`GET /wifi/status` возвращает
`{"connected":true,"ssid":"MyNetwork","ip":"192.168.1.42","rssi":-51,"state":"connected"}`.
`POST /wifi/connect` без поля `ssid` возвращает `400`.

### Управление внешним устройством

```bash
# Состояние подсистемы питания/сброса внешнего устройства
curl http://192.168.4.1/ext/status
curl -X POST http://192.168.4.1/ext/power \
  -H 'Content-Type: application/json' -d '{"on":true}'
curl -X POST http://192.168.4.1/ext/reset
```

`GET /ext/status` возвращает текущее состояние, например
`{"power_enabled":true,"reset_enabled":true,"power_state":false,"power_pin":20,"reset_pin":21,"power_polarity":"active-high","reset_polarity":"active-low","reset_pulse_ms":100}`.
`POST /ext/power` принимает `{"on":true|false}` и отвечает
`{"power_state":true,"message":"Power ON"}`. `POST /ext/reset` отправляет импульс
заданной длительности и отвечает `{"message":"Reset pulse sent (100ms)"}`.
Оба POST-эндпоинта защищены CSRF-проверкой `Origin`, а сброс троттлится — не
чаще одного импульса в 2 секунды (`429`). Если соответствующая подсистема
отключена (`ext_pwr_enable` / `ext_rst_enable` = 0), возвращается `503`. Сброс
выполняется синхронно в HTTP-обработчике и блокирует его на время импульса
(до 5000 мс).

### Система

```bash
curl -X POST http://192.168.4.1/system/reboot   # -> 200, затем перезагрузка
curl -X POST http://192.168.4.1/system/reset    # -> 200, стереть NVS, перезагрузка
```

Оба обработчика отправляют HTTP-ответ **до** перезапуска, поэтому клиент всегда
получает `200`. Это разрушающие эндпоинты: они проверяют заголовок `Origin` и
отклоняют кросс-доменные запросы (запросы без `Origin`, например из `curl`,
проходят) — базовая защита от CSRF.

## Распиновка GPIO

Заводские значения по умолчанию; любое назначение перенастраивается через
`POST /config` и сохраняется в NVS.

| Функция    | GPIO по умолчанию | Направление | Примечания                          |
|------------|-------------------|-------------|-------------------------------------|
| UART0 TX   | 16                | OUT         | Консоль USB Serial/JTAG (115200)    |
| UART0 RX   | 17                | IN          | Консоль USB Serial/JTAG             |
| UART1 TX   | 4                 | OUT         | Мост датчиков (9600)                |
| UART1 RX   | 5                 | IN          | Мост датчиков                       |
| SDSPI CS   | 10                | OUT         | Выбор кристалла SD                  |
| SDSPI MOSI | 11                | OUT         | Данные SD (out)                     |
| SDSPI MISO | 18                | IN          | Данные SD (in)                      |
| SDSPI SCLK | 19                | OUT         | Тактирование SD                     |
| I2C SDA    | 6                 | OD          | Данные шины датчиков                |
| I2C SCL    | 7                 | OD          | Тактирование шины датчиков          |
| Sensor 1   | 8                 | IN/OUT      | Универсальный GPIO (strapping-пин)  |
| Sensor 2   | 9                 | IN/OUT      | Универсальный GPIO (strapping-пин)  |
| LED red    | 14                | OUT         | Статусный светодиод                 |
| LED green  | 15                | OUT         | Статусный светодиод                 |
| Ext Power  | 20                | OUT         | Питание внешнего устройства         |
| Ext Reset  | 21                | OUT         | Сброс внешнего устройства           |

> **Примечание о USB-консоли:** GPIO 12 и 13 — это нативные линии USB D-/D+
> ESP32-C6. Пока активен `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (значение по
> умолчанию), назначать их нельзя, а валидация отклоняет любую попытку
> (`PIN_USB_CONFLICT`). Поэтому заводские значения SDSPI MISO/SCLK — GPIO 18/19,
> а не 12/13. Пины GPIO 2, 8 и 9 являются strapping-пинами и вызывают только
> предупреждение при загрузке.

## Поддержка I2C-датчиков

Шина I2C по умолчанию отключена. Включите её и выберите установленные датчики на
вкладке Configuration или через `POST /config`:

| Датчик  | Флаг NVS     | Адрес                          | Измерения                              |
|---------|--------------|--------------------------------|----------------------------------------|
| SHT21   | `sht21_en`   | фиксированный `0x40`           | температура (0.1 °C), влажность (0.1 %) |
| INA219  | `ina219_en`  | `ina219_adr` (по умолчанию `0x40`) | напряжение шины (мВ), шунт (мкВ), ток (мА), мощность (мВт) |
| INA226  | `ina226_en`  | `ina226_adr` (по умолчанию `0x40`) | напряжение шины (мВ), шунт (мкВ), ток (мА), мощность (мВт) |

Подключение использует пины `i2c_sda` / `i2c_scl` (по умолчанию GPIO 6 / 7).
Шина работает на 100 кГц с внутренними подтяжками и глитч-фильтром. Поскольку
SHT21 и устройства INA делят адрес `0x40` по умолчанию, адреса нужно задавать
вручную. Валидация отклоняет конфигурацию, в которой:

- две роли используют один GPIO (`PIN_DUPLICATE`);
- одновременно включены INA219 и INA226 (единая модель данных датчика) —
  `I2C_CONFLICT`;
- включённое устройство INA конфликтует с включённым SHT21 по адресу `0x40` —
  `I2C_CONFLICT`.

## Wi-Fi

- **Режим AP (всегда включён):** SoftAP с SSID `ESP32-Gateway` и паролем
  `gateway123` (WPA2-PSK, канал 1, до 4 клиентов). Это запасные значения,
  используемые, когда `wifi_ssid` / `wifi_password` не заданы; AP можно
  перепровизионировать через эти поля конфигурации. Пустой пароль делает AP
  открытой точкой. Локальный доступ всегда доступен по адресу
  **http://192.168.4.1**.
- **Режим STA:** станция работает параллельно с AP. Найдите сети на вкладке WiFi
  и подключитесь. Учётные данные STA хранятся отдельно от учётных данных AP в
  NVS (`sta_ssid` / `sta_password`) и восстанавливаются при загрузке.

## Веб-интерфейс

SPA — это единый самодостаточный файл `web/index.html` (инлайн CSS + JS),
встраиваемый в раздел SPIFFS на этапе сборки и отдаваемый обработчиком
статики HTTP-сервера. Откройте **http://192.168.4.1** после подключения к AP.
Доступно семь вкладок: Dashboard, Configuration, WiFi, Sensors, SD Card,
External и System Log. Вкладка Configuration строит выпадающие списки GPIO на
клиенте из таблицы пинов ESP32-C6 и отключает пины, уже назначенные другой роли.

## Конфигурация

Конфигурация хранится в пространстве имён NVS `storage`. Ключи (≤15 символов):

| Ключ            | Тип  | По умолчанию | Примечания                         |
|-----------------|------|--------------|------------------------------------|
| `baud_rate`     | u32  | `115200`     | Скорость консоли UART0             |
| `uart0_enable`  | u8   | `1`          | Консоль UART0 вкл/выкл             |
| `uart1_enable`  | u8   | `1`          | Мост датчиков UART1 вкл/выкл       |
| `sd_enable`     | u8   | `1`          | Подсистема SD-карты вкл/выкл       |
| `sensor_enable` | u8   | `1`          | Подсистема датчиков вкл/выкл       |
| `led_enable`    | u8   | `1`          | Статусные светодиоды вкл/выкл      |
| `telnet0_en`    | u8   | `0`          | Telnet-мост → UART0 вкл/выкл       |
| `telnet0_port`  | u16  | `2323`       | TCP-порт моста UART0 (1-65535)     |
| `telnet1_en`    | u8   | `0`          | Telnet-мост → UART1 вкл/выкл       |
| `telnet1_port`  | u16  | `23`         | TCP-порт моста UART1 (1-65535)     |
| `u0_sd_log`     | u8   | `0`          | Логировать трафик UART0 в `/sdcard/uar0.log` |
| `u1_sd_log`     | u8   | `0`          | Логировать трафик UART1 в `/sdcard/uar1.log` |
| `sht21_sd`      | u8   | `0`          | Писать строки SHT21 в `/sdcard/sht21.csv`  |
| `ina219_sd`     | u8   | `0`          | Писать строки INA219 в `/sdcard/ina219.csv` |
| `ina226_sd`     | u8   | `0`          | Писать строки INA226 в `/sdcard/ina226.csv` |
| `env_sd`        | u8   | `0`          | Писать давление/освещённость в `/sdcard/env.csv` |
| `i2c_en`        | u8   | `0`          | Шина I2C вкл/выкл                  |
| `sht21_en`      | u8   | `0`          | SHT21 вкл/выкл                     |
| `ina219_en`     | u8   | `0`          | INA219 вкл/выкл                    |
| `ina226_en`     | u8   | `0`          | INA226 вкл/выкл                    |
| `ina219_adr`    | u8   | `0x40`       | I2C-адрес INA219 (0x40-0x4F)       |
| `ina226_adr`    | u8   | `0x40`       | I2C-адрес INA226 (0x40-0x4F)       |
| `pin_<роль>`    | u32  | см. распиновку | Один ключ на роль GPIO           |
| `wifi_ssid`     | str  | (нет)        | Переопределение SSID SoftAP        |
| `wifi_password` | str  | (нет)        | Переопределение пароля SoftAP      |
| `server_url`    | str  | (нет)        | Опциональный URL вышестоящего сервера |
| `mqtt_en`       | u8   | `0`          | Публикация в MQTT вкл/выкл         |
| `mqtt_url`      | str  | (нет)        | URL брокера (`mqtt://` или `mqtts://`) |
| `mqtt_topic`    | str  | (нет)        | Префикс топика (по умолчанию `gateway`) |
| `ext_pwr_en`    | u8   | `0`          | Управление питанием внешнего устройства вкл/выкл |
| `ext_rst_en`    | u8   | `0`          | Управление сбросом внешнего устройства вкл/выкл  |
| `ext_pwr_pol`   | u8   | `1`          | Полярность питания (0=active-low, 1=active-high) |
| `ext_rst_pol`   | u8   | `0`          | Полярность сброса (0=active-low, 1=active-high)  |
| `ext_rst_ms`    | u32  | `100`        | Длительность импульса сброса, мс (10-5000)       |
| `ext_pwr_st`    | u8   | `0`          | Состояние питания (0=OFF, 1=ON, персистентно)    |
| `sta_ssid`      | str  | (нет)        | Сохранённый SSID станции (вкладка WiFi) |
| `sta_password`  | str  | (нет)        | Сохранённый пароль станции         |

Изменения `POST /config` пишутся в NVS немедленно, но **требуют перезагрузки для
применения**. Используйте `POST /system/reboot` (или `POST /system/reset` для
заводских значений). Подробная таблица, правила валидации и миграция устаревших
ключей телнета описаны в [`docs/configuration.md`](docs/configuration.md).

## Скрипты сборки

- **`build_and_flash.sh`** — канонический враппер. Команды: `build`, `flash`,
  `monitor`, `all`, `test`, `clean`, `env`, `help`. Он сам настраивает путь к
  ESP-IDF, инструментальные цепочки и Python venv, поэтому работает там, где
  `idf.py` отсутствует в `PATH`.
- **`tools/gen_http_tests.py`** — генерирует таблицу ожиданий self-test из
  `tests/http_endpoints.csv`. Вызывается автоматически CMake во время сборки.

## Тестирование

- **Self-test:** выполняется автоматически при загрузке через внутренний AP и
  пишет результаты в консоль. Таблица ожиданий генерируется из
  `tests/http_endpoints.csv`; разрушающие эндпоинты `/system/*` и `/ext/reset`
  пропускаются, чтобы устройство не перезагружалось и не дёргало внешнее
  устройство посреди теста.
- **Матрица HTTP-эндпоинтов:** `./build_and_flash.sh test` выполняет каждую
  строку `tests/http_endpoints.csv` через `curl` по адресу `TEST_HOST`
  (по умолчанию `192.168.4.1`). Хост должен быть подключён к SoftAP.

Оба потребителя используют один и тот же CSV, поэтому тесты на устройстве и на
хосте остаются синхронными.

## Лицензия

Лицензировано под [Apache License 2.0](LICENSE).

---

# ESP32-C6 Gateway Firmware (EN)

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
  (no SD card required), with 7 tabs: Dashboard, Configuration, WiFi, Sensors,
  SD Card, External and System Log.
- **I2C sensors** — SHT21 (temperature / humidity) and INA219 / INA226
  (bus voltage, shunt voltage, current, power).
- **Persistent configuration** in NVS with partial-update semantics.
- **Runtime GPIO reassignment** with capability and conflict validation.
- **Dual telnet bridges** relaying TCP to UART0 and UART1, each on its own port.
- **Optional SD-card logging** of UART traffic (`uar0.log` / `uar1.log`) and
  per-sensor readings (`sht21.csv` / `ina219.csv` / `ina226.csv` / `env.csv`),
  with 1 MB rotation, on an exFAT-capable FatFs.
- **MQTT publishing** of sensor readings (one JSON topic per sensor, QoS 0).
- **External device control** — two GPIOs for power (held level) and reset
  (pulse of a configurable width), configurable polarity, power state persisted
  in NVS, and a web UI with operation confirmation.
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
| GET    | `/ext/status`        | External device power/reset state              | 200     |
| POST   | `/ext/power`         | Turn external power on/off (`{"on":bool}`)     | 200/503 |
| POST   | `/ext/reset`         | Send a reset pulse to the external device      | 200/429/503 |

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
    "sdspi_cs": 10, "sdspi_mosi": 11, "sdspi_miso": 18, "sdspi_sclk": 19,
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
| `PIN_INVALID` | GPIO number out of range for the ESP32-C6 (0-22) |
| `PIN_NO_CAPABILITY` | GPIO cannot act in the requested direction / lacks open-drain |
| `PIN_USB_CONFLICT` | GPIO 12/13 conflict with the USB Serial/JTAG console |
| `PIN_DUPLICATE` | Two roles assigned to the same GPIO |
| `PIN_I2C_CONFLICT` | I2C SDA and SCL are the same GPIO |
| `I2C_CONFLICT` | Both INA monitors enabled, or SHT21/INA address clash |
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
  "bus_voltage_mv": 3300,
  "shunt_voltage_uv": 1250,
  "current_ma": 150,
  "power_mw": 495
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

### External Device Control

```bash
# Read the power/reset subsystem state
curl http://192.168.4.1/ext/status
curl -X POST http://192.168.4.1/ext/power \
  -H 'Content-Type: application/json' -d '{"on":true}'
curl -X POST http://192.168.4.1/ext/reset
```

`GET /ext/status` returns the current state, e.g.
`{"power_enabled":true,"reset_enabled":true,"power_state":false,"power_pin":20,"reset_pin":21,"power_polarity":"active-high","reset_polarity":"active-low","reset_pulse_ms":100}`.
`POST /ext/power` takes `{"on":true|false}` and replies
`{"power_state":true,"message":"Power ON"}`. `POST /ext/reset` drives a reset
pulse for the configured width and replies
`{"message":"Reset pulse sent (100ms)"}`. Both POST endpoints share the
same-origin CSRF guard, and reset is throttled to one pulse every 2 seconds
(`429`). When the matching subsystem is disabled (`ext_pwr_enable` /
`ext_rst_enable` = 0) the endpoint returns `503`. The reset runs synchronously
in the HTTP handler and blocks it for the pulse width (up to 5000 ms).

### System

```bash
curl -X POST http://192.168.4.1/system/reboot   # -> 200, then reboot
curl -X POST http://192.168.4.1/system/reset    # -> 200, erase NVS, reboot
```

Both handlers send the HTTP response **before** restarting so the client always
receives a `200`. These destructive endpoints check the `Origin` header and
reject cross-origin requests (requests without an `Origin`, e.g. from `curl`,
pass) — a basic CSRF guard.

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
| SDSPI MISO  | 18           | IN        | SD data in                         |
| SDSPI SCLK  | 19           | OUT       | SD clock                           |
| I2C SDA     | 6            | OD        | Sensor bus data                    |
| I2C SCL     | 7            | OD        | Sensor bus clock                   |
| Sensor 1    | 8            | IN/OUT    | Generic GPIO (strapping pin)       |
| Sensor 2    | 9            | IN/OUT    | Generic GPIO (strapping pin)       |
| LED red     | 14           | OUT       | Status LED                         |
| LED green   | 15           | OUT       | Status LED                         |
| Ext Power   | 20           | OUT       | External device power              |
| Ext Reset   | 21           | OUT       | External device reset              |

> **USB console note:** GPIO 12 and 13 are the ESP32-C6 native USB D-/D+ lines.
> While `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (the default) they cannot be used
> as GPIO, and pin validation rejects any assignment to them
> (`PIN_USB_CONFLICT`). The factory SDSPI MISO/SCLK defaults are therefore GPIO
> 18/19, not 12/13. GPIO 2, 8 and 9 are strapping pins and only produce a
> warning.

## I2C Sensor Support

The I2C bus is disabled by default. Enable it and select the populated sensors
in the Configuration tab or via `POST /config`:

| Sensor  | NVS flag     | Address                        | Measurements                          |
|---------|--------------|--------------------------------|---------------------------------------|
| SHT21   | `sht21_en`   | fixed `0x40`                   | temperature (0.1 °C), humidity (0.1 %) |
| INA219  | `ina219_en`  | `ina219_adr` (default `0x40`)  | bus V (mV), shunt V (µV), I (mA), P (mW) |
| INA226  | `ina226_en`  | `ina226_adr` (default `0x40`)  | bus V (mV), shunt V (µV), I (mA), P (mW) |

Wiring uses the `i2c_sda` / `i2c_scl` pins (default GPIO 6 / 7). The bus runs at
100 kHz with internal pull-ups and a glitch filter. Because SHT21 and the INA
devices share the `0x40` default, addresses must be set manually. Validation
rejects a configuration where:

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
handler. Open **http://192.168.4.1** after joining the AP. It has seven tabs:
Dashboard, Configuration, WiFi, Sensors, SD Card, External and System Log. The
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
| `ext_pwr_en`    | u8   | `0`          | External power control on/off       |
| `ext_rst_en`    | u8   | `0`          | External reset control on/off       |
| `ext_pwr_pol`   | u8   | `1`          | Power polarity (0=active-low, 1=active-high) |
| `ext_rst_pol`   | u8   | `0`          | Reset polarity (0=active-low, 1=active-high) |
| `ext_rst_ms`    | u32  | `100`        | Reset pulse width, ms (10-5000)     |
| `ext_pwr_st`    | u8   | `0`          | Persisted power state (0=OFF, 1=ON) |
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
  `tests/http_endpoints.csv`; destructive `/system/*` and `/ext/reset` endpoints
  are skipped so the device does not reboot and the external device is not
  disturbed mid-test.
- **HTTP endpoint matrix:** `./build_and_flash.sh test` runs every row of
  `tests/http_endpoints.csv` with `curl` against `TEST_HOST`
  (default `192.168.4.1`). The host must be joined to the SoftAP.

Both consumers share the same CSV, so the on-device and host-side tests stay in
sync.

## License

Licensed under the [Apache License 2.0](LICENSE).