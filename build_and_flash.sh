#!/bin/bash
#
# Canonical build / flash / monitor / test script for the ESP32-C6 Gateway.
#
# The ESP-IDF environment is configured manually (no `source export.sh`),
# because export.sh fails before its fallback when the embedded venv is
# incomplete, and because `source`/`idf.py` may be blocked by the sandbox.
#
# Usage:
#   ./build_and_flash.sh build     - clean, set target, build firmware
#   ./build_and_flash.sh flash     - flash firmware to /dev/ttyACM0
#   ./build_and_flash.sh monitor   - open serial monitor (interactive)
#   ./build_and_flash.sh all       - build + flash + monitor
#   ./build_and_flash.sh test      - run HTTP endpoint tests
#   ./build_and_flash.sh clean     - remove build/ sdkconfig sdkconfig.old
#   ./build_and_flash.sh env       - print resolved environment
#   ./build_and_flash.sh help      - show this help
#
# Environment overrides:
#   IDF_PATH_OVERRIDE, IDF_TOOLS_PATH, PORT, TEST_HOST

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Defaults -----------------------------------------------------------------
IDF_PATH_DEFAULT="/home/spectre/esp-idf-v6.0.2"
IDF_TARGET_DEFAULT="esp32c6"
ESP_IDF_VERSION_DEFAULT="6.0.2"
TOOLCHAIN_VER="esp-15.2.0_20251204"
DEFAULT_PORT="/dev/ttyACM0"
DEFAULT_TEST_HOST="192.168.4.1"

# --- Environment setup (manual, no export.sh) ---------------------------------
setup_env() {
    export IDF_PATH="${IDF_PATH_OVERRIDE:-$IDF_PATH_DEFAULT}"
    export IDF_TARGET="${IDF_TARGET:-$IDF_TARGET_DEFAULT}"
    export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
    export ESP_IDF_VERSION="${ESP_IDF_VERSION:-$ESP_IDF_VERSION_DEFAULT}"
    export IDF_PYTHON_CHECK_CONSTRAINTS=0

    if [ ! -f "$IDF_PATH/tools/idf.py" ]; then
        echo "ERROR: IDF not found at $IDF_PATH (missing tools/idf.py)" >&2
        exit 1
    fi

    local tools="$IDF_TOOLS_PATH/tools"
    local riscv="$tools/riscv32-esp-elf/$TOOLCHAIN_VER/riscv32-esp-elf/bin"
    local xtensa="$tools/xtensa-esp-elf/$TOOLCHAIN_VER/xtensa-esp-elf/bin"

    if [ -d "$riscv" ]; then PATH="$riscv:$PATH"; else
        echo "WARNING: riscv32 toolchain not found at $riscv" >&2
    fi
    if [ -d "$xtensa" ]; then PATH="$xtensa:$PATH"; fi

    # GDB (used by idf.py monitor for panic decoding) ships as a separate tool.
    local gdb_bin
    gdb_bin="$(ls -d "$tools"/riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin 2>/dev/null | head -n1 || true)"
    if [ -n "$gdb_bin" ] && [ -d "$gdb_bin" ]; then PATH="$gdb_bin:$PATH"; fi

    PATH="$IDF_PATH/tools:$PATH"
    PATH="$IDF_PATH/components/esptool_py/esptool:$PATH"
    export PATH

    local venv="$IDF_TOOLS_PATH/python_env/idf6.0_py3.11_env"
    if [ -x "$venv/bin/python" ]; then
        export IDF_PYTHON_ENV_PATH="$venv"
        PYTHON="$venv/bin/python"
    else
        PYTHON="$(command -v python3 || echo /usr/bin/python3)"
    fi

    # Sanity-check the RISC-V compiler (the cc1 failure mode).
    local cc="$riscv/riscv32-esp-elf-gcc"
    if [ -x "$cc" ] && [ ! -x "$riscv/../libexec/gcc/riscv32-esp-elf/15.2.0/cc1" ]; then
        echo "ERROR: toolchain cc1 missing; re-extract $IDF_TOOLS_PATH/dist/riscv32-esp-elf-${TOOLCHAIN_VER#esp-}*.tar.xz" >&2
        exit 1
    fi
}

