# ESP32 Audio Player - Internet Radio & Smart Speaker
# For ESP32-LyraT V4.3

cmake_minimum_required(VERSION 3.16)

# Ścieżki do ESP-ADF (dostosuj do swojej instalacji)
set(EXTRA_COMPONENT_DIRS "")

# Dodaj ADF components jeśli ADF_PATH jest ustawione
if(DEFINED ENV{ADF_PATH})
    list(APPEND EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
endif()

# Dodaj lokalne components jeśli folder istnieje
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/components")
    list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_audio_player)
