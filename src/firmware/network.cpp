#include "network.h"
#include "app.h"
#include "dns.h"
#include "security.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>

namespace firmware {
NetworkManager network;
using connex::NetworkState;
bool NetworkManager::lock() { return mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE; }
void NetworkManager::unlock() { xSemaphoreGive(mutex_); }

void NetworkManager::begin(const WifiCredentials& saved, bool valid) {
  mutex_ = xSemaphoreCreateMutex();
  valid_ = valid && mutex_;
  saved_ = saved;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname(hostname);
  if (!valid_) { networkHealthy = false; return; }
  if (saved_.ssid[0]) {
    policy_.state = NetworkState::Configured;
    WiFi.mode(WIFI_STA); connect(saved_);
  } else {
    policy_.state = NetworkState::Setup;
    openPortal();
  }
}
void NetworkManager::openPortal() {
  MDNS.end(); mdns_ = false;
  WiFi.mode(WIFI_AP_STA);
  const IPAddress ip(192, 168, 4, 1);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255,255,255,0)) || !WiFi.softAP(setupSsid, nullptr, 1, false, 4)) {
    networkHealthy = false; return;
  }
  portal_ = true;
  // DNS intentionally has no interface isolation in this version, by owner decision.
  // A bounded parser replaces the bundled helper's unchecked label scan.
  if (!dns_.begin(53)) networkHealthy = false;  // Manual address remains available.
}
bool NetworkManager::closePortal() {
  dns_.stop();
  const bool stopped = WiFi.softAPdisconnect(true);
  const bool stationOnly = WiFi.mode(WIFI_STA);
  if (!stopped || !stationOnly) { networkHealthy = false; return false; }
  portal_ = false; return true;
}
void NetworkManager::connect(const WifiCredentials& credentials) {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true);
  WiFi.begin(credentials.ssid, credentials.password);
  retryAt_ = millis() + 30000;
}
bool NetworkManager::status(NetworkStatus& out) {
  if (!lock()) return false;
  out.state = policy_.state; out.portal = portal_; out.connected = WiFi.status() == WL_CONNECTED;
  out.scanning = scanning_;
  memcpy(out.ssid, (policy_.state == NetworkState::Trial || policy_.state == NetworkState::Editing)
                      ? candidate_.ssid : saved_.ssid, sizeof out.ssid);
  memcpy(out.error, error_, sizeof out.error);
  const uint32_t now = millis();
  out.remaining = (policy_.state == NetworkState::Trial || policy_.state == NetworkState::Editing ||
                   policy_.state == NetworkState::Grace) && !policy_.expired(now)
                    ? (policy_.deadline - now + 999) / 1000 : 0;
  unlock(); return true;
}
int NetworkManager::submit(const WifiCredentials& credentials, uint32_t& generation) {
  if (!lock()) return 503;
  int result = 202;
  if (!valid_ || policy_.state == NetworkState::Recovery) result = 503;
  else if (resetRequested || scanning_ || !policy_.submit(millis())) result = 409;
  else {
    candidate_ = credentials; generation = policy_.generation;
    launch_ = false; scanRequested_ = false; error_[0] = 0;
  }
  unlock(); return result;
}
void NetworkManager::releaseSubmission(uint32_t generation) {
  // Called after the HTTP response attempt, even if the client disconnected.
  if (!mutex_) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!resetRequested && generation == policy_.generation &&
      (policy_.state == NetworkState::Trial || policy_.state == NetworkState::Editing)) launch_ = true;
  unlock();
}
int NetworkManager::requestScan() {
  if (!lock()) return 503;
  int result = 202;
  if (!valid_ || resetRequested || (policy_.state != NetworkState::Setup && policy_.state != NetworkState::Configured)) result = 409;
  else if (scanning_ || scanRequested_ || (lastScan_ && millis() - lastScan_ < 5000)) result = 429;
  else scanRequested_ = true;
  unlock(); return result;
}
bool NetworkManager::scanResults(ScanEntry out[12], size_t& count, bool& scanning) {
  if (!lock()) return false;
  memcpy(out, scans_, sizeof scans_); count = scanCount_; scanning = scanning_ || scanRequested_;
  unlock(); return true;
}
void NetworkManager::scanTick() {
  if (!lock()) return;
  if (scanRequested_) {
    scanRequested_ = false; scanning_ = true; lastScan_ = millis();
    unlock();
    WiFi.scanNetworks(true, true);
    return;
  }
  const bool running = scanning_;
  unlock();
  if (!running) return;
  const int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) return;
  if (!lock()) return;
  scanCount_ = 0;
  for (int i = 0; i < count && scanCount_ < 12; ++i) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length() || ssid.length() > 32) continue;
    auto& entry = scans_[scanCount_++];
    snprintf(entry.ssid, sizeof entry.ssid, "%s", ssid.c_str());
    entry.rssi = WiFi.RSSI(i); entry.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  scanning_ = false; unlock(); WiFi.scanDelete();
}
void NetworkManager::dnsTick() {
  if (!portal_) return;
  const int length = dns_.parsePacket();
  if (length <= 0) return;
  uint8_t request[512], response[512];
  if (length > 512 || dns_.read(request, length) != length) { dns_.clear(); return; }
  const uint8_t ip[] = {192,168,4,1};
  const size_t count = dnsReply(request, length, ip, response);
  if (count) {
    dns_.beginPacket(dns_.remoteIP(), dns_.remotePort());
    dns_.write(response, count); dns_.endPacket();
  }
  dns_.clear();
}
void NetworkManager::reset() {
  if (!valid_ || !mutex_) { resetRequested = false; return; } // Not a storage/factory reset.
  // A password operation may hold access longer than five seconds. Leave reset
  // pending and keep servicing HTTP deadlines until it completes; do not turn a
  // busy credential worker into a storage-failure/recovery state.
  if (accessMutex && xSemaphoreTake(accessMutex, 0) != pdTRUE) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  policy_.reset(); launch_ = false; scanning_ = scanRequested_ = false;
  unlock();
  WiFi.scanDelete(); WiFi.setAutoReconnect(false); WiFi.disconnect(false, true); MDNS.end(); mdns_ = false;
  const WifiCredentials empty{};
  bool accessReset = false;
  if (accessMutex) {
    accessReset = access.resetPassword() == 200;
    xSemaphoreGive(accessMutex);
  }
  const bool saved = accessReset && storage.saveWifi(empty) == connex::SaveResult::Saved;
  if (saved && !portal_) openPortal();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (saved) saved_ = empty;
  candidate_ = WifiCredentials{};
  policy_.resetComplete(saved);
  snprintf(error_, sizeof error_, "%s", saved ? "" : "Setup reset could not be verified; local recovery required");
  if (!saved) { valid_ = false; networkHealthy = false; }
  resetRequested = false; unlock();
}
void NetworkManager::failTrial(uint32_t generation, const char* message) {
  WiFi.disconnect(false, true);
  if (!lock()) return;
  if (policy_.failed(generation)) {
    candidate_ = WifiCredentials{};
    snprintf(error_, sizeof error_, "%s", message);
    retryAt_ = 0; launch_ = false;
  }
  unlock();
}
void NetworkManager::tick() {
  if (resetRequested) { reset(); return; }
  dnsTick();
  if (!valid_ || !lock()) return;
  const auto state = policy_.state;
  const auto generation = policy_.generation;
  const bool expired = policy_.expired(millis());
  const bool launch = launch_;
  const bool started = startedGeneration_ == generation;
  WifiCredentials candidate = candidate_;
  if (launch) launch_ = false;
  unlock();
  if (state == NetworkState::Trial || state == NetworkState::Editing) {
    if (launch) { connect(candidate); startedGeneration_ = generation; return; }
    // Do not mistake the old connection for success while the response is still being sent.
    if (started && WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress() && WiFi.SSID() == candidate.ssid) {
      if (portal_ && WiFi.localIP()[0] == 192 && WiFi.localIP()[1] == 168 && WiFi.localIP()[2] == 4) {
        failTrial(generation, "Home network conflicts with setup address 192.168.4.1"); return;
      }
      if (resetRequested) return;
      const auto result = storage.saveWifi(candidate);
      if (resetRequested) { reset(); return; }
      xSemaphoreTake(mutex_, portMAX_DELAY);
      if (result == connex::SaveResult::Saved && policy_.saved(generation, millis())) {
        saved_ = candidate; candidate_ = WifiCredentials{}; retryDelay_ = 1000;
      } else if (result == connex::SaveResult::Uncertain) {
        policy_.state = NetworkState::Recovery; valid_ = false; networkHealthy = false;
        snprintf(error_, sizeof error_, "Wi-Fi save could not be verified; USB recovery required");
      }
      unlock();
      if (result == connex::SaveResult::Rejected) failTrial(generation, "Unable to save Wi-Fi configuration");
    } else if (expired) failTrial(generation, "Connection failed; check network name and password");
    return;
  }
  if (state == NetworkState::Grace) {
    if (expired) {
      const bool closed = closePortal();
      if (!lock()) return;
      if (!resetRequested && policy_.generation == generation && policy_.state == NetworkState::Grace) {
        policy_.state = closed ? NetworkState::Configured : NetworkState::Recovery;
        if (!closed) { valid_ = false; snprintf(error_, sizeof error_, "Hotspot shutdown failed; restart the controller"); }
      }
      unlock();
    }
    return;
  }
  if (state == NetworkState::Configured) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mdns_) { mdns_ = MDNS.begin(hostname); if (mdns_) MDNS.addService("http", "tcp", 80); }
      retryDelay_ = 1000;
    } else if (connex::reached(millis(), retryAt_)) {
      MDNS.end(); mdns_ = false; connect(saved_);
      retryAt_ = millis() + 30000 + retryDelay_;
      retryDelay_ = min(uint32_t(60000), retryDelay_ * 2);
    }
  }
  if (state == NetworkState::Configured || state == NetworkState::Setup) scanTick();
}
}
