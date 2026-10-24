#pragma once
#include <Arduino.h>
#include <RF24.h>
#include "domain.h"

namespace firmware {
class RadioDriver {
 public:
  bool begin();
  void start(const connex::Command& command);
  bool poll();
  void cancel();
  bool fault() const { return fault_; }
 private:
  void stop();
  RF24 radio_{D7, D8};
  bool active_ = false, finished_ = false, fault_ = false;
  uint32_t started_ = 0;
};
}
