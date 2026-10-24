#include "security.h"
#include "app.h"
#include "network.h"
#include <esp_random.h>
#include <mbedtls/pkcs5.h>
#include <string.h>
namespace firmware {
namespace {
// Keep the library PBKDF2 implementation and stored credential format intact.
// The idle-priority worker shares core 0 with IDLE0 under the pinned scheduler's
// time slicing; the HTTP task sleeps rather than starving the watchdog's task.
static_assert(configUSE_TIME_SLICING == 1, "Password worker requires time slicing");
class Crypto : public connex::AccessCrypto {
  SemaphoreHandle_t requested_ = nullptr, finished_ = nullptr;
  std::atomic<bool> busy_{false};
  char password_[129] = {};
  uint8_t salt_[16] = {}, output_[32] = {};
  uint32_t iterations_ = 0;
  bool result_ = false;

  void clearJob() {
    connex::wipe(password_, sizeof password_); connex::wipe(salt_, sizeof salt_);
    connex::wipe(output_, sizeof output_);
  }
  bool claim() {
    bool expected = false;
    return requested_ && busy_.compare_exchange_strong(expected, true);
  }
  void run() {
    for (;;) {
      xSemaphoreTake(requested_, portMAX_DELAY);
      result_ = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
          reinterpret_cast<const unsigned char*>(password_), strlen(password_),
          salt_, sizeof salt_, iterations_, sizeof output_, output_) == 0;
      // The caller owns the result until it copies and wipes it. Do not release
      // the slot here: another request must never overwrite an unread result.
      xSemaphoreGive(finished_);
    }
  }
 public:
  bool begin() {
    requested_ = xSemaphoreCreateBinary(); finished_ = xSemaphoreCreateBinary();
    if (requested_ && finished_ && xTaskCreatePinnedToCore([](void* self) {
          static_cast<Crypto*>(self)->run();
        }, "connex-password", 6144, this, tskIDLE_PRIORITY, nullptr, 0) == pdPASS) return true;
    if (requested_) vSemaphoreDelete(requested_);
    if (finished_) vSemaphoreDelete(finished_);
    requested_ = finished_ = nullptr; return false;
  }
  void random(void* out, size_t size) override { esp_fill_random(out, size); }
  bool derive(const char* password, const uint8_t salt[16], uint32_t iterations, uint8_t out[32]) override {
    const size_t length = strnlen(password, sizeof password_);
    if (length >= sizeof password_ || !claim()) return false;
    memcpy(password_, password, length + 1); memcpy(salt_, salt, sizeof salt_);
    iterations_ = iterations;
    xSemaphoreGive(requested_); xSemaphoreTake(finished_, portMAX_DELAY);
    const bool result = result_;
    if (result) memcpy(out, output_, sizeof output_);
    clearJob(); busy_ = false; return result;
  }
  bool busy() const { return busy_.load(); }
} crypto;
}
connex::Access access{storage, crypto};
SemaphoreHandle_t accessMutex = nullptr;
bool beginSecurity() {
  accessMutex = xSemaphoreCreateMutex();
  if (!accessMutex || !crypto.begin()) return false;
  connex::AccessRecord record;
  const auto loaded = storage.loadAccess(record);
  if (loaded == LoadResult::Error) return false;
  const bool ready = access.load(record);
  connex::wipe(&record, sizeof record);
  return ready;
}
void pollSecurity() {
  // Expire idle entries continuously so an untouched cookie cannot revive after
  // a full millis() rollover. Never block network/reset service behind hashing.
  if (accessMutex && xSemaphoreTake(accessMutex, 0) == pdTRUE) {
    access.expire(millis()); xSemaphoreGive(accessMutex);
  }
}
bool hashBusy() { return crypto.busy(); }
}
