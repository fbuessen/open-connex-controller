#include "access.h"
#include <string.h>

namespace connex {
void wipe(void* value, size_t count) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (count--) *bytes++ = 0;
}
bool constantEqual(const void* a, const void* b, size_t count) {
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < count; ++i) difference |= x[i] ^ y[i];
  return difference == 0;
}
namespace {
bool hex(const char* value) {
  if (strnlen(value, 65) != 64) return false;
  for (size_t i = 0; i < 64; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  return true;
}
bool passwordSize(const char* value) { const size_t n = strnlen(value, 129); return n >= 10 && n <= 128; }
}
bool validAccess(const AccessRecord& r) {
  // Accept the previously deployed work factor until successful authentication
  // can replace the hash. Never accept arbitrary costs from damaged storage.
  return r.schema == 1 && (r.iterations == kPasswordIterations || r.iterations == 100000) && r.configured <= 1 &&
         (r.token[0] ? hex(r.token) : true);
}
bool sessionCookie(const char* header, char out[65]) {
  out[0] = 0; bool found = false;
  while (*header) {
    while (*header == ' ' || *header == ';') ++header;
    const char* end = strchr(header, ';'); if (!end) end = header + strlen(header);
    const char* equals = static_cast<const char*>(memchr(header, '=', end - header));
    if (equals && equals - header == 14 && !memcmp(header, "connex_session", 14)) {
      if (found || end - equals - 1 != 64) { out[0] = 0; return false; }
      memcpy(out, equals + 1, 64); out[64] = 0;
      if (!hex(out)) { out[0] = 0; return false; }
      found = true;
    }
    header = end;
  }
  return found;
}
bool Access::load(const AccessRecord& r) {
  invalidate(); ready_ = validAccess(r);
  if (ready_) record_ = r;
  return ready_;
}
void Access::invalidate() { wipe(sessions_, sizeof sessions_); }
int Access::save(const AccessRecord& next) {
  const auto result = store_.saveAccess(next);
  if (result == SaveResult::Uncertain) { ready_ = false; invalidate(); }
  if (result != SaveResult::Saved) return 503;
  record_ = next; return 200;
}
bool Access::admit(uint32_t now) {
  if (now - window_ >= 60000) { attempts_ = 0; window_ = now; }
  if (attempts_ >= 5) return false;
  ++attempts_; return true;
}
bool Access::verify(const char* password) {
  uint8_t derived[32] = {};
  const bool ok = passwordSize(password) && crypto_.derive(password, record_.salt, record_.iterations, derived) &&
                  constantEqual(derived, record_.passwordHash, sizeof derived);
  wipe(derived, sizeof derived); return ok;
}
int Access::savePassword(const char* password) {
  AccessRecord next = record_;
  crypto_.random(next.salt, sizeof next.salt);
  next.configured = 1;
  next.iterations = kPasswordIterations;
  const int result = crypto_.derive(password, next.salt, next.iterations, next.passwordHash) ? save(next) : 503;
  wipe(&next, sizeof next);
  return result;
}
void Access::secret(char out[65]) {
  uint8_t bytes[32]; crypto_.random(bytes, sizeof bytes);
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof bytes; ++i) { out[i*2] = digits[bytes[i] >> 4]; out[i*2+1] = digits[bytes[i] & 15]; }
  out[64] = 0; wipe(bytes, sizeof bytes);
}
int Access::issue(uint32_t now, char out[65]) {
  Session* free = nullptr;
  for (auto& s : sessions_) if (!s.id[0] || now - s.created >= kSessionMs) { free = &s; break; }
  if (!free) {
    free = &sessions_[0];
    for (auto& s : sessions_) if (now - s.created > now - free->created) free = &s;
  }
  secret(free->id); free->created = now; memcpy(out, free->id, 65); return 200;
}
int Access::enroll(const char* password, bool setupPortal, uint32_t now, char out[65]) {
  out[0] = 0;
  if (!ready_) return 503;
  if (configured()) return 409;
  if (!enrollmentAllowed(setupPortal)) return 403;
  if (!passwordSize(password)) return 422;
  if (!admit(now)) return 429;
  const int result = savePassword(password);
  return result == 200 ? issue(now, out) : result;
}
int Access::login(const char* password, uint32_t now, char out[65]) {
  out[0] = 0;
  if (!ready_) return 503;
  if (!configured()) return 409;
  if (!admit(now)) return 429;
  if (!verify(password)) return 401;
  if (record_.iterations != kPasswordIterations) {
    const int result = savePassword(password);
    if (result != 200) return result;
  }
  return issue(now, out);
}
int Access::password(const char* current, const char* replacement, uint32_t now) {
  if (!configured()) return 503;
  if (!passwordSize(replacement)) return 422;
  if (!admit(now)) return 429;
  if (!verify(current)) return 401;
  const int result = savePassword(replacement);
  if (result == 200) invalidate();
  return result;
}
bool Access::bearer(const char* value) const {
  return configured() && record_.token[0] && strlen(value) == 71 && !memcmp(value, "Bearer ", 7) &&
         constantEqual(value + 7, record_.token, 64);
}
bool Access::session(const char* id, uint32_t now) {
  if (!configured() || !hex(id)) return false;
  expire(now);
  for (auto& s : sessions_) {
    if (s.id[0] && constantEqual(id, s.id, 64)) return true;
  }
  return false;
}
void Access::expire(uint32_t now) {
  for (auto& s : sessions_) if (s.id[0] && now - s.created >= kSessionMs) wipe(&s, sizeof s);
}
void Access::logout(const char* id) {
  if (!hex(id)) return;
  for (auto& s : sessions_) if (s.id[0] && constantEqual(id, s.id, 64)) wipe(&s, sizeof s);
}
int Access::rotateToken() {
  if (!configured()) return 503;
  AccessRecord next = record_; secret(next.token);
  const int result = save(next); wipe(&next, sizeof next); return result;
}
int Access::revokeToken() {
  if (!configured()) return 503;
  AccessRecord next = record_; wipe(next.token, sizeof next.token);
  const int result = save(next); wipe(&next, sizeof next); return result;
}
int Access::resetPassword() {
  if (!ready_) return 503; // Do not erase an unreadable token while claiming to preserve it.
  AccessRecord next{};
  if (ready_) memcpy(next.token, record_.token, sizeof next.token);
  const int result = save(next); wipe(&next, sizeof next);
  if (result == 200) { ready_ = true; invalidate(); attempts_ = 0; }
  return result;
}
}
