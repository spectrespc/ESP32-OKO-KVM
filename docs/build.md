# Сборка и прошивка

Документ описывает установку ESP-IDF, настройку инструментальных цепочек,
сборку, прошивку и мониторинг, а также устранение типовых проблем.

## Требования

- Linux (проверено на Linux) с установленными `git`, `cmake`, `ninja`, `python3`.
- **ESP-IDF v6.0.2**.
- Инструментальные цепочки ESP-IDF RISC-V (target `esp32c6`).
- Python 3 с окружением ESP-IDF.

## Шаг 1. Установка ESP-IDF v6.0.2

```bash
# Установка в ~/.espressif (IDF_TOOLS_PATH по умолчанию)
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf-v6.0.2
cd ~/esp-idf-v6.0.2
./install.sh esp32c6
```

`./install.sh esp32c6` устанавливает toolchain и Python-зависимости. Если вы
используете собственные пути, запомните их для переменных `IDF_PATH_OVERRIDE`
и `IDF_TOOLS_PATH`.

### Активация окружения

Классический способ IDF:

```bash
. ~/esp-idf-v6.0.2/export.sh
```

> `build_and_flash.sh` **не** использует `export.sh`: он настраивает окружение
> вручную, так как `export.sh` падает, если встроенный venv неполон, а также
> из-за возможных ограничений песочницы на `source`/`idf.py`.

## Шаг 2. Настройка toolchain-ов

`build_and_flash.sh` автоматически добавляет в `PATH`:

- RISC-V компилятор:
  `$IDF_TOOLS_PATH/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin`
- Xtensa компилятор (если присутствует):
  `$IDF_TOOLS_PATH/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin`
- GDB (для декодирования паник в мониторе).

Python venv ожидается по пути
`$IDF_TOOLS_PATH/python_env/idf6.0_py3.11_env`; при его отсутствии скрипт
создаёт venv и доустанавливает зависимости из
`$IDF_PATH/tools/requirements/requirements.core.txt`.

## Шаг 3. Сборка

### Через `idf.py` (окружение уже активировано)

```bash
idf.py set-target esp32c6
idf.py build
```

### Через `build_and_flash.sh` (рекомендуется)

```bash
./build_and_flash.sh build
```

Команда `build`:

1. настраивает окружение и venv;
2. удаляет старые `build/`, `sdkconfig`, `sdkconfig.old`;
3. выполняет `idf.py set-target esp32c6`;
4. выполняет `idf.py build`;
5. печатает пути к собранным `*.bin` и `*.elf`.

Во время конфигурации CMake автоматически:

- генерирует `http_tests_generated.h` из `tests/http_endpoints.csv`;
- патчит `fatfs/src/ffconf.h` (`FF_FS_EXFAT 0 → 1`) — IDF v6.0.2 не даёт опции
  Kconfig для exFAT;
- собирает образ SPIFFS из `web/` и включает его в проект для прошивки.

> Первая сборка требует доступа к сети: `espressif/cjson` и `espressif/mqtt`
> подтягиваются из ESP Component Registry и кешируются в `managed_components/`
> (этот каталог коммитится для воспроизводимости).

## Шаг 4. Прошивка

```bash
./build_and_flash.sh flash
# или
idf.py -p /dev/ttyACM0 flash
```

Скрипт проверяет:

- наличие устройства (`/dev/ttyACM0` по умолчанию);
- права на чтение/запись (иначе подскажет `sudo chmod 666 /dev/ttyACM0`).

Устройство во время прошивки обычно переходит в режим загрузчика автоматически
(USB Serial/JTAG). Если автоматический сброс не срабатывает, войдите в режим
загрузки вручную (удерживая BOOT при сбросе).

## Шаг 5. Мониторинг

```bash
./build_and_flash.sh monitor
# или
idf.py -p /dev/ttyACM0 monitor
```

Монитор требует интерактивного TTY. Для выхода — `Ctrl+]`.

При загрузке в консоль выводятся этапы инициализации и результаты self-test
(`SELF-TEST PASS/FAIL` и итоговая строка).

