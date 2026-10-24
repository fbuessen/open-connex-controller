#pragma once
#include <stddef.h>
#include <stdint.h>

namespace firmware {
// Minimal captive DNS: one uncompressed IN question, bounded labels/packet.
// Unsupported/malformed packets are dropped; AAAA gets an empty answer.
size_t dnsReply(const uint8_t* request, size_t length, const uint8_t address[4], uint8_t out[512]);
}
