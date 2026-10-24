#include "radio.h"
#include <SPI.h>

namespace firmware {
bool RadioDriver::begin() {
  if (!radio_.begin()) return false;
  radio_.setAutoAck(true);
  radio_.disableDynamicPayloads(); radio_.disableAckPayload();
  radio_.setRetries(5, 15); radio_.setChannel(40);
  const bool rate = radio_.setDataRate(RF24_250KBPS);
  radio_.setAddressWidth(5); radio_.setPayloadSize(10);
  radio_.setCRCLength(RF24_CRC_16); radio_.setPALevel(RF24_PA_LOW);
  radio_.openWritingPipe(0x5049382716ULL); radio_.stopListening();
  stop(); finished_ = false;
  return rate;
}
void RadioDriver::stop() {
  radio_.powerDown(); radio_.flush_tx(); radio_.flush_rx(); radio_.clearStatusFlags();
  active_ = false; finished_ = true;
}
void RadioDriver::cancel() {
  if (active_) stop(); // Idle is already powered down; initialization may have failed.
  finished_ = false; // The caller explicitly completes the canceled dispatch.
}
void RadioDriver::start(const connex::Command& command) {
  fault_ = false; finished_ = false;
  if (!radio_.isChipConnected()) { fault_ = true; stop(); return; }
  radio_.flush_tx(); radio_.flush_rx(); radio_.clearStatusFlags(); radio_.powerUp();
  started_ = micros();
  active_ = radio_.startWrite(command.bytes, sizeof command.bytes, false);
  if (!active_) { fault_ = true; stop(); }
}
bool RadioDriver::poll() {
  if (active_) {
    const uint8_t flags = radio_.update();
    if (flags == 0xff || micros() - started_ >= 200000) { fault_ = true; stop(); }
    else if (flags & (RF24_TX_DS | RF24_TX_DF)) stop(); // A missing ACK is not a hardware fault.
  }
  if (!finished_) return false;
  finished_ = false; return true;
}
}
