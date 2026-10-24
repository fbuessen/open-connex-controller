#pragma once

#include <stddef.h>
#include <stdint.h>

namespace connex {
constexpr size_t kZones = 8;
constexpr size_t kQueue = 16;
constexpr uint32_t kDispatchGraceMs = 10000;
constexpr uint8_t kControlSends = 5;
constexpr uint32_t kControlIntervalMs = 1000;
constexpr uint32_t kRefreshIntervalMs = 60 * 60 * 1000;
constexpr uint32_t kPairingIntervalMs = 5000;
constexpr size_t kRetired = 64;
constexpr uint32_t kSchema = 1;

struct Configuration {
  uint16_t temperature = 0;  // Tenths of the selected unit; zero is never sent.
  uint8_t hasTarget = 0;
  uint8_t celsius = 1;
  uint8_t eco = 0;
  int8_t offset = 0;
};

struct Zone {
  uint64_t id = 0;
  uint16_t prefix = 0;
  uint8_t index = 0;
  uint8_t active = 0;
  char name[65] = {};
  Configuration configuration;
};

struct Retired {
  uint64_t id = 0;
  uint16_t prefix = 0;
  uint8_t index = 0;
};

struct Snapshot {
  uint32_t schema = kSchema;
  uint64_t deviceId = 0;
  uint16_t retiredCount = 0;
  Zone zones[kZones];
  Retired retired[kRetired];
};

enum class SaveResult { Saved, Rejected, Uncertain };
struct Storage {
  virtual ~Storage() = default;
  virtual SaveResult save(const Snapshot&) = 0;
};
struct Random {
  virtual ~Random() = default;
  virtual uint64_t next() = 0;
};

enum class Result { Ok, Invalid, Missing, Conflict, Full, Unavailable, Uncertain };
enum Field : uint8_t { Name = 1, Target = 2, Unit = 4, Mode = 8, Offset = 16 };
struct Patch {
  uint8_t fields = 0;
  char name[65] = {};
  Configuration configuration;
  bool controls() const { return fields & (Target | Unit | Mode | Offset); }
};
struct Command {
  uint64_t zoneId = 0;
  uint8_t bytes[10] = {};
  bool pairing = false;
};

bool validName(const char* name);
bool validConfiguration(const Configuration& c);
bool validSnapshot(const Snapshot& s);
void encode(const Zone& zone, uint8_t sequence, uint8_t out[10]);
void formatId(const char* prefix, uint64_t id, char* out, size_t size);
bool parseZoneId(const char* text, uint64_t& id);
inline bool reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// External serialization: the platform owns the mutex. No RF or socket waits here.
class Controller {
 public:
  Controller(Storage& storage, Random& random) : storage_(storage), random_(random) {}
  bool load(const Snapshot& snapshot);
  Result initialize();
  const Snapshot& snapshot() const { return snapshot_; }
  const Zone* find(uint64_t id) const;
  bool healthy() const { return ready_ && radioReady_ && enabled_; }
  bool storageReady() const { return ready_; }
  bool enabled() const { return enabled_; }
  void setEnabled(bool enabled);
  void setRadioReady(bool ready);
  Result create(const char* name, uint64_t& id);
  Result patch(uint64_t id, const Patch& patch, uint32_t now);
  Result remove(uint64_t id);
  Result pair(uint64_t id, uint32_t seconds, uint32_t now);
  Result cancelPairing(uint64_t id);
  bool take(uint32_t now, Command& command);
  void complete(bool localFault);
  bool busy(uint64_t id) const;
  size_t queued() const { return count_; }

 private:
  struct Pairing { bool active = false; uint32_t end = 0; uint32_t next = 0; };
  struct Pending { Command command; uint32_t due = 0; uint8_t remaining = 1; };
  struct Refresh { bool armed = false; uint32_t due = 0; };
  Result commit(const Snapshot& next);
  bool pairing(uint64_t id) const;
  int slot(uint64_t id) const;
  bool unique(uint64_t id, uint16_t prefix) const;
  int pendingControl(uint64_t id) const;
  void enqueue(const Zone& zone, bool pairing, uint32_t due, uint8_t sends = 1);
  Storage& storage_;
  Random& random_;
  Snapshot snapshot_;
  Pairing pairs_[kZones];
  Refresh refresh_[kZones];
  Pending queue_[kQueue];
  Command flight_;
  size_t count_ = 0, cursor_ = 0;
  uint8_t sequence_ = 0;
  bool ready_ = false, radioReady_ = false, inFlight_ = false, enabled_ = true;
};

enum class ButtonEvent { None, Toggle, Reset };
class SetupButton {
 public:
  ButtonEvent sample(bool low, uint32_t now);
 private:
  bool raw_ = false, stable_ = false, fired_ = false;
  uint32_t changed_ = 0, pressed_ = 0;
};

// Pure Wi-Fi admission/timing policy, shared by the platform and native tests.
enum class NetworkState { Setup, Trial, Grace, Configured, Editing, Resetting, Recovery };
class NetworkPolicy {
 public:
  NetworkState state = NetworkState::Recovery;
  uint32_t generation = 0, deadline = 0;
  bool submit(uint32_t now);
  bool saved(uint32_t attempt, uint32_t now);
  bool failed(uint32_t attempt);
  bool expired(uint32_t now) const { return reached(now, deadline); }
  void reset();
  void resetComplete(bool verified);
};
}  // namespace connex
