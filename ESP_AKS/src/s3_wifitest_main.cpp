// ESP32-S3 Wi-Fi TEST firmware'i — yalnız [env:esp32s3_wifitest].
//
// Bu dosya araç firmware'inin (main.cpp) YERİNE geçer; S3 build'inde
// main.cpp hiç derlenmez (bkz. src/CMakeLists.txt: IDF_TARGET esp32s3).
// Neden ayrı: S3'te AKS anakartının CAN/LoRa/Nextion/röle donanımı yok ve
// AKS pinlerinin bir kısmı S3'te mevcut değil (GPIO 22-25) ya da flash'a
// ayrılmış (26-32). Bu firmware Wi-Fi zincirinin tamamını test eder:
//
//   Sim_Task (5 Hz, core 0) ──xQueueOverwrite──> simQueue ──peek──> WiFi_Task
//   (CAN_Task'ın araçta yaptığının aynısı)              (araçtakiyle AYNI kod)
//
// Üretilen veri SAHTEDİR (bkz. lib/WifiTelemetry/TelSimulator.h); MQTT durum
// mesajında ve seri logda bu açıkça yazar.

#include "SystemConfig.h"

#if AKS_S3_WIFITEST

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "TelSimulator.h"
#include "VehicleData.h"
#include "wifi_telemetry.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static constexpr const char* TAG = "S3_WIFITEST";
static QueueHandle_t s_simQueue = nullptr;

static void vTask_Simulator(void*) {
    esp_task_wdt_add(nullptr);
    TelSimulator sim;
    TickType_t wake = xTaskGetTickCount();
    uint32_t n = 0;
    while (true) {
        esp_task_wdt_reset();
        const uint32_t nowMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const TelemetryData d = sim.next(nowMs);
        xQueueOverwrite(s_simQueue, &d);
        if ((++n % 25U) == 0U) {  // 5 sn'de bir özet
            ESP_LOGI(TAG, "SAHTE: hiz=%u.%u km/h akim=%ld cA SoC=%u.%02u%% V=%u dV T=%d C",
                     d.TEL_speedKmhX10 / 10U, d.TEL_speedKmhX10 % 10U,
                     static_cast<long>(d.TEL_bmsCurrentCentiA),
                     d.TEL_bmsSocHundredths / 100U, d.TEL_bmsSocHundredths % 100U,
                     d.TEL_bmsPackVoltageDeciV, d.TEL_bmsTempHighestC);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(WIFI_TEL_PERIOD_MS));
    }
}

extern "C" void app_main() {
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, " ESP32-S3 Wi-Fi TEST firmware'i (%s)", FW_VERSION);
    ESP_LOGW(TAG, " Arac verisi SAHTEDIR (TelSimulator) - CAN yok");
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGI(TAG, "RESET SEBEBI: %d", static_cast<int>(esp_reset_reason()));

    s_simQueue = xQueueCreate(1, sizeof(TelemetryData));
    if (s_simQueue == nullptr) {
        ESP_LOGE(TAG, "Kuyruk olusturulamadi");
        return;
    }
    xTaskCreatePinnedToCore(vTask_Simulator, "Sim_Task", 4096, nullptr, 5, nullptr, 0);
    WifiTelemetry_start(s_simQueue);
}

#endif  // AKS_S3_WIFITEST
