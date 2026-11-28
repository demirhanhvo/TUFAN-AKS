#pragma once
// Wi-Fi TEST yolu — SAHTE araç verisi üreteci (yalnız esp32s3_wifitest).
//
// Neden var: Wi-Fi testleri ESP32-S3 geliştirme kartında yapılıyor. S3'te
// AKS anakartındaki CAN (TJA1050 + BMS/motor), LoRa ve Nextion yok; ayrıca
// S3'te GPIO 22-25 hiç yok ve 26-32 flash'a ayrılmış olduğundan AKS pin
// haritası (LoRa M0=IO25, Nextion IO32/33...) S3'e taşınamaz. Bu yüzden S3
// build'i CAN_Task yerine bu üreteci çalıştırır ve aynı TelemetryData'yı
// aynı kuyruğa yazar; Wi-Fi yolunun geri kalanı (formatFrame, ring, HTTP,
// MQTT) araçtakiyle BİREBİR aynı koddur.
//
// Senaryo: 120 sn'lik tekrarlanan bir sürüş döngüsü
//    0- 10 sn  BEKLEME     hız 0, akım ~ -0,3 A (elektronik tüketim)
//   10- 30 sn  HIZLANMA    0 -> 45 km/h, akım -55 A'e kadar (deşarj)
//   30- 80 sn  SEYİR       40-50 km/h dalgalı, akım -12..-20 A
//   80-100 sn  FRENLEME    45 -> 0 km/h, rejeneratif +8 A (pakete akım)
//  100-120 sn  BEKLEME
// Paket 24S LFP (nominal 76,8 V, 100 Ah): SoC akım entegre edilerek düşer,
// gerilim SoC'ye ve yük altındaki çökmeye (iç direnç) göre hesaplanır,
// sıcaklık yükle yavaşça ısınır / boşta soğur. Gürültü deterministiktir
// (sabit tohumlu LCG) — aynı başlangıçla aynı dizi üretilir, test edilebilir.
//
// Birimler TelemetryData sözleşmesiyle aynıdır (VehicleData.h):
//   current centi-A (negatif = deşarj), packV deciV, hücre deci-mV,
//   SoC yüzde×100, hız km/h×10 (rpm'den rpmToSpeedKmhX10 ile — araçtaki gibi).
//
// Saf C++ — ESP-IDF bağımlılığı yok; native testte doğrulanır
// (test/test_native_wifi_sim).

#include <cmath>
#include <cstdint>

#include "Telemetry.h"     // rpmToSpeedKmhX10
#include "VehicleData.h"   // TelemetryData

class TelSimulator {
public:
    static constexpr uint32_t kCycleMs = 120000U;
    static constexpr float kCapacityAh = 100.0f;
    static constexpr int kCells = 24;

    explicit TelSimulator(float startSocPercent = 90.0f, uint32_t seed = 12345U)
        : m_socPct(startSocPercent), m_rng(seed) {}

    // Senaryodaki hedef hız (km/h) ve akım (A, negatif = deşarj) — gürültüsüz.
    static void profile(uint32_t ms, float* speedKmh, float* currentA) {
        const float t = static_cast<float>(ms % kCycleMs) / 1000.0f;
        float v = 0.0f;
        float i = -0.3f;
        if (t < 10.0f) {
            v = 0.0f; i = -0.3f;
        } else if (t < 30.0f) {
            const float k = (t - 10.0f) / 20.0f;  // 0..1
            v = 45.0f * k;
            i = -(15.0f + 40.0f * std::sin(k * 3.14159265f));  // tepe -55 A
        } else if (t < 80.0f) {
            v = 45.0f + 5.0f * std::sin((t - 30.0f) * 0.25f);
            i = -(16.0f + 4.0f * std::sin((t - 30.0f) * 0.25f));
        } else if (t < 100.0f) {
            const float k = (t - 80.0f) / 20.0f;
            v = 45.0f * (1.0f - k);
            i = (v > 3.0f) ? 8.0f : -0.3f;  // rejeneratif fren
        }
        *speedKmh = v;
        *currentA = i;
    }

