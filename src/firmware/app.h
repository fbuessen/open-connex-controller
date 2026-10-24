#pragma once
#include <Arduino.h>
#include <atomic>
#include "domain.h"
#include "storage.h"
#include "device_info.h"

namespace firmware {
extern NvsStore storage;
extern connex::Controller controller;
extern SemaphoreHandle_t controlMutex;
extern std::atomic<bool> resetRequested;
extern std::atomic<bool> networkHealthy;
extern char setupSsid[32];
void begin();
void tick();
}
