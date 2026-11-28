// Wi-Fi TEST yolu — TelSimulator (ESP32-S3 test kartının sahte araç verisi)
//
// Amaç: S3'te üretilen sahte veri, UKS'in Decode_Line aralık kapılarından
// GEÇMELİ (aksi halde zincirin sonundaki arayüz hiç veri göstermez) ve
// senaryo (bekleme/hızlanma/seyir/fren) beklenen yönde davranmalı.
#include <unity.h>

#include <cstdint>

#include "SysStateDerive.h"
#include "TelSimulator.h"
#include "TelemetrySanitize.h"

void setUp() {}
void tearDown() {}

// UKS telemetry.c::Decode_Line'daki Parse_Int sınırları (TEL v2).
static void assertWithinUksRanges(const TelemetryData& raw) {
    TelemetryData d = raw;
    SysStateDerive::applyIfEnabled(d);
    d = TelemetrySanitize::sanitizeForUplink(d);
    TEST_ASSERT_TRUE(d.TEL_motorRpm <= 20000);
    TEST_ASSERT_TRUE(d.TEL_motorVoltageDeciV <= 32767);
    TEST_ASSERT_TRUE(d.TEL_bmsTempHighestC >= -128 && d.TEL_bmsTempHighestC <= 127);
    TEST_ASSERT_TRUE(d.TEL_bmsTempLowestC <= d.TEL_bmsTempHighestC);
    TEST_ASSERT_TRUE(d.TEL_bmsSystemState >= 1 && d.TEL_bmsSystemState <= 4);
    TEST_ASSERT_TRUE(d.TEL_bmsSocHundredths <= 10000);
    TEST_ASSERT_TRUE(d.TEL_speedKmhX10 <= 3000);
    TEST_ASSERT_TRUE(d.TEL_bmsCellVoltageMinDeciMv <= d.TEL_bmsCellVoltageMaxDeciMv);
}

static void test_all_samples_pass_uks_ranges_over_two_cycles() {
    TelSimulator sim;
    for (uint32_t ms = 0; ms < 2 * TelSimulator::kCycleMs; ms += 200) {
        assertWithinUksRanges(sim.next(ms));
    }
}

static void test_profile_phases() {
    float v = 0, i = 0;
    TelSimulator::profile(5000, &v, &i);  // bekleme
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
    TEST_ASSERT_TRUE(i < 0.0f && i > -1.0f);

    TelSimulator::profile(20000, &v, &i);  // hızlanma ortası: yüksek deşarj
    TEST_ASSERT_TRUE(v > 15.0f && v < 30.0f);
    TEST_ASSERT_TRUE(i < -40.0f);

    TelSimulator::profile(50000, &v, &i);  // seyir
    TEST_ASSERT_TRUE(v >= 40.0f && v <= 50.0f);
    TEST_ASSERT_TRUE(i <= -12.0f && i >= -20.0f);

    TelSimulator::profile(85000, &v, &i);  // fren: rejeneratif (pozitif) akım
    TEST_ASSERT_TRUE(v > 3.0f);
    TEST_ASSERT_TRUE(i > 0.0f);

    TelSimulator::profile(TelSimulator::kCycleMs + 5000, &v, &i);  // döngü tekrarlar
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
}

static void test_sysstate_derived_like_vehicle() {
    TelSimulator sim;
    TelemetryData d = {};
    for (uint32_t ms = 0; ms <= 20000; ms += 200) d = sim.next(ms);
    TEST_ASSERT_EQUAL_UINT8(0, d.TEL_bmsSystemState);  // simülatör kaynak vermez
    SysStateDerive::applyIfEnabled(d);
    TEST_ASSERT_EQUAL_UINT8(1, d.TEL_bmsSystemState);  // hızlanmada Deşarj
}

static void test_soc_drops_over_a_cycle() {
    TelSimulator sim(90.0f);
    for (uint32_t ms = 0; ms <= TelSimulator::kCycleMs; ms += 200) sim.next(ms);
    TEST_ASSERT_TRUE(sim.socPercent() < 90.0f);
    TEST_ASSERT_TRUE(sim.socPercent() > 89.0f);  // 2 dk'da ~%0,3 — gerçekçi
}

static void test_speed_consistent_with_rpm() {
    TelSimulator sim;
    TelemetryData d = {};
    for (uint32_t ms = 0; ms <= 50000; ms += 200) d = sim.next(ms);
    TEST_ASSERT_EQUAL_UINT16(rpmToSpeedKmhX10(static_cast<int16_t>(d.TEL_motorRpm)),
                             d.TEL_speedKmhX10);
    TEST_ASSERT_TRUE(d.TEL_speedKmhX10 >= 380 && d.TEL_speedKmhX10 <= 520);
}

static void test_pack_voltage_in_real_pack_range() {
    TelSimulator sim;
    for (uint32_t ms = 0; ms < TelSimulator::kCycleMs; ms += 200) {
        const TelemetryData d = sim.next(ms);
        TEST_ASSERT_TRUE(d.TEL_bmsPackVoltageDeciV >= 600 && d.TEL_bmsPackVoltageDeciV <= 876);
    }
}

static void test_deterministic_for_same_seed() {
    TelSimulator a(90.0f, 7U), b(90.0f, 7U);
    for (uint32_t ms = 0; ms < 10000; ms += 200) {
        const TelemetryData x = a.next(ms), y = b.next(ms);
        TEST_ASSERT_EQUAL_INT32(x.TEL_bmsCurrentCentiA, y.TEL_bmsCurrentCentiA);
        TEST_ASSERT_EQUAL_UINT16(x.TEL_speedKmhX10, y.TEL_speedKmhX10);
    }
}

static void test_timestamp_passthrough() {
    TelSimulator sim;
    TEST_ASSERT_EQUAL_UINT32(1234U, sim.next(1234U).TEL_timestampMs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_all_samples_pass_uks_ranges_over_two_cycles);
    RUN_TEST(test_profile_phases);
    RUN_TEST(test_sysstate_derived_like_vehicle);
    RUN_TEST(test_soc_drops_over_a_cycle);
    RUN_TEST(test_speed_consistent_with_rpm);
    RUN_TEST(test_pack_voltage_in_real_pack_range);
    RUN_TEST(test_deterministic_for_same_seed);
    RUN_TEST(test_timestamp_passthrough);
    return UNITY_END();
}
