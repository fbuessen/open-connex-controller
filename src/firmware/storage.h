#pragma once
#include "domain.h"
#include "access.h"
#include <nvs.h>

namespace firmware {
enum class LoadResult { Valid, Missing, Error };
struct WifiCredentials {
  uint32_t schema = 1;
  char ssid[33] = {};
  char password[65] = {};
};
struct ZoneIcon { uint64_t id = 0; uint8_t icon = 0; };
struct UiPreferences { uint32_t schema = 1; ZoneIcon zones[connex::kZones]; };
class NvsStore : public connex::Storage, public connex::AccessStorage {
 public:
  LoadResult load(connex::Snapshot& snapshot);
  connex::SaveResult save(const connex::Snapshot& snapshot) override;
  LoadResult loadWifi(WifiCredentials& credentials);
  connex::SaveResult saveWifi(const WifiCredentials& credentials);
  LoadResult marker();
  bool markProvisioned();
  LoadResult loadAccess(connex::AccessRecord&);
  connex::SaveResult saveAccess(const connex::AccessRecord&) override;
  LoadResult loadUi(UiPreferences&);
  connex::SaveResult saveUi(const UiPreferences&);
};
class Entropy : public connex::Random {
 public:
  uint64_t next() override;
};
}  // namespace firmware
