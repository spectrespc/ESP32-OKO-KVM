# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Version reported by esp_app_get_description()->version and used by the OTA
# manifest comparison. Keep in sync with CHANGELOG.md. Do not add a root
# version.txt: it would take precedence over this value.
project(gateway VERSION 1.4.1)