    // Bir sonraki örneği üretir. `nowMs` monoton artmalıdır (açılıştan beri ms).
    TelemetryData next(uint32_t nowMs) {
        float speedKmh = 0.0f, currentA = 0.0f;
        profile(nowMs, &speedKmh, &currentA);
        speedKmh = clampf(speedKmh + noise(0.4f), 0.0f, 120.0f);
        if (speedKmh < 0.5f) speedKmh = 0.0f;
        // Boşta küçük gürültü: SysStateDerive bandında (±) Deşarj/Boşta titremesin.
        currentA += noise(std::fabs(currentA) < 1.0f ? 0.1f : 0.6f);

        // SoC: Ah entegrasyonu (deşarj negatif akım SoC'yi düşürür).
        const uint32_t dtMs = m_hasLast ? (nowMs - m_lastMs) : 0U;
        m_lastMs = nowMs;
        m_hasLast = true;
        const float dtH = static_cast<float>(dtMs) / 3600000.0f;
        m_socPct = clampf(m_socPct + (currentA * dtH / kCapacityAh) * 100.0f, 5.0f, 100.0f);
        // Döngüyü uzun testlerde sonsuza çalıştırmak için %20'nin altında başa sar.
        if (m_socPct < 20.0f) m_socPct = 90.0f;

        // Sıcaklık: |akım|^2 ile ısınır, ortam 28 °C'ye doğru soğur.
        const float dtS = static_cast<float>(dtMs) / 1000.0f;
        m_tempC += dtS * (currentA * currentA * 0.00004f - (m_tempC - 28.0f) * 0.004f);
        m_tempC = clampf(m_tempC, 20.0f, 60.0f);

        // Hücre gerilimi: LFP düz bölge 3,20-3,35 V + uçlar, yük altında 1,5 mΩ/hücre çökme.
        const float ocvCell = 3.00f + 0.0035f * m_socPct;  // %90 -> 3,315 V
        const float cellV = ocvCell + currentA * 0.0015f;    // deşarjda düşer
        const float packV = cellV * kCells;

        TelemetryData d = {};
        const float rpmF = speedKmh * 1000.0f / (60.0f * 3.14159265f * WHEEL_DIAMETER_M) *
                           (MOTOR_RPM_IS_WHEEL_RPM ? 1.0f : GEAR_RATIO);
        d.TEL_motorRpm = static_cast<uint16_t>(rpmF + 0.5f);
        d.TEL_motorVoltageDeciV = toU16((packV - 0.4f) * 10.0f);
        d.TEL_motorErrorFlags = 0;
        d.TEL_motorDataValid = true;
        d.TEL_motorTimeoutActive = false;

        const float cellMvAvg = cellV * 1000.0f;
        const float spreadMv = 12.0f + std::fabs(currentA) * 0.25f;  // yük altında açılır
        d.TEL_bmsCellVoltageAvgDeciMv = toU16(cellMvAvg * 10.0f);
        d.TEL_bmsCellVoltageMaxDeciMv = toU16((cellMvAvg + spreadMv * 0.5f) * 10.0f);
        d.TEL_bmsCellVoltageMinDeciMv = toU16((cellMvAvg - spreadMv * 0.5f) * 10.0f);
        for (int c = 0; c < kCells; ++c) {
            d.TEL_bmsCellVoltages[c] = toU16(cellMvAvg + noise(spreadMv * 0.4f));
        }
        d.TEL_cellVoltageDataValid = true;
        d.TEL_cellVoltageTimeoutActive = false;

        d.TEL_bmsSystemState = 0;  // 0 = kaynak yok; WiFi_Task SysStateDerive ile akımdan türetir (araçtaki gibi)
        d.TEL_bmsPackVoltageDeciV = toU16(packV * 10.0f);
        d.TEL_bmsCurrentCentiA = static_cast<int32_t>(std::lround(currentA * 100.0f));
        d.TEL_bmsSocHundredths = toU16(m_socPct * 100.0f);
        d.TEL_bmsSoc2Hundredths = d.TEL_bmsSocHundredths;
        d.TEL_bmsTempHighestC = static_cast<int8_t>(std::lround(m_tempC + 1.5f));
        d.TEL_bmsTempLowestC = static_cast<int8_t>(std::lround(m_tempC - 1.5f));
        d.TEL_bmsDataValid = true;
        d.TEL_bmsTimeoutActive = false;
        d.TEL_chargerActive = false;

        d.TEL_timestampMs = nowMs;
        d.TEL_speedKmhX10 = rpmToSpeedKmhX10(static_cast<int16_t>(d.TEL_motorRpm));
        return d;
    }

    float socPercent() const { return m_socPct; }

private:
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static uint16_t toU16(float v) {
        if (v <= 0.0f) return 0;
        if (v >= 65535.0f) return 65535;
        return static_cast<uint16_t>(v + 0.5f);
    }
    // [-amp, +amp] aralığında deterministik gürültü (Numerical Recipes LCG).
    float noise(float amp) {
        m_rng = m_rng * 1664525U + 1013904223U;
        const float u = static_cast<float>(m_rng >> 8) / 16777216.0f;  // 0..1
        return (u * 2.0f - 1.0f) * amp;
    }

    float m_socPct;
    float m_tempC = 30.0f;
    uint32_t m_rng;
    uint32_t m_lastMs = 0;
    bool m_hasLast = false;
};
