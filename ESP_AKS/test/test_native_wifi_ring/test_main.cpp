// Wi-Fi TEST telemetri yolu — TelLineRing (HTTP /telemetry.csv?since= sözleşmesi)
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "TelLineRing.h"

using Ring = TelLineRing<4, 32>;

static void pushSeq(Ring& r, uint32_t seq) {
    char line[32];
    const int n = snprintf(line, sizeof(line), "TEL,2,%lu\r\n", (unsigned long)seq);
    TEST_ASSERT_TRUE(r.push(seq, line, (size_t)n));
}

// Tüm satırları (ilk çağrı + devam çağrıları) tek string'de topla — httpd
// handler'ının yaptığı döngünün aynısı, küçük çıkış tamponuyla.
static std::string drain(const Ring& r, bool hasSince, uint32_t since,
                         size_t defaultLines, size_t chunk) {
    std::string out;
    char buf[128];
    size_t idx = r.startIndex(hasSince, since, defaultLines);
    uint32_t last = 0;
    while (true) {
        const size_t n = r.collect(idx, buf, chunk, &last);
        if (n == 0) break;
        out.append(buf, n);
        idx = r.indexAfter(last);
    }
    return out;
}

void setUp(void) {}
void tearDown(void) {}

void test_empty_ring_returns_nothing(void) {
    Ring r;
    TEST_ASSERT_TRUE(r.empty());
    TEST_ASSERT_EQUAL_STRING("", drain(r, false, 0, 10, 64).c_str());
    TEST_ASSERT_EQUAL_STRING("", drain(r, true, 5, 10, 64).c_str());
}

void test_overwrites_oldest_when_full(void) {
    Ring r;
    for (uint32_t s = 0; s < 6; ++s) pushSeq(r, s);
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)r.count());
    TEST_ASSERT_EQUAL_UINT32(2, r.oldestSeq());
    TEST_ASSERT_EQUAL_UINT32(5, r.newestSeq());
}

void test_no_since_returns_last_default_lines(void) {
    Ring r;
    for (uint32_t s = 0; s < 4; ++s) pushSeq(r, s);
    TEST_ASSERT_EQUAL_STRING("TEL,2,2\r\nTEL,2,3\r\n",
                             drain(r, false, 0, 2, 64).c_str());
}

void test_since_returns_only_newer_lines(void) {
    Ring r;
    for (uint32_t s = 10; s < 14; ++s) pushSeq(r, s);
    TEST_ASSERT_EQUAL_STRING("TEL,2,12\r\nTEL,2,13\r\n",
                             drain(r, true, 11, 99, 64).c_str());
}

void test_since_equal_newest_returns_nothing(void) {
    Ring r;
    for (uint32_t s = 10; s < 14; ++s) pushSeq(r, s);
    TEST_ASSERT_EQUAL_STRING("", drain(r, true, 13, 99, 64).c_str());
}

void test_since_older_than_buffer_returns_all(void) {
    Ring r;
    for (uint32_t s = 10; s < 14; ++s) pushSeq(r, s);
    TEST_ASSERT_EQUAL_STRING("TEL,2,10\r\nTEL,2,11\r\nTEL,2,12\r\nTEL,2,13\r\n",
                             drain(r, true, 3, 99, 64).c_str());
}

// ESP32 yeniden başladı: istemcinin since'i yeni seq'lerden büyük → hepsi.
void test_since_ahead_of_newest_means_reboot_returns_all(void) {
    Ring r;
    pushSeq(r, 0);
    pushSeq(r, 1);
    TEST_ASSERT_EQUAL_STRING("TEL,2,0\r\nTEL,2,1\r\n",
                             drain(r, true, 5000, 99, 64).c_str());
}

// Küçük çıkış tamponu: satırlar asla bölünmemeli, hepsi sırayla gelmeli.
void test_collect_never_splits_lines(void) {
    Ring r;
    for (uint32_t s = 100; s < 104; ++s) pushSeq(r, s);
    char buf[16];
    uint32_t last = 0;
    const size_t n = r.collect(0, buf, 15, &last);  // tek satır 11 B
    TEST_ASSERT_EQUAL_UINT32(11, (uint32_t)n);
    TEST_ASSERT_EQUAL_UINT32(100, last);
    TEST_ASSERT_EQUAL_STRING(
        "TEL,2,100\r\nTEL,2,101\r\nTEL,2,102\r\nTEL,2,103\r\n",
        drain(r, true, 99, 99, 15).c_str());
}

// Parça gönderimi sırasında ring ilerlerse indexAfter devam noktasını bulur.
void test_index_after_survives_wrap(void) {
    Ring r;
    for (uint32_t s = 0; s < 4; ++s) pushSeq(r, s);
    // istemciye 0..1 gönderildi, sonra 2 yeni satır geldi (0 ve 1 düştü)
    pushSeq(r, 4);
    pushSeq(r, 5);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)r.indexAfter(1));  // en eski = 2
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)r.indexAfter(3));  // 4'ün indeksi
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)r.indexAfter(5));  // yeni yok
}

void test_rejects_overlong_and_empty_lines(void) {
    Ring r;
    char big[40];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_FALSE(r.push(1, big, sizeof(big)));
    TEST_ASSERT_FALSE(r.push(1, big, 0));
    TEST_ASSERT_FALSE(r.push(1, nullptr, 5));
    TEST_ASSERT_TRUE(r.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_ring_returns_nothing);
    RUN_TEST(test_overwrites_oldest_when_full);
    RUN_TEST(test_no_since_returns_last_default_lines);
    RUN_TEST(test_since_returns_only_newer_lines);
    RUN_TEST(test_since_equal_newest_returns_nothing);
    RUN_TEST(test_since_older_than_buffer_returns_all);
    RUN_TEST(test_since_ahead_of_newest_means_reboot_returns_all);
    RUN_TEST(test_collect_never_splits_lines);
    RUN_TEST(test_index_after_survives_wrap);
    RUN_TEST(test_rejects_overlong_and_empty_lines);
    return UNITY_END();
}
