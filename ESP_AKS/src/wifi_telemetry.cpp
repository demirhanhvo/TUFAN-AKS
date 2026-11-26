// Wi-Fi TEST telemetri yolu (LoRa'ya paralel; yarış build'i DEĞİL).
//
//   CAN task ──xQueueOverwrite──> TEL_sensorDataQueue ──peek──> LoRa task (değişmedi)
//                                                     └─peek──> WiFi_Task (bu dosya)
//   WiFi_Task: 5 Hz → SysStateDerive + sanitize (LoRa ile aynı) →
//              Telemetry::formatFrame (aynı 19 alanlı TEL v2 satırı, kendi seq'i)
//              → TelLineRing (HTTP) + esp_mqtt_client_enqueue (MQTT)
//
// Engellememe kuralı: WiFi_Task ağ G/Ç'si YAPMAZ. MQTT gönderimini MQTT
// task'ı (outbox), HTTP'yi httpd task'ı yapar; WiFi_Task yalnız kuyruğa koyar,
// böylece task watchdog'u ağ gecikmesinden etkilenmez.
//
// Hata kuralı: ESP_ERROR_CHECK YOK — bir Wi-Fi/MQTT hatası aracı abort/reboot
// ETTİREMEZ. Init başarısızsa task loglar ve telemetrisiz bekler.

#include "SystemConfig.h"

#if WIFI_TELEMETRY_ENABLED

#include "wifi_telemetry.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "SysStateDerive.h"
#include "TelLineRing.h"
#include "Telemetry.h"
#include "TelemetrySanitize.h"
#include "VehicleData.h"

#if __has_include("WifiSecrets.h")
#include "WifiSecrets.h"
#else
#error "include/WifiSecrets.h yok: include/WifiSecrets.example.h dosyasini kopyalayip doldurun (bkz. Documents/WIFI_TELEMETRI_TEST.md)"
#endif

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static constexpr const char* TAG = "WIFI_TEL";

