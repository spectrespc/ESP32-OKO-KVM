# main/CMakeLists.txt

# Generate the self-test expectation table from the shared CSV so the
# on-device self-test and build_and_flash.sh stay in sync.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(HTTP_TESTS_CSV "${CMAKE_CURRENT_LIST_DIR}/../tests/http_endpoints.csv")
set(HTTP_TESTS_HDR "${CMAKE_CURRENT_BINARY_DIR}/http_tests_generated.h")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_http_tests.py" "${HTTP_TESTS_CSV}" "${HTTP_TESTS_HDR}"
    RESULT_VARIABLE "HTTP_TESTS_GEN_RC"
)
if(NOT HTTP_TESTS_GEN_RC EQUAL 0)
    message(FATAL_ERROR "Failed to generate http_tests_generated.h (rc=${HTTP_TESTS_GEN_RC})")
endif()

idf_component_register(
    SRCS "main.c" "pins.c" "uart_handler.c" "http_server.c" "sd_handler.c" "sensor_handler.c" "config.c" "wifi_ap.c" "wifi_scan.c" "self_test.c" "pin_validation.c" "telnet_bridge.c" "web_handlers.c" "log_handler.c" "system_handler.c" "ext_control.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_driver_uart esp_driver_sdspi fatfs esp_http_server nvs_flash esp_wifi esp_driver_spi esp_driver_i2c esp_driver_gpio esp_timer esp_netif esp_http_client lwip cjson spiffs mqtt
)

# Enable exFAT support in the FatFs library. IDF v6.0.2 hardcodes
# `#define FF_FS_EXFAT 0` in fatfs/src/ffconf.h and exposes no Kconfig option,
# so a command-line -DFF_FS_EXFAT=1 is both a redefinition error (ffconf.h
# redefines it to 0) and ineffective. Patch the header itself at configure time;
# the edit is idempotent so it is only applied once.
idf_component_get_property(fatfs_dir fatfs COMPONENT_DIR)
set(_ffconf "${fatfs_dir}/src/ffconf.h")
if(EXISTS "${_ffconf}")
    file(READ "${_ffconf}" _ffconf_src)
    if(_ffconf_src MATCHES "#define[ \t]+FF_FS_EXFAT[ \t]+0")
        string(REGEX REPLACE "#define[ \t]+FF_FS_EXFAT[ \t]+0"
                             "#define FF_FS_EXFAT 1" _ffconf_new "${_ffconf_src}")
        file(WRITE "${_ffconf}" "${_ffconf_new}")
        message(STATUS "Patched fatfs ffconf.h: FF_FS_EXFAT=1")
    endif()
else()
    message(WARNING "fatfs ffconf.h not found at ${_ffconf}; exFAT support not enabled")
endif()

# Build the SPA (web/) into a SPIFFS image and flash it with the app.
spiffs_create_partition_image(spiffs ${CMAKE_SOURCE_DIR}/web FLASH_IN_PROJECT)