#pragma once
#include "storage.h"
#include <Arduino.h>
#include <WiFiUdp.h>
#include <atomic>

namespace firmware {
struct NetworkStatus {
  connex::NetworkState state = connex::NetworkState::Recovery;
  char ssid[33] = {};
  char error[80] = {};
  bool portal = false, connected = false, scanning = false;
  uint32_t remaining = 0;
};
struct ScanEntry { char ssid[33] = {}; int rssi = 0; bool secured = false; };
class NetworkManager {
 public:
  void begin(const WifiCredentials& saved, bool valid);
  void tick();
  bool status(NetworkStatus& out);
  int submit(const WifiCredentials& candidate, uint32_t& generation);
  void releaseSubmission(uint32_t generation);
  int requestScan();
  bool scanResults(ScanEntry out[12], size_t& count, bool& scanning);
  bool portal() const { return portal_.load(); }
 private:
  bool lock();
  void unlock();
  void openPortal();
  bool closePortal();
  void connect(const WifiCredentials& credentials);
  void dnsTick();
  void scanTick();
  void reset();
  void failTrial(uint32_t generation, const char* error);
  SemaphoreHandle_t mutex_ = nullptr;
  connex::NetworkPolicy policy_;
  WifiCredentials saved_, candidate_;
  char error_[80] = {};
  bool valid_ = false, launch_ = false, scanning_ = false, scanRequested_ = false, mdns_ = false;
  uint32_t retryAt_ = 0, retryDelay_ = 1000, lastScan_ = 0, startedGeneration_ = 0;
  std::atomic<bool> portal_{false};
  ScanEntry scans_[12];
  size_t scanCount_ = 0;
  WiFiUDP dns_;
};
extern NetworkManager network;
}
