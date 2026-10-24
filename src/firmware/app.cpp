#include "app.h"
#include "radio.h"
#include "network.h"
#include "http_server.h"
#include "security.h"
#include "status.h"
#include <WiFi.h>

namespace firmware {
NvsStore storage;
Entropy entropy;
connex::Controller controller{storage, entropy};
SemaphoreHandle_t controlMutex = nullptr;
std::atomic<bool> resetRequested{false}, networkHealthy{false};
char setupSsid[32] = {};
namespace {
constexpr uint8_t kSetupButtonPin = D2;
// External and onboard RGB LEDs are active-low; each row mirrors one color.
constexpr uint8_t kStatusPins[3][2] = {{D3, LED_RED}, {D4, LED_GREEN}, {D5, LED_BLUE}};
RadioDriver radio;
connex::SetupButton button;
bool completed = false, localFault = false;
bool runRequested = true, standbyPending = false, controllerHealthy = false, statusPwm = false;
uint32_t statusAt = 0;
void beginStatusLed() {
  for (const auto& pins : kStatusPins) {
    for (const uint8_t pin : pins) {
      pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); // Common anode: initially off.
    }
  }
  bool attached[3][2] = {};
  statusPwm = true;
  for (size_t color = 0; color < 3; ++color) {
    for (size_t led = 0; led < 2; ++led) {
      const uint8_t pin = kStatusPins[color][led];
      attached[color][led] = ledcAttach(pin, 1000, 8);
      if (attached[color][led]) ledcWrite(pin, 255);
      else statusPwm = false;
    }
  }
  if (!statusPwm) {
    // Preserve matching digital fault indications if any PWM allocation fails.
    for (size_t color = 0; color < 3; ++color) {
      for (size_t led = 0; led < 2; ++led) {
        const uint8_t pin = kStatusPins[color][led];
        if (attached[color][led]) ledcDetach(pin);
        pinMode(pin, OUTPUT); digitalWrite(pin, HIGH);
      }
    }
  }
}
void statusTick(uint32_t now) {
  if (now - statusAt < 20) return;
  statusAt = now;
  const bool connected = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress();
  const auto color = connex::statusColor(runRequested, network.portal(), connected,
                                         controllerHealthy && networkHealthy && statusPwm, now);
  const uint8_t brightness[] = {color.red, color.green, color.blue};
  for (size_t component = 0; component < 3; ++component) {
    for (const uint8_t pin : kStatusPins[component]) {
      if (statusPwm) ledcWrite(pin, connex::activeLowDuty(brightness[component]));
      else digitalWrite(pin, brightness[component] ? LOW : HIGH);
    }
  }
}
void networkTask(void*) {
  for (;;) { pollHttpDeadlines(); pollSecurity(); network.tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
}
}
void begin() {
  pinMode(kSetupButtonPin, INPUT_PULLUP);
  beginStatusLed();
  controlMutex = xSemaphoreCreateMutex();
  if (!controlMutex) return;
  // Radio/Wi-Fi entropy is enabled before allocating persistent random identities.
  WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA);
  connex::Snapshot saved; WifiCredentials wifi;
  const auto appResult = storage.load(saved), wifiResult = storage.loadWifi(wifi), marker = storage.marker();
  bool valid = false;
  if (appResult == LoadResult::Missing && wifiResult == LoadResult::Missing && marker == LoadResult::Missing) {
    valid = controller.initialize() == connex::Result::Ok && storage.saveWifi(wifi) == connex::SaveResult::Saved && storage.markProvisioned();
  } else if (appResult == LoadResult::Valid && wifiResult == LoadResult::Valid && marker == LoadResult::Valid) {
    valid = controller.load(saved);
  }
  formatSetupSsid(controller.snapshot().deviceId, setupSsid, sizeof setupSsid);
  controller.setRadioReady(radio.begin());
  networkHealthy = valid && statusPwm;
  network.begin(wifi, valid);
  if (!beginSecurity()) networkHealthy = false;
  if (!startHttp()) networkHealthy = false;
  if (xTaskCreate(networkTask, "connex-network", 12288, nullptr, 1, nullptr) != pdPASS) networkHealthy = false;
}
void tick() {
  const uint32_t now = millis();
  if (!controlMutex) { statusTick(now); delay(10); return; }
  const auto event = button.sample(digitalRead(kSetupButtonPin) == LOW, now);
  if (event == connex::ButtonEvent::Reset) {
    runRequested = true; // Holding Setup also wakes a controller in standby.
    resetRequested = true;
  } else if (event == connex::ButtonEvent::Toggle && !resetRequested) {
    runRequested = !runRequested;
    if (!runRequested) {
      standbyPending = true;
      // Stop hardware retries now, even if NVS work temporarily owns the mutex.
      // A packet already on air cannot be recalled. Preserve any prior fault.
      radio.cancel(); completed = true; localFault |= radio.fault();
    }
  }
  if (radio.poll()) { completed = true; localFault = radio.fault(); }
  // Never wait behind a storage mutation before sampling the setup button or completing RF.
  if (xSemaphoreTake(controlMutex, 0) == pdTRUE) {
    if (completed) { controller.complete(localFault); completed = localFault = false; }
    // An off/on pair while storage owns the mutex must still cancel old work.
    if (standbyPending) { controller.setEnabled(false); standbyPending = false; }
    controller.setEnabled(runRequested);
    connex::Command command;
    const bool send = controller.take(now, command);
    controllerHealthy = controller.healthy();
    xSemaphoreGive(controlMutex);
    if (send) radio.start(command);
  }
  statusTick(now);
  delay(1);
}
}
