# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.
#
# Wi-Fi TEST: ESP32-S3 build'i ([env:esp32s3_wifitest]) arac firmware'ini
# (main.cpp) DERLEMEZ — S3'te AKS pinlerinin bir kismi yok/flash'a ayrilmis.
# Yerine s3_wifitest_main.cpp (sahte veri) + wifi_telemetry.cpp derlenir.
# ESP32 (esp32dev*) build'leri eskisi gibi src/ altindaki her seyi derler.

if(IDF_TARGET STREQUAL "esp32s3")
    set(app_sources
        ${CMAKE_SOURCE_DIR}/src/s3_wifitest_main.cpp
        ${CMAKE_SOURCE_DIR}/src/wifi_telemetry.cpp)
else()
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
endif()

idf_component_register(SRCS ${app_sources})
