#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// TelLineRing — Wi-Fi TEST telemetri yolu (WIFI_TELEMETRY_ENABLED) için son N
// TEL satırının sabit boyutlu halka tamponu. SAF: donanım/FreeRTOS bağımlılığı
// YOK ve thread-safe DEĞİL — çağıran (src/wifi_telemetry.cpp) bir mutex ile
// korur. Native test: test/test_native_wifi_ring.
//
// HTTP `GET /telemetry.csv?since=<seq>` sözleşmesi bu sınıfın collect()
// davranışıdır:
//   * since verilmemiş  → son `defaultLines` satır.
//   * since < en yeni   → seq'i since'ten BÜYÜK olan satırlar (tampondan
//                          düşmüş olanlar geri gelmez; istemci seq boşluğunu
//                          görür).
//   * since >= en yeni+1 değil ama since > en yeni → ESP32 yeniden başlamış
//                          (seq 0'dan saymaya döndü) demektir: istemci takılı
//                          kalmasın diye tamponun TAMAMI döner.
// Satırlar tam satır olarak kopyalanır (yarım satır asla yazılmaz).
template <size_t N, size_t LineMax>
class TelLineRing {
   public:
    static_assert(N > 0, "N > 0 olmali");
    static_assert(LineMax > 2, "LineMax en az CRLF sigdirmali");

    // Satırı (CRLF dahil, NUL'suz uzunluk `len`) ekler. LineMax'ten uzun satır
    // REDDEDİLİR (false) — kesik satır yayınlamaktansa hiç yayınlamamak.
    bool push(uint32_t seq, const char* line, size_t len) {
        if (line == nullptr || len == 0 || len > LineMax) return false;
        Entry& e = entries_[head_];
        e.seq = seq;
        e.len = static_cast<uint16_t>(len);
        std::memcpy(e.text, line, len);
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
        return true;
    }

    size_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    uint32_t newestSeq() const { return at(count_ - 1).seq; }  // empty() ise anlamsız
    uint32_t oldestSeq() const { return at(0).seq; }           // empty() ise anlamsız

    // Bir HTTP isteğinin "başlangıç imleci"ni (ring içi mantıksal indeks)
    // döndürür; collect() bu imleçten devam eder. Bkz. sınıf yorumu.
    size_t startIndex(bool hasSince, uint32_t since, size_t defaultLines) const {
        if (count_ == 0) return 0;
        if (!hasSince) {
            return (count_ > defaultLines) ? (count_ - defaultLines) : 0;
        }
        if (since > newestSeq()) return 0;  // ESP32 reboot → hepsini ver
        return indexAfter(since);           // count_ → istemci güncel
    }

    // `cursorSeq` (son gönderilen satırın seq'i) sonrasındaki satırları `out`a
    // sığdığı kadar TAM satır olarak kopyalar. İlk çağrıda `first=true` ve
    // `startIdx` startIndex()'ten gelir; sonraki çağrılar (tampon mutex
    // dışında gönderilirken ring ilerlemiş olabilir) seq ile devam eder.
    // Döner: yazılan bayt; `*lastSeq` son kopyalanan satırın seq'i (0 bayt
    // yazıldıysa dokunulmaz).
    size_t collect(size_t startIdx, char* out, size_t outLen,
                   uint32_t* lastSeq) const {
        size_t written = 0;
        for (size_t i = startIdx; i < count_; ++i) {
            const Entry& e = at(i);
            if (written + e.len > outLen) break;
            std::memcpy(out + written, e.text, e.len);
            written += e.len;
            if (lastSeq != nullptr) *lastSeq = e.seq;
        }
        return written;
    }

    // Parça parça gönderimde devam imleci: seq'i `afterSeq`ten büyük ilk
    // satırın indeksi (yoksa count()). Ring arada sarmış olsa da doğru çalışır.
    size_t indexAfter(uint32_t afterSeq) const {
        for (size_t i = 0; i < count_; ++i) {
            if (at(i).seq > afterSeq) return i;
        }
        return count_;
    }

   private:
    struct Entry {
        uint32_t seq;
        uint16_t len;
        char text[LineMax];
    };

    // Mantıksal indeks i (0 = en eski) → fiziksel giriş.
    const Entry& at(size_t i) const {
        const size_t oldest = (head_ + N - count_) % N;
        return entries_[(oldest + i) % N];
    }

    Entry entries_[N] = {};
    size_t head_ = 0;   // bir sonraki yazılacak fiziksel indeks
    size_t count_ = 0;
};
