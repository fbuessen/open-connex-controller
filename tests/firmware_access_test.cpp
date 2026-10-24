#include "access.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using namespace connex;
static_assert(kSessionSeconds == 2592000 && kSessionMs == 2592000000u, "Fixed 30-day session lifetime");
struct Memory : AccessStorage {
  AccessRecord stored; SaveResult result = SaveResult::Saved; unsigned writes = 0;
  SaveResult saveAccess(const AccessRecord& value) override { ++writes; if (result == SaveResult::Saved) stored = value; return result; }
};
// Policy tests inject a deterministic derivation, not a replacement production cipher.
struct Crypto : AccessCrypto {
  unsigned serial = 1, calls = 0;
  uint32_t failIterations = 0;
  std::vector<uint32_t> costs;
  void random(void* out, size_t size) override { auto* bytes = static_cast<uint8_t*>(out); for (size_t i = 0; i < size; ++i) bytes[i] = serial++; }
  bool derive(const char* password, const uint8_t salt[16], uint32_t iterations, uint8_t out[32]) override {
    ++calls; costs.push_back(iterations);
    assert(iterations == 10000 || iterations == 100000);
    if (iterations == failIterations) return false;
    const size_t n = strlen(password);
    for (size_t i = 0; i < 32; ++i) out[i] = salt[i%16] ^ password[i%n] ^ uint8_t(iterations >> (8*(i%4)));
    return true;
  }
};
void workFactorTests() {
  static_assert(kPasswordIterations == 10000);
  AccessRecord old; old.iterations = 100000; old.configured = 1;
  memset(old.token, 'a', 64);
  Crypto reference;
  assert(reference.derive("previous-password",old.salt,old.iterations,old.passwordHash));
  assert(validAccess(old));
  for (uint32_t iterations : {0u,1u,9999u,10001u,99999u,100001u,0xffffffffu}) {
    AccessRecord bad = old; bad.iterations = iterations;
    Memory m; Crypto c; Access a(m,c);
    assert(!validAccess(bad) && !a.load(bad) && !a.ready());
  }
  Memory memory; memory.stored = old;
  Crypto crypto; Access auth(memory,crypto); char session[65];
  assert(auth.load(old));
  assert(auth.login("wrong-password",0,session) == 401 && !session[0]);
  assert(crypto.costs == std::vector<uint32_t>{100000} && !memory.writes);
  crypto.costs.clear();
  assert(auth.login("previous-password",60000,session) == 200);
  assert((crypto.costs == std::vector<uint32_t>{100000,10000}));
  assert(memory.writes == 1 && memory.stored.iterations == 10000);
  assert(strcmp(memory.stored.token,old.token) == 0 && auth.session(session,60000));
  assert(memcmp(memory.stored.salt,old.salt,sizeof old.salt));
  crypto.costs.clear();
  assert(auth.load(memory.stored) && auth.login("previous-password",120000,session) == 200);
  assert(crypto.costs == std::vector<uint32_t>{10000} && memory.writes == 1);
  // Changing an older password verifies at its stored cost, then saves at 10k.
  assert(auth.load(old)); crypto.costs.clear();
  assert(auth.password("previous-password","replacement-password",180000) == 200);
  assert((crypto.costs == std::vector<uint32_t>{100000,10000}));
  assert(memory.stored.iterations == 10000 && !strcmp(memory.stored.token,old.token));
  crypto.costs.clear();
  assert(auth.login("previous-password",240000,session) == 401);
  assert(auth.login("replacement-password",240000,session) == 200);
  assert((crypto.costs == std::vector<uint32_t>{10000,10000}));
  // A stale work factor on an unconfigured record must not affect enrollment.
  AccessRecord empty = old; empty.configured = 0;
  assert(auth.load(empty)); crypto.costs.clear();
  assert(auth.enroll("setup-password",true,300000,session) == 200);
  assert(crypto.costs == std::vector<uint32_t>{10000} && memory.stored.iterations == 10000);
  // Failed hashing/persistence must never issue a session or lose the old hash.
  for (const auto result : {SaveResult::Rejected,SaveResult::Uncertain}) {
    Memory failed; failed.stored = old; failed.result = result;
    Crypto c; Access a(failed,c); assert(a.load(old));
    assert(a.login("previous-password",0,session) == 503 && !session[0]);
    assert(failed.stored.iterations == 100000);
    assert(a.ready() == (result == SaveResult::Rejected));
    assert(!a.session(session,0));
  }
  Memory failed; failed.stored = old;
  Crypto c; c.failIterations = 10000; Access a(failed,c); assert(a.load(old));
  assert(a.login("previous-password",0,session) == 503 && !session[0] && !failed.writes && a.ready());
  assert(failed.stored.iterations == 100000);
  c.failIterations = 0;
  assert(a.login("previous-password",60000,session) == 200 && failed.stored.iterations == 10000);
  assert(a.load(old) && a.resetPassword() == 200);
  assert(failed.stored.iterations == 10000 && !failed.stored.configured);
}
int main() {
  workFactorTests();
  Memory memory; Crypto crypto; Access auth(memory,crypto); char session[65], other[65], header[192];
  assert(!auth.ready() && !auth.session("",0) && !auth.bearer(""));
  assert(!auth.enrollmentAllowed(false) && !auth.enrollmentAllowed(true));
  assert(auth.enroll("example-password",true,0,session) == 503);
  assert(auth.load(AccessRecord{}));
  assert(!auth.enrollmentAllowed(false) && auth.enrollmentAllowed(true));
  assert(auth.enroll("example-password",false,0,session) == 403 && !crypto.calls && !memory.writes);
  assert(auth.enroll("short",true,0,session) == 422);
  assert(auth.enroll("example-password",true,0,session) == 200);
  assert(memory.stored.iterations == 10000 && crypto.costs == std::vector<uint32_t>{10000});
  assert(auth.configured() && auth.session(session,0) && strlen(session) == 64);
  assert(!auth.enrollmentAllowed(false) && !auth.enrollmentAllowed(true));
  assert(auth.enroll("example-password",false,0,other) == 409);
  assert(auth.enroll("example-password",true,0,other) == 409);
  assert(!auth.bearer(session));
  assert(auth.login("wrong-password",0,other) == 401 && !other[0]);
  assert(auth.login("example-password",0,other) == 200 && strcmp(session,other));
  assert(auth.session(session,kSessionMs-1) && !auth.session(session,kSessionMs));
  assert(auth.rotateToken() == 200);
  snprintf(header,sizeof header,"Bearer %s",auth.token()); assert(auth.bearer(header));
  assert(!auth.session(auth.token(),0));
  char token[65]; strcpy(token,auth.token());
  assert(auth.rotateToken() == 200 && !auth.bearer(header));
  assert(auth.revokeToken() == 200 && !auth.token()[0]);
  assert(auth.rotateToken() == 200); strcpy(token,auth.token());
  crypto.costs.clear();
  assert(auth.password("wrong-password","replacement-password",60000) == 401);
  assert(auth.password("example-password","replacement-password",60000) == 200);
  assert((crypto.costs == std::vector<uint32_t>{10000,10000,10000}));
  assert(!auth.session(other,60000) && !strcmp(auth.token(),token));
  assert(auth.login("example-password",60000,other) == 401);
  assert(auth.login("replacement-password",60000,other) == 200);
  assert(auth.resetPassword() == 200 && !auth.configured() && !auth.session(other,60000));
  assert(!auth.enrollmentAllowed(false) && auth.enrollmentAllowed(true));
  const unsigned resetCalls = crypto.calls, resetWrites = memory.writes;
  assert(auth.enroll("third-password",false,60000,session) == 403);
  assert(crypto.calls == resetCalls && memory.writes == resetWrites);
  assert(!strcmp(auth.token(),token)); snprintf(header,sizeof header,"Bearer %s",token); assert(!auth.bearer(header));
  assert(auth.enroll("third-password",true,60000,session) == 200 && auth.bearer(header));
  // Simulated restart preserves credentials/token but never browser sessions.
  assert(auth.load(memory.stored) && !auth.session(session,60000) && auth.bearer(header));
  assert(auth.login("third-password",120000,session) == 200);
  snprintf(header,sizeof header,"other=value; connex_session=%s",session);
  assert(sessionCookie(header,other) && !strcmp(session,other));
  snprintf(header,sizeof header,"connex_session=%s; connex_session=%s",session,session);
  assert(!sessionCookie(header,other) && !other[0]);
  assert(!sessionCookie("connex_session=short",other));
  assert(!sessionCookie("another=value",other));
  auth.logout(session); assert(!auth.session(session,120000));
  for (unsigned i = 0; i < 4; ++i) assert(auth.login("wrong-password",120000,session) == 401);
  const unsigned calls = crypto.calls;
  assert(auth.login("third-password",120000,session) == 429 && crypto.calls == calls);
  assert(auth.login("third-password",180000,session) == 200);
  memory.result = SaveResult::Rejected; assert(auth.rotateToken() == 503 && auth.ready());
  memory.result = SaveResult::Uncertain; assert(auth.rotateToken() == 503 && !auth.ready() && !auth.session(session,180000));
  AccessRecord corrupt; corrupt.configured = 2; assert(!auth.load(corrupt));
  corrupt = AccessRecord{}; corrupt.token[0] = 'x'; assert(!auth.load(corrupt));
  // Both absolute session age and throttling use wrap-safe elapsed time.
  memory.result = SaveResult::Saved; assert(auth.load(memory.stored));
  assert(auth.login("third-password",0xfffffff0u,session) == 200 && auth.session(session,20));
  assert(auth.session(session,0xfffffff0u+kSessionMs-1));
  assert(!auth.session(session,0xfffffff0u+kSessionMs));
  // Four concurrent sessions; the fifth successful login evicts only the oldest.
  Memory slotsMemory; Crypto slotsCrypto; Access slots(slotsMemory,slotsCrypto); char ids[5][65];
  assert(slots.load(AccessRecord{}));
  assert(slots.enroll("slots-password",true,0,ids[0]) == 200);
  for (unsigned i=1;i<5;++i) assert(slots.login("slots-password",i*60000,ids[i]) == 200);
  assert(!slots.session(ids[0],240000));
  for (unsigned i=1;i<5;++i) assert(slots.session(ids[i],240000));
  slots.expire(kSessionMs+240000);
  // Once erased by the periodic sweep, an old cookie cannot revive on wrap.
  assert(!slots.session(ids[4],240000));
  puts("PASS: 10k password costs/old-hash replacement/failures, enrollment, session expiry/rotation/logout, bearer isolation, token lifecycle, password/reset persistence, throttling, cookie parsing, fail-closed storage, rollover");
}