namespace {

using LineRing = TelLineRing<WIFI_TEL_RING_LINES, LORA_TEL_FRAME_MAX_BYTES>;

LineRing s_ring;  // ~18 KB .bss — heap'i (TLS için gerekli) yemez
SemaphoreHandle_t s_ringMutex = nullptr;
QueueHandle_t s_queue = nullptr;
esp_mqtt_client_handle_t s_mqtt = nullptr;
httpd_handle_t s_httpd = nullptr;

std::atomic<bool> s_wifiUp{false};
std::atomic<bool> s_mqttUp{false};
std::atomic<uint32_t> s_lastSeq{0};
std::atomic<uint32_t> s_ipAddr{0};  // esp_ip4_addr_t.addr (ağ bayt sırası)
std::atomic<uint32_t> s_mqttDropCount{0};

char s_topicTel[64];
char s_topicStatus[64];

void formatIp(uint32_t addr, char* out, size_t len) {
    snprintf(out, len, "%u.%u.%u.%u", (unsigned)(addr & 0xFFu),
             (unsigned)((addr >> 8) & 0xFFu), (unsigned)((addr >> 16) & 0xFFu),
             (unsigned)((addr >> 24) & 0xFFu));
}

int currentRssi() {
    wifi_ap_record_t ap = {};
    if (s_wifiUp.load() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

int formatStatusJson(char* out, size_t len, bool online) {
    char ip[16];
    formatIp(s_ipAddr.load(), ip, sizeof(ip));
    size_t lines = 0;
    if (xSemaphoreTake(s_ringMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        lines = s_ring.count();
        xSemaphoreGive(s_ringMutex);
    }
    return snprintf(out, len,
                    "{\"online\":%s,\"fw\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                    "\"mqtt\":%s,\"lastSeq\":%lu,\"lines\":%u,\"uptimeMs\":%lu,"
                    "\"mqttDrops\":%lu,\"test\":true}",
                    online ? "true" : "false", FW_VERSION, ip, currentRssi(),
                    s_mqttUp.load() ? "true" : "false",
                    (unsigned long)s_lastSeq.load(), (unsigned)lines,
                    (unsigned long)(esp_timer_get_time() / 1000LL),
                    (unsigned long)s_mqttDropCount.load());
}

// ---------------------------------------------------------------------------
// HTTP (aynı ağdaki istemci için) — httpd task'ında çalışır.
// ---------------------------------------------------------------------------
void addCommonHeaders(httpd_req_t* req) {
    // Tarayıcı sayfası file:// veya başka bir origin'den açılır → CORS serbest.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t handleTelemetryCsv(httpd_req_t* req) {
    bool hasSince = false;
    uint32_t since = 0;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            char* end = nullptr;
            const unsigned long v = strtoul(val, &end, 10);
            if (end != val && *end == '\0') {
                hasSince = true;
                since = static_cast<uint32_t>(v);
            }
        }
    }

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    addCommonHeaders(req);

    // Mutex altında küçük parçalar kopyala, mutex DIŞINDA gönder: yavaş bir
    // istemci WiFi_Task'ın ring'e yazmasını bloklamasın.
    char chunk[1024];
    bool first = true;
    uint32_t last = 0;
    while (true) {
        size_t n = 0;
        if (xSemaphoreTake(s_ringMutex, pdMS_TO_TICKS(50)) != pdTRUE) break;
        const size_t idx =
            first ? s_ring.startIndex(hasSince, since, WIFI_TEL_HTTP_DEFAULT_LINES)
                  : s_ring.indexAfter(last);
        n = s_ring.collect(idx, chunk, sizeof(chunk), &last);
        xSemaphoreGive(s_ringMutex);
        first = false;
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(n)) != ESP_OK) {
            return ESP_FAIL;  // istemci koptu
        }
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t handleStatus(httpd_req_t* req) {
    char json[256];
    const int n = formatStatusJson(json, sizeof(json), true);
    httpd_resp_set_type(req, "application/json");
    addCommonHeaders(req);
    return httpd_resp_send(req, json, n > 0 ? n : 0);
}

esp_err_t handleRoot(httpd_req_t* req) {
    static const char kInfo[] =
        "TUFAN-AKS Wi-Fi telemetri (TEST yolu - LoRa resmi kanaldir)\n"
        "\n"
        "GET /telemetry.csv            son satirlar\n"
        "GET /telemetry.csv?since=N    seq > N olan satirlar\n"
        "GET /status                   JSON durum (ip, rssi, mqtt, lastSeq)\n"
        "\n"
        "Satir: TEL,ver,seq,rpm,motorVoltDeciV(4. alan 'torque' adli),motorErr,\n"
        "motorValid,motorTimeout,cellVMax,cellVMin,tempH,tempL,sysState,packV,\n"
        "current(centi-A),soc,bmsValid,ts_ms,spd_x10\n"
        "Gosterim: TUFAN-UKS-TELEMETRY/pc/wifi-telemetri/index.html\n";
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    addCommonHeaders(req);
    return httpd_resp_send(req, kInfo, HTTPD_RESP_USE_STRLEN);
}

bool startHttpServer() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 1;
    cfg.task_priority = TASK_PRIORITY_WIFI + 1;  // HMI (2) ile eşit, CAN/LoRa'nın altında
    cfg.stack_size = 4096;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP sunucu baslatilamadi");
        return false;
    }
    const httpd_uri_t uris[] = {
        {"/telemetry.csv", HTTP_GET, handleTelemetryCsv, nullptr},
        {"/status", HTTP_GET, handleStatus, nullptr},
        {"/", HTTP_GET, handleRoot, nullptr},
    };
    for (const httpd_uri_t& u : uris) httpd_register_uri_handler(s_httpd, &u);
    return true;
}

