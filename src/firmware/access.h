#pragma once
#include "domain.h"

namespace connex {
constexpr uint32_t kPasswordIterations = 10000;
constexpr uint32_t kSessionSeconds = 30u * 24 * 60 * 60;
constexpr uint32_t kSessionMs = kSessionSeconds * 1000;
struct AccessRecord {
  uint32_t schema = 1;
  uint32_t iterations = kPasswordIterations;
  uint8_t configured = 0;
  uint8_t salt[16] = {};
  uint8_t passwordHash[32] = {};
  char token[65] = {};
};
struct AccessStorage {
  virtual ~AccessStorage() = default;
  virtual SaveResult saveAccess(const AccessRecord&) = 0;
};
struct AccessCrypto {
  virtual ~AccessCrypto() = default;
  virtual void random(void*, size_t) = 0;
  virtual bool derive(const char*, const uint8_t salt[16], uint32_t iterations, uint8_t out[32]) = 0;
};
bool validAccess(const AccessRecord&);
bool constantEqual(const void*, const void*, size_t);
void wipe(void*, size_t);
// Strict extraction: duplicate, malformed, or oversized session cookies fail closed.
bool sessionCookie(const char* header, char out[65]);

// Serialized by the platform. No global default password, persistent sessions,
// bearer-to-session exchange, or browser-readable session secret.
class Access {
 public:
  Access(AccessStorage& store, AccessCrypto& crypto) : store_(store), crypto_(crypto) {}
  bool load(const AccessRecord&);
  bool ready() const { return ready_; }
  bool configured() const { return ready_ && record_.configured; }
  bool enrollmentAllowed(bool setupPortal) const { return setupPortal && ready_ && !configured(); }
  const char* token() const { return ready_ ? record_.token : ""; }
  bool bearer(const char*) const;
  bool session(const char*, uint32_t now);
  void expire(uint32_t now);
  int enroll(const char* password, bool setupPortal, uint32_t now, char session[65]);
  int login(const char* password, uint32_t now, char session[65]);
  int password(const char* current, const char* replacement, uint32_t now);
  int rotateToken();
  int revokeToken();
  void logout(const char*);
  int resetPassword();
 private:
  struct Session { char id[65] = {}; uint32_t created = 0; } sessions_[4];
  AccessStorage& store_;
  AccessCrypto& crypto_;
  AccessRecord record_;
  bool ready_ = false;
  uint32_t window_ = 0;
  unsigned attempts_ = 0;
  int save(const AccessRecord&);
  bool admit(uint32_t now);
  bool verify(const char*);
  int savePassword(const char*);
  void secret(char out[65]);
  int issue(uint32_t now, char out[65]);
  void invalidate();
};
}
