#pragma once

// Wi-Fi TEST telemetri yolu — yalnız WIFI_TELEMETRY_ENABLED=1 ([env:esp32dev_wifi]).
// Bkz. SystemConfig.h "Wi-Fi TEST telemetri yolu" ve
// Documents/WIFI_TELEMETRI_TEST.md.

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Wi-Fi task'ını (core 1, TASK_PRIORITY_WIFI) başlatır. `telemetryQueue`
// CAN task'inin xQueueOverwrite ettiği tek elemanlı TelemetryData kuyruğudur;
// Wi-Fi yolu ondan YALNIZ xQueuePeek ile okur (LoRa/VCU ile aynı desen).
// Hiçbir hata aracı durdurmaz/yeniden başlatmaz: init başarısızsa task
// loglayıp telemetrisiz bekler.
void WifiTelemetry_start(QueueHandle_t telemetryQueue);
