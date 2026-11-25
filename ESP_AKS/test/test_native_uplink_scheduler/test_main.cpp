#include <unity.h>

// M1 — UplinkScheduler saf sınıf testleri (link FSM + offline örnekleme +
// replay politikası). LoraLink de aynı lib'de (lib/LoraLink) olduğundan bu
// paket derlenirken native-derlenebilirliği otomatik doğrulanır.
extern void test_rx_heartbeat_is_classified(void);
extern void test_rx_unknown_warns_once_then_quiet(void);
extern void test_boot_grace_link_down_when_no_heartbeat(void);
extern void test_tx_tick_aux_busy_keeps_buffer(void);
extern void test_tx_tick_one_replay_then_live(void);
extern void test_outage_offline_sampling_then_replay_drain(void);
extern void test_replay_every_n_ticks_alternates(void);
extern void test_live_retry_after_aux_busy(void);
extern void test_live_retry_cleared_by_next_tick(void);
extern void test_replay_every_n_ticks_zero_treated_as_one(void);
extern void test_firmware_config_replay_divider_is_contract_value(void);

void setUp(void) {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_rx_heartbeat_is_classified);
    RUN_TEST(test_rx_unknown_warns_once_then_quiet);
    RUN_TEST(test_boot_grace_link_down_when_no_heartbeat);
    RUN_TEST(test_tx_tick_aux_busy_keeps_buffer);
    RUN_TEST(test_live_retry_after_aux_busy);
    RUN_TEST(test_live_retry_cleared_by_next_tick);
    RUN_TEST(test_tx_tick_one_replay_then_live);
    RUN_TEST(test_outage_offline_sampling_then_replay_drain);
    RUN_TEST(test_replay_every_n_ticks_alternates);
    RUN_TEST(test_replay_every_n_ticks_zero_treated_as_one);
    RUN_TEST(test_firmware_config_replay_divider_is_contract_value);
    return UNITY_END();
}
