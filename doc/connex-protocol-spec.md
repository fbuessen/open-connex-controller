# Connex application protocol

This reference describes the 10-byte zone-setting message used by Open Connex Controller. The [radio framing reference](connex-radio-framing.md) defines its transport. Byte offsets are zero-based and byte values are hexadecimal.

## Payload

```text
Offset:   00 01 02 03 04 05 06 07 08 09
Payload:  I0 I1 SS 01 ZZ TH TL EE FL 00
```

| Offset | Field | Meaning |
| --- | --- | --- |
| 0–1 | `I0 I1` | Opaque two-byte zone identity prefix, retained across commands. |
| 2 | `SS` | Application sequence counter, wrapping modulo 256. |
| 3 | `01` | Fixed value for zone-setting messages. |
| 4 | `ZZ` | Zero-based radio zone index. The firmware uses `00`–`07`. |
| 5–6 | `TH TL` | Unsigned big-endian temperature in tenths of the selected unit. |
| 7 | `EE` | Eco offset plus 9, in the selected unit. |
| 8 | `FL` | Unit and mode flags. |
| 9 | `00` | Fixed value for zone-setting messages. |

Supply only these ten bytes to the radio. The radio adds its address, packet-control field, and CRC.

## Temperature and Eco offset

```text
base_temperature = ((TH << 8) | TL) / 10
eco_offset = EE - 9
eco_target = base_temperature + eco_offset
```

Examples: `00 C8` encodes 20.0; `01 18` encodes 28.0; `02 BC` encodes 70.0. The unit flag determines whether those numbers are Celsius or Fahrenheit. Changing the unit flag does not convert the numeric values.

For the offset, `02` means −7, `09` means zero, and `0D` means +4. The offset is stored in both modes and takes effect in Eco mode.

The firmware accepts base targets of 10–30°C or 50–86°F in half-degree steps, and integer offsets from −9 through +9. These are firmware limits, separate from the payload's numeric capacity.

## Unit and mode flags

| `FL` | Unit | Mode |
| --- | --- | --- |
| `00` | Fahrenheit | Normal |
| `01` | Celsius | Normal |
| `02` | Fahrenheit | Eco |
| `03` | Celsius | Eco |

Bit 0 selects Celsius; bit 1 selects Eco. Send all other flag bits as zero. Preserve bytes 3 and 9 as `01` and `00`.

## Zone identity and sequence

The identity prefix and radio zone index associate commands with a zone. Prefixes are opaque values, not a shared controller identifier or a table of predefined constants. Open Connex Controller generates a distinct random prefix for each zone and persists it with the zone index.

The application sequence advances for each firmware transmission, including repeated settings and pairing messages. It is shared across zones and wraps at 256. The radio's two-bit packet identifier is separate from this application counter.

## Pairing

A heater in learning mode associates with a zone from an ordinary zone-setting message. Pairing uses the same payload as temperature and mode changes.

The firmware sends the zone's current settings immediately and then every five seconds for the pairing window: 30 seconds by default, configurable up to 60 seconds through the API. The heater must be placed in learning mode using its own controls. Deleting a zone from the controller does not unpair the heater.

## Examples

These examples use the fictional prefix `12 34` and radio zone index `02`:

| Payload | Meaning |
| --- | --- |
| `12 34 01 01 02 00 C8 09 01 00` | 20°C, Normal, zero Eco offset. |
| `12 34 02 01 02 00 C8 06 03 00` | 20°C base target, Eco offset −3°C, Eco target 17°C. |
| `12 34 03 01 02 02 BC 07 02 00` | 70°F base target, Eco offset −2°F, Eco target 68°F. |

The firmware implements zone settings and pairing through this message. It has no heater-off command or heater telemetry. Radio acknowledgments do not confirm that a heater applied a setting; see the [REST API](rest-api-spec.md) for accepted-write and transmission timing semantics.