# Repair/create the Python venv if pip or core packages are missing.
ensure_venv() {
    local venv="${IDF_PYTHON_ENV_PATH:-$IDF_TOOLS_PATH/python_env/idf6.0_py3.11_env}"
    if [ ! -x "$venv/bin/python" ]; then
        echo "Creating Python venv at $venv ..."
        python3 -m venv "$venv"
    fi
    if [ ! -x "$venv/bin/pip" ]; then
        "$venv/bin/python" -m ensurepip --upgrade >/dev/null 2>&1 || true
    fi
    if ! "$venv/bin/python" -c "import click, pyparsing" >/dev/null 2>&1; then
        echo "Installing ESP-IDF Python requirements (this may take a while) ..."
        "$venv/bin/python" -m pip install -r "$IDF_PATH/tools/requirements/requirements.core.txt"
    fi
    mkdir -p "$IDF_TOOLS_PATH"
    touch "$IDF_TOOLS_PATH/espidf.constraints.v6.0.txt"
    PYTHON="$venv/bin/python"
}

idf() {
    "$PYTHON" "$IDF_PATH/tools/idf.py" "$@"
}

# --- Commands -----------------------------------------------------------------
cmd_clean() {
    cd "$PROJECT_DIR"
    echo "Removing build/ sdkconfig sdkconfig.old"
    rm -rf build sdkconfig sdkconfig.old
}

