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

## ESP32-S3 test kartı (`esp32s3_wifitest`)

Wi-Fi testleri araç anakartı yerine bir **ESP32-S3 geliştirme kartında**
(DevKitC-1 vb.) yapılır. S3 build'i araç firmware'i **değildir**:

| | Araç (`esp32dev`, `esp32dev_wifi`) | Test kartı (`esp32s3_wifitest`) |
|---|---|---|
| Çip | ESP32 (Xtensa LX6, 2 çekirdek) | ESP32-S3 (Xtensa LX7, 2 çekirdek) |
| Giriş noktası | `src/main.cpp` | `src/s3_wifitest_main.cpp` (`main.cpp` derlenmez — `src/CMakeLists.txt`) |
| Veri kaynağı | CAN_Task (BMS + motor, TJA1050) | `Sim_Task` → `TelSimulator` (**sahte** veri) |
| Wi-Fi yolu | `wifi_telemetry.cpp` | **aynı dosya** (formatFrame, ring, HTTP, MQTT) |
| LoRa / Nextion / röle | var | yok |

Neden ayrı giriş noktası: S3'te ESP32'nin GPIO 22–25'i hiç yoktur, 26–32
dahili flash'a ayrılmıştır (çoğu modülde 33–37 de PSRAM'e). AKS pinleri
(LoRa M0=IO25, M1=IO26, Nextion IO32/33, röle SPI IO23...) S3'te
derlenmez/çalışmaz. S3'e hiçbir kablo bağlanmaz; yalnız USB.

Sahte veri (`lib/WifiTelemetry/TelSimulator.h`, testi
`test/test_native_wifi_sim`): 120 sn'lik döngü — bekleme (0 km/h,
−0,3 A) → hızlanma (0→45 km/h, −55 A'e kadar) → seyir (40–50 km/h,
−12..−20 A) → rejeneratif fren (+8 A) → bekleme. 24S LFP paket (76,8 V
nominal, 100 Ah): SoC akımın integraliyle düşer, gerilim SoC'ye ve yük
altındaki çökmeye göre değişir, sıcaklık yükle ısınır. Tüm değerler
UKS'in `Decode_Line` aralık kontrollerinden geçer (testle doğrulanır).
`sysState` araçtaki gibi akımdan türetilir. Durum JSON'unda `"sim":true`
yazar; PC sayfası "SAHTE VERİ (S3 simülatör)" gösterir.

Kurulum: `include/WifiSecrets.h` aynı dosyadır. Yükleme:
`LC_ALL=C.UTF-8 pio run -e esp32s3_wifitest --target upload` — kartın
**"UART"/"COM"** yazan USB portunu kullanın (seri log oradan gelir).
Flash 4 MB varsayılır, daha büyük flash'lı kartlarda da çalışır. Gerçek CAN
verisiyle test istenirse S3'e bir 3,3 V CAN transceiver (ör. SN65HVD230)
bağlanıp S3'te var olan pinler seçilmelidir — bu yapılmadı.

## PC tarafı: kayıt ve arayüz

```
HiveMQ ──> logger.py (TUFAN-UKS-TELEMETRY/pc/wifi-telemetri)
             ├─ logs/wifi_*.csv          kalıcı kayıt
             └─ http://<pc>:8080         kayıtlı verinin web adresi
                  ├─ /                   canlı sayfa (geçmiş + canlı)
                  └─ /api/lines?since=   ──> TUFAN-monitor --port WIFI (UKS arayüzü)
```

Ayrıntı: `TUFAN-UKS-TELEMETRY/pc/wifi-telemetri/README.md` ve
TUFAN-monitor README "Wi-Fi TEST kaynağı". Monitor bu kaynakla yazdığı
dosyalara `_WIFI` ekler; jüri kaydıyla karışmaz.

## HTTP uç noktaları (ESP32 ile aynı ağdayken)

| İstek | Yanıt |
|---|---|
| `GET /telemetry.csv` | son 20 satır |
| `GET /telemetry.csv?since=N` | seq > N olan satırlar; ESP32 yeniden başladıysa (N > en yeni) tüm tampon |
| `GET /status` | JSON: ip, rssi, mqtt, lastSeq, uptimeMs, mqttDrops, sim |
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
   (Test kartı için `-e esp32s3_wifitest`; bkz. yukarıdaki S3 bölümü.)
3. PC: `TUFAN-UKS-TELEMETRY/pc/wifi-telemetri` içinde `python logger.py`
   → tarayıcıda `http://localhost:8080`; UKS arayüzü için
   `python monitor.py --port WIFI` (bkz. oradaki README).
4. Bitince normal firmware'e dön: `LC_ALL=C.UTF-8 pio run -e esp32dev --target upload`

## Ayarlar (`include/SystemConfig.h`)

`WIFI_TEL_PERIOD_MS` (200), `WIFI_TEL_RING_LINES` (150),
`WIFI_TEL_RECONNECT_MS` (5000), `WIFI_TEL_STATUS_PERIOD_MS` (10000),
`WIFI_TEL_MAX_TX_POWER_QDBM` (52 = 13 dBm), `TASK_PRIORITY_WIFI` (1).

## Bilinen riskler / doğrulanmadı

- `esp32s3_wifitest` build'i derleniyor (RAM 56,5 KB / 320 KB, Flash
  991 KB / 1,5 MB) ama **kart üstünde çalıştırılmadı**.

- Wi-Fi TX akım tepeleri (~250–400 mA) AKS beslemesinde **brownout**
  yaratabilir. TX gücü 13 dBm'e kısıldı; boot logundaki `RESET SEBEBI`
  izlenmeli.
- TLS el sıkışması core 1'de birkaç yüz ms CPU kullanır. VCU döngü süresi ve
  `Stack HWM` logları bench'te kontrol edilmeli.
- Heap: statik RAM 69.7 KB'tan 112 KB'a çıktı. `WiFi_Task` 30 sn'de bir boş
  heap/min heap loglar.
