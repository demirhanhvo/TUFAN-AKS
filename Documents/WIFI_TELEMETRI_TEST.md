# Wi-Fi Telemetri — TEST Yolu

> **Yarış kanalı değildir.** Resmi telemetri kanalı LoRa'dır (yönetmelik 9.2).
> Bu yol yalnız test/pit içindir ve ayrı bir build ortamıyla
> (`esp32dev_wifi`) derlenir. Yarış build'inde (`esp32dev`) Wi-Fi kodu
> **hiç derlenmez/linklenmez**. Yarışta kullanılacaksa önce sağlamlaştırılmalı
> ve 9.2 uygunluğu teyit edilmelidir.

## Mimari

```
CAN_Task ──xQueueOverwrite──> TEL_sensorDataQueue ──peek──> LoRa_Task   (değişmedi)
                                                   └─peek──> WiFi_Task   (core 1, prio 1)
WiFi_Task (5 Hz): SysStateDerive + sanitize (LoRa ile aynı)
                  → Telemetry::formatFrame → aynı 19 alanlı "TEL,2,..." satırı
                  ├─> TelLineRing (son 150 satır) ──> HTTP  GET /telemetry.csv?since=<seq>
                  └─> esp_mqtt_client_enqueue ──> HiveMQ Cloud (TLS) <kök>/tel
```

- Satır formatı LoRa ile **birebir aynıdır** (ortak `Telemetry::formatFrame`).
  Wi-Fi yolunun kendi `seq` sayacı vardır; LoRa seq'i ile karıştırılmamalıdır.
- Wi-Fi yolu `TEL_sensorDataQueue`'yu yalnız `xQueuePeek` ile okur. VCU/FAULT
  kararlarına ve LoRa yoluna hiçbir şekilde girmez.
- Wi-Fi sürücüsü, lwIP, MQTT ve httpd task'ları core 1'dedir
  (`sdkconfig.esp32dev_wifi`). CAN/LoRa/HMI core 0'dadır ve Wi-Fi tarafından
  preempt edilmez. VCU (core 1, prio 10), Wi-Fi sürücüsünün (prio 23) kısa
  kesintilerini görebilir; bench'te ölçülmelidir.
- `WiFi_Task` ağ G/Ç'si yapmaz: MQTT'ye `enqueue` eder (gönderimi MQTT task'ı
  yapar), HTTP'ye ring'den kopyalanır. Böylece task watchdog'u ağ
  gecikmesinden etkilenmez. Hiçbir Wi-Fi hatası aracı reboot ettirmez
  (`ESP_ERROR_CHECK` yok).
- Bölüm tablosu: Wi-Fi + TLS + MQTT 1 MB'a sığmadığı için bu ortam 1.5 MB'lık
  tek-app bölümü (`partitions_singleapp_large.csv`) kullanır. Normal firmware'i
  geri yüklemek tabloyu da geri yazar.

## HTTP uç noktaları (ESP32 ile aynı ağdayken)

| İstek | Yanıt |
|---|---|
| `GET /telemetry.csv` | son 20 satır |
| `GET /telemetry.csv?since=N` | seq > N olan satırlar; ESP32 yeniden başladıysa (N > en yeni) tüm tampon |
| `GET /status` | JSON: ip, rssi, mqtt, lastSeq, uptimeMs, mqttDrops |
| `GET /` | kısa açıklama |

CORS açıktır (`Access-Control-Allow-Origin: *`); kimlik doğrulama **yoktur**.
Hotspot'a bağlanan herkes okuyabilir.

## MQTT

- `<MQTT_TEL_TOPIC_BASE>/tel`: her mesaj bir TEL satırıdır (CRLF'siz), QoS 0.
- `<MQTT_TEL_TOPIC_BASE>/status`: 10 sn'de bir durum JSON'u (retained). Kopunca
  LWT `{"online":false}` yayınlanır.
- Kopukluk sırasında en fazla ~8 KB satır outbox'ta bekler, fazlası düşer
  (`mqttDrops`).

## Kurulum

1. `cp ESP_AKS/include/WifiSecrets.example.h ESP_AKS/include/WifiSecrets.h`
   ve değerleri doldur (dosya `.gitignore`'dadır).
   - Hotspot **2.4 GHz** olmalı, WPA2.
   - HiveMQ Cloud → Access Management: ESP32 için PUBLISH izinli bir kimlik,
     tarayıcı için ayrı bir SUBSCRIBE kimliği önerilir.
2. Derle ve yükle:
   ```
   cd ESP_AKS
   LC_ALL=C.UTF-8 pio run -e esp32dev_wifi --target upload
   LC_ALL=C.UTF-8 pio device monitor
   ```
   Seri logda `Hotspot'a baglandi — IP ...` ve `MQTT broker'a baglandi` görülmeli.
3. PC: `TUFAN-UKS-TELEMETRY/pc/wifi-telemetri/index.html` dosyasını tarayıcıda aç
   (bkz. oradaki README).
4. Bitince normal firmware'e dön: `LC_ALL=C.UTF-8 pio run -e esp32dev --target upload`

## Ayarlar (`include/SystemConfig.h`)

`WIFI_TEL_PERIOD_MS` (200), `WIFI_TEL_RING_LINES` (150),
`WIFI_TEL_RECONNECT_MS` (5000), `WIFI_TEL_STATUS_PERIOD_MS` (10000),
`WIFI_TEL_MAX_TX_POWER_QDBM` (52 = 13 dBm), `TASK_PRIORITY_WIFI` (1).

## Bilinen riskler / doğrulanmadı

- Wi-Fi TX akım tepeleri (~250–400 mA) AKS beslemesinde **brownout**
  yaratabilir. TX gücü 13 dBm'e kısıldı; boot logundaki `RESET SEBEBI`
  izlenmeli.
- TLS el sıkışması core 1'de birkaç yüz ms CPU kullanır. VCU döngü süresi ve
  `Stack HWM` logları bench'te kontrol edilmeli.
- Heap: statik RAM 69.7 KB'tan 112 KB'a çıktı. `WiFi_Task` 30 sn'de bir boş
  heap/min heap loglar.