cmd_build() {
    setup_env
    ensure_venv
    cd "$PROJECT_DIR"
    echo "Cleaning stale artifacts ..."
    rm -rf build sdkconfig sdkconfig.old
    echo "Setting target $IDF_TARGET ..."
    idf set-target "$IDF_TARGET"
    echo "Building ..."
    idf build
    echo "Build OK. Artifacts:"
    ls -1 build/*.bin build/*.elf 2>/dev/null || true
}

cmd_flash() {
    setup_env
    ensure_venv
    cd "$PROJECT_DIR"
    local port="${PORT:-$DEFAULT_PORT}"
    if [ ! -e "$port" ]; then
        echo "ERROR: serial device $port not found. Available ports:" >&2
        ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  (none)" >&2
        exit 1
    fi
    if [ ! -r "$port" ] || [ ! -w "$port" ]; then
        echo "ERROR: no read/write permission on $port. Run:" >&2
        echo "  sudo chmod 666 $port" >&2
        exit 1
    fi
    idf -p "$port" flash
}

cmd_monitor() {
    setup_env
    ensure_venv
    cd "$PROJECT_DIR"
    local port="${PORT:-$DEFAULT_PORT}"
    if [ ! -e "$port" ]; then
        echo "ERROR: serial device $port not found." >&2
        exit 1
    fi
    if [ ! -t 0 ]; then
        echo "ERROR: monitor needs an interactive TTY. Run '$0 monitor' from a terminal." >&2
        exit 1
    fi
    idf -p "$port" monitor
}

cmd_all() {
    cmd_build
    cmd_flash
    cmd_monitor
}

# Run one HTTP test: method, path, expected status.
HTTP_PASS=0
HTTP_FAIL=0
run_http_test() {
    local method="$1" path="$2" expected="$3"
    local tmp code body
    tmp="$(mktemp)"
    code="$(curl -sS -m 5 -o "$tmp" -w '%{http_code}' -X "$method" "$TEST_HOST$path" 2>/dev/null || echo 000)"
    body="$(head -c 200 "$tmp" 2>/dev/null || true)"
    rm -f "$tmp"
    if [ "$code" = "$expected" ]; then
        echo "PASS  $method $path -> $code"
        HTTP_PASS=$((HTTP_PASS + 1))
    else
        echo "FAIL  $method $path -> $code (expected $expected)"
        HTTP_FAIL=$((HTTP_FAIL + 1))
    fi
    if [ -n "$body" ]; then echo "      body: $body"; fi
}

cmd_test() {
    local host="${TEST_HOST:-$DEFAULT_TEST_HOST}"
    TEST_HOST="http://$host"
    echo "Testing ESP32-C6 Gateway at $TEST_HOST"
    echo "Host must be joined to WiFi 'ESP32-Gateway' (password: gateway123)."
    echo
    if ! curl -sS -m 4 -o /dev/null "$TEST_HOST/sensor" 2>/dev/null; then
        echo "WARNING: $host is not reachable. Connect the host to the AP first." >&2
    fi

    # Reset to factory defaults before testing so the destructive ext/reset and
    # ext/power tests run against a known-clean configuration (both disabled).
    # The device erases NVS and reboots; wait for it to come back.
    echo "Factory-resetting device to known state..."
    if curl -sS -m 5 -X POST "$TEST_HOST/system/reset" 2>/dev/null; then
        echo "Reset command sent. Waiting for device to restart..."
    else
        echo "WARNING: Could not send reset command. Device may be in unknown state." >&2
    fi

    # Wait for the SoftAP to reappear (up to 60 seconds).
    echo "Waiting for AP to become reachable..."
    local waited=0 max_wait=60
    while [ "$waited" -lt "$max_wait" ]; do
        if curl -sS -m 2 -o /dev/null "$TEST_HOST/sensor" 2>/dev/null; then
            echo "Device is back after ${waited}s."
            break
        fi
        sleep 2
        waited=$((waited + 2))
    done
    if [ "$waited" -ge "$max_wait" ]; then
        echo "ERROR: Device did not come back within ${max_wait}s." >&2
        return 1
    fi
    echo

    # Expected responses reflect the current stub implementation.
    # The matrix is read from the shared CSV (single source of truth) that
    # also generates the on-device self-test table.
    local csv="$PROJECT_DIR/tests/http_endpoints.csv"
    if [ ! -f "$csv" ]; then
        echo "ERROR: test matrix not found: $csv" >&2
        return 1
    fi
    local method path expected
    while IFS=, read -r method path expected || [ -n "$method" ]; do
        case "$method" in "" | "#"*) continue ;; esac
        # Factory default state was already restored above; a mid-test
        # reboot would break the remaining assertions.
        case "$method $path" in "POST /system/reboot") continue ;; esac
        run_http_test "${method// /}" "${path// /}" "${expected// /}"
    done < <(tr -d '\r' < "$csv")
    echo
    echo "Summary: $HTTP_PASS passed, $HTTP_FAIL failed"
    [ "$HTTP_FAIL" -eq 0 ]
}

cmd_env() {
    setup_env
    echo "IDF_PATH       = $IDF_PATH"
    echo "IDF_TARGET     = $IDF_TARGET"
    echo "ESP_IDF_VER    = $ESP_IDF_VERSION"
    echo "IDF_TOOLS_PATH = $IDF_TOOLS_PATH"
    echo "IDF_PYTHON_ENV = ${IDF_PYTHON_ENV_PATH:-<none>}"
    echo "PYTHON         = $PYTHON"
    echo "PATH           = $PATH"
}

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

main() {
    local cmd="${1:-help}"
    case "$cmd" in
        build)   cmd_build ;;
        flash)   cmd_flash ;;
        monitor) cmd_monitor ;;
        all)     cmd_all ;;
        test)    cmd_test ;;
        clean)   cmd_clean ;;
        env)     cmd_env ;;
        help|-h|--help) usage ;;
        *) echo "Unknown command: $cmd" >&2; usage; exit 2 ;;
    esac
}

main "$@"