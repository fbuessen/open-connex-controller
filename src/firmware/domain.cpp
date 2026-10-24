#include "domain.h"

#include <stdio.h>
#include <string.h>

namespace connex {
bool validName(const char* name) {
  const size_t length = strnlen(name, 65);
  if (!length || length > 64) return false;
  bool visible = false;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(name[i]);
    if (c < 32 || c == 127) return false;
    if (c != ' ') visible = true;
    if (c < 128) continue;
    // Reject malformed/overlong UTF-8 and surrogate code points.
    unsigned extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x7F >> extra);
    if (c < 0xC2 || c > 0xF4 || i + extra >= length) return false;
    for (unsigned j = 0; j < extra; ++j) {
      const uint8_t next = static_cast<uint8_t>(name[++i]);
      if ((next & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (next & 63);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
  }
  return visible;
}

bool validConfiguration(const Configuration& c) {
  if (c.hasTarget > 1 || c.celsius > 1 || c.eco > 1 || c.offset < -9 || c.offset > 9) return false;
  if (!c.hasTarget) return c.temperature == 0;
  const unsigned low = c.celsius ? 100 : 500;
  const unsigned high = c.celsius ? 300 : 860;
  return c.temperature >= low && c.temperature <= high && c.temperature % 5 == 0;
}

bool validSnapshot(const Snapshot& s) {
  if (s.schema != kSchema || !s.deviceId || s.retiredCount > kRetired) return false;
  for (size_t i = 0; i < kZones; ++i) {
    const Zone& z = s.zones[i];
    if (z.active > 1) return false;
    if (!z.active) continue;
    if (!z.id || z.index != i || !validName(z.name) || !validConfiguration(z.configuration)) return false;
    for (size_t j = 0; j < i; ++j)
      if (s.zones[j].active && (s.zones[j].id == z.id || s.zones[j].prefix == z.prefix)) return false;
    for (size_t j = 0; j < s.retiredCount; ++j)
      if (s.retired[j].id == z.id || s.retired[j].prefix == z.prefix) return false;
  }
  for (size_t i = 0; i < s.retiredCount; ++i) {
    if (!s.retired[i].id || s.retired[i].index >= kZones) return false;
    for (size_t j = 0; j < i; ++j)
      if (s.retired[j].id == s.retired[i].id || s.retired[j].prefix == s.retired[i].prefix) return false;
  }
  return true;
}

void encode(const Zone& z, uint8_t sequence, uint8_t out[10]) {
  out[0] = z.prefix >> 8; out[1] = z.prefix;
  out[2] = sequence; out[3] = 1; out[4] = z.index;
  out[5] = z.configuration.temperature >> 8; out[6] = z.configuration.temperature;
  out[7] = z.configuration.offset + 9;
  out[8] = z.configuration.celsius | (z.configuration.eco << 1); out[9] = 0;
}

void formatId(const char* prefix, uint64_t id, char* out, size_t size) {
  snprintf(out, size, "%s%08lx%08lx", prefix,
           static_cast<unsigned long>(id >> 32), static_cast<unsigned long>(id & 0xffffffff));
}
bool parseZoneId(const char* text, uint64_t& id) {
  if (strlen(text) != 21 || strncmp(text, "zone_", 5)) return false;
  id = 0;
  for (size_t i = 5; i < 21; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    id = (id << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return id != 0;
}

bool Controller::load(const Snapshot& s) {
  ready_ = validSnapshot(s);
  count_ = 0; inFlight_ = false;
  for (auto& p : pairs_) p = Pairing{};
  for (auto& r : refresh_) r = Refresh{};
  if (ready_) snapshot_ = s;
  return ready_;
}
Result Controller::initialize() {
  Snapshot s{};
  for (unsigned i = 0; i < 32 && !s.deviceId; ++i) s.deviceId = random_.next();
  if (!s.deviceId) return Result::Unavailable;
  const Result r = commit(s);
  if (r == Result::Ok) ready_ = true;
  return r;
}
Result Controller::commit(const Snapshot& next) {
  const SaveResult saved = storage_.save(next);
  if (saved == SaveResult::Rejected) return Result::Unavailable;
  if (saved == SaveResult::Uncertain) {
    ready_ = false; count_ = 0;
    for (auto& p : pairs_) p.active = false;
    return Result::Uncertain;
  }
  snapshot_ = next;
  return Result::Ok;
}
int Controller::slot(uint64_t id) const {
  for (size_t i = 0; i < kZones; ++i)
    if (snapshot_.zones[i].active && snapshot_.zones[i].id == id) return i;
  return -1;
}
const Zone* Controller::find(uint64_t id) const {
  const int i = slot(id);
  return i < 0 ? nullptr : &snapshot_.zones[i];
}
bool Controller::unique(uint64_t id, uint16_t prefix) const {
  if (!id) return false;
  for (const auto& z : snapshot_.zones)
    if (z.active && (z.id == id || z.prefix == prefix)) return false;
  for (size_t i = 0; i < snapshot_.retiredCount; ++i)
    if (snapshot_.retired[i].id == id || snapshot_.retired[i].prefix == prefix) return false;
  return true;
}
Result Controller::create(const char* name, uint64_t& id) {
  if (!ready_) return Result::Unavailable;
  if (snapshot_.retiredCount == kRetired) return Result::Conflict;
  if (!validName(name)) return Result::Invalid;
  for (size_t i = 0; i < kZones; ++i) {
    if (snapshot_.zones[i].active) continue;
    Snapshot next = snapshot_;
    Zone z{};
    z.active = 1; z.index = i;
    z.configuration.temperature = 200;  // New zones are immediately pairable at 20°C.
    z.configuration.hasTarget = 1;
    bool allocated = false;
    for (unsigned tries = 0; tries < 64; ++tries) {
      z.id = random_.next(); z.prefix = random_.next();
      if (unique(z.id, z.prefix)) { allocated = true; break; }
    }
    if (!allocated) return Result::Unavailable;
    memcpy(z.name, name, strlen(name) + 1);
    next.zones[i] = z;
    const Result result = commit(next);
    if (result == Result::Ok) { id = z.id; refresh_[i] = Refresh{}; }
    return result;
  }
  return Result::Conflict;
}
bool Controller::pairing(uint64_t id) const {
  const int i = slot(id);
  return i >= 0 && pairs_[i].active;
}
bool Controller::busy(uint64_t id) const {
  if (pairing(id) || (inFlight_ && flight_.zoneId == id)) return true;
  for (size_t i = 0; i < count_; ++i)
    if (queue_[i].command.zoneId == id) return true;
  return false;
}
int Controller::pendingControl(uint64_t id) const {
  for (size_t i = 0; i < count_; ++i) {
    const Command& c = queue_[i].command;
    if (c.zoneId == id && !c.pairing) return static_cast<int>(i);
  }
  return -1;
}
void Controller::enqueue(const Zone& z, bool pair, uint32_t due, uint8_t sends) {
  const int existing = pair ? -1 : pendingControl(z.id);
  Pending& pending = queue_[existing >= 0 ? static_cast<size_t>(existing) : count_++];
  Command& c = pending.command;
  c.zoneId = z.id; c.pairing = pair;
  encode(z, 0, c.bytes);  // Assign a sequence only when this snapshot starts dispatch.
  pending.due = due;
  pending.remaining = sends;
}
Result Controller::patch(uint64_t id, const Patch& patch, uint32_t now) {
  if (!ready_) return Result::Unavailable;
  const int i = slot(id);
  if (i < 0) return Result::Missing;
  if (!patch.fields || (patch.fields & ~31)) return Result::Invalid;
  if (pairing(id)) return Result::Conflict;
  Snapshot next = snapshot_;
  Zone& z = next.zones[i];
  if (patch.fields & Name) {
    if (!validName(patch.name)) return Result::Invalid;
    memcpy(z.name, patch.name, sizeof z.name);
  }
  if (patch.fields & Target) {
    z.configuration.temperature = patch.configuration.temperature;
    z.configuration.hasTarget = patch.configuration.hasTarget;
  }
  if (patch.fields & Unit) z.configuration.celsius = patch.configuration.celsius;
  if (patch.fields & Mode) z.configuration.eco = patch.configuration.eco;
  if (patch.fields & Offset) z.configuration.offset = patch.configuration.offset;
  if (!validConfiguration(z.configuration) || (patch.controls() && !z.configuration.hasTarget)) return Result::Invalid;
  if (patch.controls() && (!radioReady_ || !enabled_)) return Result::Unavailable;
  if (patch.controls() && pendingControl(id) < 0 && count_ == kQueue) return Result::Full;
  // Queue admission is reserved by the caller's service mutex until publication.
  if (memcmp(&z, &snapshot_.zones[i], sizeof z)) {
    const Result result = commit(next);
    if (result != Result::Ok) return result;
  }
  if (patch.controls()) enqueue(snapshot_.zones[i], false, now + kDispatchGraceMs, kControlSends);
  return Result::Ok;
}
Result Controller::remove(uint64_t id) {
  if (!ready_) return Result::Unavailable;
  const int i = slot(id);
  if (i < 0) return Result::Missing;
  if (busy(id) || snapshot_.retiredCount == kRetired) return Result::Conflict;
  Snapshot next = snapshot_;
  Retired& r = next.retired[next.retiredCount++];
  r.id = id; r.prefix = next.zones[i].prefix; r.index = i;
  next.zones[i] = Zone{};
  const Result result = commit(next);
  if (result == Result::Ok) refresh_[i] = Refresh{};
  return result;
}
Result Controller::pair(uint64_t id, uint32_t seconds, uint32_t now) {
  if (!healthy()) return Result::Unavailable;
  const int i = slot(id);
  if (i < 0) return Result::Missing;
  if (!seconds || seconds > 60 || !snapshot_.zones[i].configuration.hasTarget) return Result::Invalid;
  if (busy(id)) return Result::Conflict;
  if (count_ == kQueue) return Result::Full;
  pairs_[i] = {true, now + seconds * 1000, now + kPairingIntervalMs};
  enqueue(snapshot_.zones[i], true, now);
  return Result::Ok;
}
Result Controller::cancelPairing(uint64_t id) {
  const int i = slot(id);
  if (i < 0) return Result::Missing;
  pairs_[i].active = false;
  size_t kept = 0;
  for (size_t j = 0; j < count_; ++j) {
    const Pending pending = queue_[j];
    if (pending.command.zoneId == id && pending.command.pairing) continue;
    queue_[kept++] = pending;
  }
  count_ = kept;
  return Result::Ok;
}
bool Controller::take(uint32_t now, Command& command) {
  if (!healthy()) return false;
  for (size_t n = 0; n < kZones; ++n) {
    const size_t i = (cursor_ + n) % kZones;
    auto& p = pairs_[i];
    if (!p.active) continue;
    if (reached(now, p.end)) { cancelPairing(snapshot_.zones[i].id); continue; }
    if (reached(now, p.next) && count_ < kQueue) {
      bool pending = inFlight_ && flight_.zoneId == snapshot_.zones[i].id;
      for (size_t j = 0; j < count_; ++j)
        pending |= queue_[j].command.zoneId == snapshot_.zones[i].id;
      if (!pending) { enqueue(snapshot_.zones[i], true, now); p.next = now + kPairingIntervalMs; }
    }
  }
  cursor_ = (cursor_ + 1) % kZones;
  // Start idle-zone clocks lazily: boot/create/recovery never send immediately.
  // Active work supersedes a refresh; its actual dispatch re-arms the hour.
  for (const auto& z : snapshot_.zones) {
    if (!z.active || !z.configuration.hasTarget) continue;
    auto& r = refresh_[z.index];
    if (!r.armed) r = {true, now + kRefreshIntervalMs};
    if (reached(now, r.due) && !busy(z.id) && count_ < kQueue)
      enqueue(z, false, now);
  }
  if (inFlight_ || !count_) return false;
  // A zone still being edited must not hold up eligible work for another zone.
  size_t next = 0;
  while (next < count_ && !reached(now, queue_[next].due)) ++next;
  if (next == count_) return false;
  Pending pending = queue_[next];
  flight_ = pending.command;
  for (size_t i = next; i + 1 < count_; ++i)
    queue_[i] = queue_[i + 1];
  --count_;
  // Move remaining copies to the back for fairness; never catch up in a burst.
  // A later control PATCH replaces this slot, cancelling stale copies.
  if (--pending.remaining) {
    pending.due = now + kControlIntervalMs;
    queue_[count_++] = pending;
  }
  flight_.bytes[2] = sequence_++;
  const int i = slot(flight_.zoneId);
  if (i >= 0) refresh_[i] = {true, now + kRefreshIntervalMs};
  if (flight_.pairing) {
    if (i >= 0 && pairs_[i].active) pairs_[i].next = now + kPairingIntervalMs;
  }
  inFlight_ = true; command = flight_;
  return true;
}
void Controller::setRadioReady(bool ready) {
  radioReady_ = ready;
  if (!ready) {
    count_ = 0;
    for (auto& p : pairs_) p.active = false;
    for (auto& r : refresh_) r = Refresh{};
  }
}
void Controller::complete(bool localFault) {
  inFlight_ = false;
  if (localFault) setRadioReady(false);
}

void Controller::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  // Standby cancels work, not saved configuration. Wake starts fresh hourly
  // timers, without replaying old controls or pairing. The driver owns aborting
  // an in-flight transmission and must call complete() when it is stopped.
  count_ = 0;
  for (auto& p : pairs_) p = Pairing{};
  for (auto& r : refresh_) r = Refresh{};
}

ButtonEvent SetupButton::sample(bool low, uint32_t now) {
  if (low != raw_) { raw_ = low; changed_ = now; }
  if (raw_ != stable_ && now - changed_ >= 30) {
    stable_ = raw_;
    if (stable_) { pressed_ = changed_; fired_ = false; }
    else {
      const bool alreadyFired = fired_;
      fired_ = false;
      if (!alreadyFired) {
        // Classify by physical release time, not the end of release debounce.
        // A delayed loop must never turn a long hold into an off/on toggle.
        return changed_ - pressed_ >= 5000 ? ButtonEvent::Reset : ButtonEvent::Toggle;
      }
    }
  }
  if (stable_ && raw_ && !fired_ && now - pressed_ >= 5000) {
    fired_ = true; return ButtonEvent::Reset;
  }
  return ButtonEvent::None;
}
bool NetworkPolicy::submit(uint32_t now) {
  if (state != NetworkState::Setup && state != NetworkState::Configured) return false;
  state = state == NetworkState::Setup ? NetworkState::Trial : NetworkState::Editing;
  ++generation; deadline = now + 30000; return true;
}
bool NetworkPolicy::saved(uint32_t attempt, uint32_t now) {
  if (attempt != generation || (state != NetworkState::Trial && state != NetworkState::Editing)) return false;
  state = state == NetworkState::Trial ? NetworkState::Grace : NetworkState::Configured;
  deadline = now + 5000; return true;
}
bool NetworkPolicy::failed(uint32_t attempt) {
  if (attempt != generation || (state != NetworkState::Trial && state != NetworkState::Editing)) return false;
  state = state == NetworkState::Trial ? NetworkState::Setup : NetworkState::Configured;
  ++generation; return true;
}
void NetworkPolicy::reset() { ++generation; state = NetworkState::Resetting; }
void NetworkPolicy::resetComplete(bool verified) {
  state = verified ? NetworkState::Setup : NetworkState::Recovery;
}
}  // namespace connex
