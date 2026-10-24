#include "storage.h"
#include <esp_random.h>
#include <esp_crc.h>
#include <string.h>
#include <type_traits>

namespace firmware {
namespace {
constexpr uint32_t magic = 0x434e5831;
template<class T> struct Blob { uint32_t tag, size; T value; uint32_t checksum; };

template<class T> LoadResult read(const char* space, T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "NVS records must be POD");
  nvs_handle_t handle;
  esp_err_t err = nvs_open(space, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return LoadResult::Missing;
  if (err != ESP_OK) return LoadResult::Error;
  Blob<T> blob{};
  size_t size = sizeof blob;
  err = nvs_get_blob(handle, "record", &blob, &size);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return LoadResult::Missing;
  if (err != ESP_OK || size != sizeof blob || blob.tag != magic || blob.size != sizeof(T) ||
      blob.checksum != esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&blob.value), sizeof(T)))
    return LoadResult::Error;
  memcpy(&value, &blob.value, sizeof value);
  return LoadResult::Valid;
}
template<class T> connex::SaveResult write(const char* space, const T& value) {
  nvs_handle_t handle;
  if (nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return connex::SaveResult::Rejected;
  Blob<T> blob{};
  blob.tag = magic; blob.size = sizeof(T); memcpy(&blob.value, &value, sizeof value);
  blob.checksum = esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&blob.value), sizeof(T));
  const esp_err_t written = nvs_set_blob(handle, "record", &blob, sizeof blob);
  const esp_err_t committed = written == ESP_OK ? nvs_commit(handle) : written;
  nvs_close(handle);
  T check{};
  if (written != ESP_OK || committed != ESP_OK || read(space, check) != LoadResult::Valid ||
      memcmp(&value, &check, sizeof value)) return connex::SaveResult::Uncertain;
  return connex::SaveResult::Saved;
}
}
LoadResult NvsStore::load(connex::Snapshot& snapshot) {
  const auto result = read("connex_app", snapshot);
  return result == LoadResult::Valid && !connex::validSnapshot(snapshot) ? LoadResult::Error : result;
}
connex::SaveResult NvsStore::save(const connex::Snapshot& snapshot) { return write("connex_app", snapshot); }
LoadResult NvsStore::loadWifi(WifiCredentials& c) {
  const auto result = read("connex_wifi", c);
  if (result != LoadResult::Valid) return result;
  if (c.schema != 1 || strnlen(c.ssid, sizeof c.ssid) == sizeof c.ssid ||
      strnlen(c.password, sizeof c.password) == sizeof c.password || (!c.ssid[0] && c.password[0]))
    return LoadResult::Error;
  return LoadResult::Valid;
}
connex::SaveResult NvsStore::saveWifi(const WifiCredentials& c) { return write("connex_wifi", c); }
LoadResult NvsStore::marker() {
  uint32_t value = 0;
  const auto result = read("connex_meta", value);
  return result == LoadResult::Valid && value != 1 ? LoadResult::Error : result;
}
bool NvsStore::markProvisioned() { return write("connex_meta", uint32_t{1}) == connex::SaveResult::Saved; }
LoadResult NvsStore::loadAccess(connex::AccessRecord& access) {
  const auto result = read("connex_access", access);
  uint32_t marker = 0;
  const auto marked = read("connex_authmark", marker);
  if (marked == LoadResult::Error || (marked == LoadResult::Valid && marker != 1)) return LoadResult::Error;
  if (result == LoadResult::Missing) return marked == LoadResult::Missing ? result : LoadResult::Error;
  if (result != LoadResult::Valid || !connex::validAccess(access)) return LoadResult::Error;
  if (marked == LoadResult::Missing && write("connex_authmark", uint32_t{1}) != connex::SaveResult::Saved)
    return LoadResult::Error;
  return LoadResult::Valid;
}
connex::SaveResult NvsStore::saveAccess(const connex::AccessRecord& access) {
  const auto result = write("connex_access", access);
  if (result != connex::SaveResult::Saved) return result;
  return write("connex_authmark", uint32_t{1}) == connex::SaveResult::Saved
      ? connex::SaveResult::Saved : connex::SaveResult::Uncertain;
}
LoadResult NvsStore::loadUi(UiPreferences& ui) {
  const auto result = read("connex_ui", ui);
  if (result != LoadResult::Valid) return result;
  if (ui.schema != 1) return LoadResult::Error;
  for (const auto& z : ui.zones) if (z.icon > 5) return LoadResult::Error;
  return result;
}
connex::SaveResult NvsStore::saveUi(const UiPreferences& ui) { return write("connex_ui", ui); }
uint64_t Entropy::next() { uint64_t n; esp_fill_random(&n, sizeof n); return n; }
}  // namespace firmware
