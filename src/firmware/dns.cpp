#include "dns.h"
#include <string.h>
namespace firmware {
size_t dnsReply(const uint8_t* q, size_t n, const uint8_t ip[4], uint8_t out[512]) {
  if (n < 17 || n > 512 || (q[2] & 0xfe) || q[3] || q[4] || q[5] != 1 ||
      q[6] || q[7] || q[8] || q[9]) return 0;
  size_t pos = 12;
  while (pos < n && q[pos]) {
    const size_t label = q[pos++];
    if (label > 63 || label > n - pos || pos + label - 12 > 254) return 0;
    pos += label;
  }
  if (pos >= n || pos + 5 > n) return 0;
  ++pos;
  const unsigned type = (unsigned(q[pos]) << 8) | q[pos + 1];
  if (q[pos + 2] || q[pos + 3] != 1) return 0;
  pos += 4;
  if (pos + 16 > 512) return 0;
  memcpy(out, q, pos);
  out[2] = 0x84 | (q[2] & 1); out[3] = 0;
  out[6] = out[7] = out[8] = out[9] = out[10] = out[11] = 0;
  if (type != 1 && type != 255) return pos;
  out[7] = 1;
  const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
  memcpy(out + pos, record, sizeof record); pos += sizeof record;
  memcpy(out + pos, ip, 4);
  return pos + 4;
}
}