## Использование `build_and_flash.sh` с переменными окружения

| Переменная | Назначение | Значение по умолчанию |
|------------|------------|------------------------|
| `IDF_PATH_OVERRIDE` | Путь к ESP-IDF | `/home/spectre/esp-idf-v6.0.2` |
| `IDF_TOOLS_PATH` | Каталог инструментов | `$HOME/.espressif` |
| `PORT` | Последовательный порт | `/dev/ttyACM0` |
| `TEST_HOST` | Хост для `test` | `192.168.4.1` |
| `IDF_TARGET` | Target сборки | `esp32c6` |

Примеры:

```bash
# Прошивка на другой порт
PORT=/dev/ttyUSB0 ./build_and_flash.sh flash

# Тестирование на другом хосте (хост должен быть в SoftAP)
TEST_HOST=192.168.4.1 ./build_and_flash.sh test

# Нестандартный путь к IDF
IDF_PATH_OVERRIDE=/opt/esp-idf ./build_and_flash.sh build

# Показать разрешённое окружение
./build_and_flash.sh env
```

Полный список команд:

```bash
./build_and_flash.sh build     # чистая сборка
./build_and_flash.sh flash     # прошивка
./build_and_flash.sh monitor   # монитор
./build_and_flash.sh all       # build + flash + monitor
./build_and_flash.sh test      # матрица HTTP-эндпоинтов
./build_and_flash.sh clean     # удалить build/ sdkconfig sdkconfig.old
./build_and_flash.sh env       # печать окружения
./build_and_flash.sh help      # справка
```

## Тестирование после прошивки

1. Подключите хост к SoftAP `ESP32-Gateway` (пароль `gateway123`).
2. Запустите матрицу HTTP-тестов:

   ```bash
   ./build_and_flash.sh test
   ```

   Тесты читаются из `tests/http_endpoints.csv` и выполняются через `curl`;
   в конце печатается итог `Summary: N passed, M failed`.

## Устранение типовых проблем

### `ERROR: IDF not found at ... (missing tools/idf.py)`
Неверный `IDF_PATH`. Установите правильный путь через `IDF_PATH_OVERRIDE`.

### `ERROR: toolchain cc1 missing; re-extract ...`
Повреждена распаковка toolchain (отсутствует `cc1`). Переустановите RISC-V
инструменты (`./install.sh esp32c6` в каталоге IDF или повторная распаковка
архива из `$IDF_TOOLS_PATH/dist`).

### `ERROR: serial device /dev/ttyACM0 not found`
Устройство не подключено или не определено. Проверьте `ls /dev/ttyACM*`,
кабель и драйверы; при необходимости задайте `PORT`.

### `ERROR: no read/write permission on /dev/ttyACM0`
Нет прав на порт. Выполните `sudo chmod 666 /dev/ttyACM0` (или добавьте
пользователя в группу `dialout`).

### `ERROR: monitor needs an interactive TTY`
`monitor` запущен без интерактивного терминала. Запускайте команду из обычного
терминала, а не из pipe/скрипта.

### Первая сборка не находит компоненты
Нет доступа к ESP Component Registry. Проверьте сеть; после успешной загрузки
компоненты кешируются в `managed_components/`.

### Предупреждение `fatfs ffconf.h not found ... exFAT support not enabled`
FatFs не найден при конфигурации (необычная установка IDF). exFAT будет
недоступен; SD-карту отформатируйте в FAT32.

### Сборка падает после смены target
Выполните `./build_and_flash.sh clean` (удаляет `build/`, `sdkconfig`,
`sdkconfig.old`) и пересоберите.

### Паника после сборки (не декодируется)
Убедитесь, что в `PATH` доступен `riscv32-esp-elf-gdb` — его добавляет
`build_and_flash.sh`; для ручного `idf.py monitor` активируйте окружение IDF.

## См. также

- [`architecture.md`](architecture.md) — устройство подсистем
- [`configuration.md`](configuration.md) — конфигурация
- [`hardware.md`](hardware.md) — распиновка
- [`api.md`](api.md) — REST API