// ---------------------------------------------------------------------------
// Wi-Fi / IP / MQTT olayları — sys_evt ve MQTT task'larında çalışır; kısa tut.
// ---------------------------------------------------------------------------
void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifiUp.exchange(false)) {
            const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
            ESP_LOGW(TAG, "Hotspot baglantisi koptu (sebep %d) — %u ms'de bir "
                          "yeniden denenecek",
                     ev ? (int)ev->reason : -1, (unsigned)WIFI_TEL_RECONNECT_MS);
        }
        s_ipAddr.store(0);
        // Yeniden bağlanma WiFi_Task'ta (throttle'lı) — burada spin yok.
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        s_ipAddr.store(ev->ip_info.ip.addr);
        s_wifiUp.store(true);
        char ip[16];
        formatIp(ev->ip_info.ip.addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "Hotspot'a baglandi — IP %s (http://%s/telemetry.csv)", ip, ip);
    }
}

void onMqttEvent(void*, esp_event_base_t, int32_t id, void*) {
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            s_mqttUp.store(true);
            ESP_LOGI(TAG, "MQTT broker'a baglandi (%s)", s_topicTel);
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_mqttUp.exchange(false)) ESP_LOGW(TAG, "MQTT baglantisi koptu");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT hata olayi");
            break;
        default:
            break;
    }
}

bool startMqtt() {
    snprintf(s_topicTel, sizeof(s_topicTel), "%s/tel", MQTT_TEL_TOPIC_BASE);
    snprintf(s_topicStatus, sizeof(s_topicStatus), "%s/status", MQTT_TEL_TOPIC_BASE);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = MQTT_TEL_BROKER_URI;
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.credentials.username = MQTT_TEL_USERNAME;
    cfg.credentials.client_id = "tufan-aks";
    cfg.credentials.authentication.password = MQTT_TEL_PASSWORD;
    cfg.session.keepalive = 15;
    cfg.session.last_will.topic = s_topicStatus;
    cfg.session.last_will.msg = "{\"online\":false,\"test\":true}";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.network.reconnect_timeout_ms = static_cast<int>(WIFI_TEL_RECONNECT_MS);
    cfg.network.timeout_ms = 5000;
    cfg.task.priority = TASK_PRIORITY_WIFI + 1;
    cfg.task.stack_size = 6144;  // TLS el sıkışması için
    // Kopukken biriken QoS0 satırları sınırla (~70 satır): heap'i korur, kısa
    // kesintide satırlar yine gider (sayfa seq'e göre sıralar).
    cfg.outbox.limit = 8192;
    cfg.buffer.size = 512;

    s_mqtt = esp_mqtt_client_init(&cfg);
    if (s_mqtt == nullptr) {
        ESP_LOGE(TAG, "MQTT istemcisi olusturulamadi");
        return false;
    }
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, onMqttEvent, nullptr);
    // start: MQTT task'ı Wi-Fi yokken de bekler ve kendisi yeniden bağlanır.
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT istemcisi baslatilamadi");
        return false;
    }
    return true;
}

bool initWifiStack() {
    esp_err_t err = nvs_flash_init();  // Wi-Fi sürücüsü kalibrasyon için NVS ister
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();  // NVS'yi bu firmware'de Wi-Fi dışında kullanan yok
        err = nvs_flash_init();
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err)); return false; }

    if ((err = esp_netif_init()) != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err)); return false; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err)); return false; }
    if (esp_netif_create_default_wifi_sta() == nullptr) { ESP_LOGE(TAG, "netif sta olusturulamadi"); return false; }

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&initCfg)) != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err)); return false; }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr, nullptr);

    wifi_config_t staCfg = {};
    strncpy(reinterpret_cast<char*>(staCfg.sta.ssid), WIFI_TEL_SSID, sizeof(staCfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(staCfg.sta.password), WIFI_TEL_PASSWORD, sizeof(staCfg.sta.password) - 1);
    staCfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // ayarlar her boot'ta WifiSecrets.h'den
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &staCfg)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi baslatilamadi: %s", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_max_tx_power(static_cast<int8_t>(WIFI_TEL_MAX_TX_POWER_QDBM));
    ESP_LOGW(TAG, "Wi-Fi TEST telemetri yolu AKTIF — hotspot '%s' (yaris build'i degil)", WIFI_TEL_SSID);
    return true;
}

