# Connex radio framing

Transport for the [10-byte application message](connex-protocol-spec.md), implemented by the firmware's [radio driver](../src/firmware/radio.cpp).

## Radio configuration

| Property | Value |
| --- | --- |
| Radio | nRF24L01+ with 250 kbit/s support |
| RF channel / frequency | 40 / 2440 MHz |
| Data rate | 250 kbit/s |
| Address width | 5 bytes |
| RF address, integer notation | `0x5049382716` |
| Address in on-air bitstream order | `50 49 38 27 16` |
| Address array for RF24 | `{0x16, 0x27, 0x38, 0x49, 0x50}` |
| Command payload | Static, 10 bytes |
| CRC | 16 bits |
| Automatic acknowledgment | Enabled, empty ACK payload |
| Dynamic payloads / ACK payloads | Disabled |

The integer address and reversed RF24 array describe the same address. Application bytes are sent in their original order. All zones use this RF address; the application identity prefix and zone index distinguish their commands.

## Frame structure

Commands use nRF24 Enhanced ShockBurst framing:

```text
preamble | address        | packet control | application payload | CRC
8 bits   | 40 bits        | 9 bits         | 80 bits             | 16 bits
         | 50 49 38 27 16 |                | 10 bytes            |
```

The nine-bit packet-control field (PCF) places the payload across byte boundaries in the complete bitstream.

| PCF bits | Meaning |
| --- | --- |
| 8–3 | Payload-length field; not used as the length in this static-payload profile. |
| 2–1 | Two-bit packet identifier (PID). |
| 0 | `NO_ACK`: 1 suppresses automatic acknowledgment. |

Connex PCFs `198`, `19A`, `19C`, and `19E` encode PIDs 0–3 with `NO_ACK=0`. Their upper field is 51, which is not the command length. Commands contain ten application bytes; ACKs contain none.

The radio manages PCF, PID, CRC, and hardware retries. The firmware passes only the application payload to RF24.

## CRC

CRC covers the 40-bit address, all nine PCF bits, and the payload, most significant bit first:

- Polynomial: `0x1021`.
- Initial value: `0xFFFF`.
- No reflection or final XOR.
- Preamble and received CRC are excluded.
- The resulting 16 bits are transmitted most significant bit first.

For each input bit `b`:

```text
feedback = (crc >> 15) XOR b
crc = ((crc << 1) XOR (0x1021 if feedback else 0)) AND 0xFFFF
```

A CRC over just the ten application bytes is not equivalent. Hardware generates and checks this CRC during normal operation.

## Acknowledgments and retries

An empty ACK is a radio acknowledgment. It carries no heater identity, application status, or pairing result. A received ACK does not confirm that the heater applied the settings, and a missing ACK does not establish that it missed the message.

The firmware configures a 1500 µs retry delay and up to 15 hardware retries using RF24's `setRetries(5, 15)`. It uses low transmit power and powers the radio down between sends. A local transaction timeout is 200 ms; missing ACK alone is not a local hardware fault.

Application repetition is independent of hardware retries: control updates use a ten-second quiet period followed by five sends one second apart; pairing repeats every five seconds; idle zones receive an hourly refresh. See the [REST API](rest-api-spec.md) for scheduling and cancellation.