void vTask_WiFi_Telemetry(void*) {
    // Init (PHY kalibrasyonu dahil) WDT kaydından ÖNCE: tek seferlik ve kısa,
    // ama 5 s TWDT'ye ağ/flash gecikmesi yüzünden takılma riski taşımasın.
    const bool stackOk = initWifiStack();
    esp_task_wdt_add(nullptr);
    if (!stackOk) {
        ESP_LOGE(TAG, "Wi-Fi telemetri devre disi (init hatasi) — arac etkilenmez");
        while (true) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    startHttpServer();  // başarısızsa yalnız MQTT ile devam
    startMqtt();        // başarısızsa yalnız HTTP ile devam

    uint32_t seq = 0;
    TickType_t lastReconnect = xTaskGetTickCount();
    TickType_t lastStatus = 0;
    TickType_t lastStackLog = xTaskGetTickCount();
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        esp_task_wdt_reset();
        const TickType_t now = xTaskGetTickCount();

        TelemetryData pkt = {};
        if (s_queue != nullptr && xQueuePeek(s_queue, &pkt, 0) == pdTRUE) {
            // LoRa_txSend ile AYNI sıra: önce türetme, sonra sanitize (bkz.
            // main.cpp LoRa_txSend yorumu). Yalnız bu yerel kopya değişir.
            SysStateDerive::applyIfEnabled(pkt);
            char line[LORA_TEL_FRAME_MAX_BYTES + 8];
            const int n = Telemetry::formatFrame(
                TelemetrySanitize::sanitizeForUplink(pkt), seq, line, sizeof(line));
            if (n > 0 && n <= static_cast<int>(LORA_TEL_FRAME_MAX_BYTES)) {
                if (xSemaphoreTake(s_ringMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    s_ring.push(seq, line, static_cast<size_t>(n));
                    xSemaphoreGive(s_ringMutex);
                }
                if (s_mqtt != nullptr && s_wifiUp.load()) {
                    // CRLF'siz yayınla; enqueue engellemez (MQTT task gönderir).
                    if (esp_mqtt_client_enqueue(s_mqtt, s_topicTel, line, n - 2, 0, 0, true) < 0) {
                        s_mqttDropCount.fetch_add(1);
                    }
                }
                s_lastSeq.store(seq);
                ++seq;
            }
        }

        if (!s_wifiUp.load() &&
            (now - lastReconnect) >= pdMS_TO_TICKS(WIFI_TEL_RECONNECT_MS)) {
            lastReconnect = now;
            esp_wifi_connect();  // asenkron; sonuç olayla gelir
        }

        if (s_mqttUp.load() && (now - lastStatus) >= pdMS_TO_TICKS(WIFI_TEL_STATUS_PERIOD_MS)) {
            lastStatus = now;
            char json[256];
            const int n = formatStatusJson(json, sizeof(json), true);
            if (n > 0) esp_mqtt_client_enqueue(s_mqtt, s_topicStatus, json, n, 1, 1, true);
        }

        if ((now - lastStackLog) >= pdMS_TO_TICKS(30000)) {
            lastStackLog = now;
            ESP_LOGI("WiFi_Task", "Stack HWM %u B, heap bos %u B (min %u B)",
                     (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(WIFI_TEL_PERIOD_MS));
    }
}

}  // namespace

void WifiTelemetry_start(QueueHandle_t telemetryQueue) {
    s_queue = telemetryQueue;
    s_ringMutex = xSemaphoreCreateMutex();
    if (s_ringMutex == nullptr) {
        ESP_LOGE(TAG, "Mutex olusturulamadi — Wi-Fi telemetri baslatilmadi");
        return;
    }
    // Core 1 (VCU ile aynı çekirdek, ama VCU=10 >> 1): core 0'daki CAN/LoRa/
    // HMI ile CPU yarışmaz.
    if (xTaskCreatePinnedToCore(vTask_WiFi_Telemetry, "WiFi_Task", 4096, nullptr,
                                TASK_PRIORITY_WIFI, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "WiFi_Task olusturulamadi — Wi-Fi telemetri yok");
    }
}

#endif  // WIFI_TELEMETRY_ENABLED
