# Connex REST API

Endpoint contract for the [standalone firmware](firmware.md). Base path: `/api/v1`. Requests and responses use JSON over HTTP. Every endpoint requires `Authorization: Bearer <token>`. Generate or rotate the token in the signed-in web interface's Security settings. Web passwords and browser cookies are not API credentials. HTTP remains unencrypted; use a trusted network only. Temperatures and offsets use the zone's selected unit.

Zone IDs are stable, opaque strings, independent of editable names. Numeric RF zone IDs, prefixes, and raw packets are never exposed. Settings describe requested state, not confirmed heater state.

## Device

### `GET /device`

Returns `200` with only:

```json
{
  "id": "controller_0123456789abcdef",
  "firmware_version": "0.2.9",
  "health": "ok"
}
```

`id` survives restarts and firmware upgrades. `health` is `ok` when ready to accept control requests, otherwise `degraded`; it does not indicate heater availability.

Physical-button standby reports `degraded`. Reads and non-radio administration remain available, but control PATCHes and pairing starts return `503` without changing settings. Entering standby cancels pending transmissions and pairing, not saved configuration. Waking does not replay canceled work; hourly timers restart. Standby is not a heater off command and has no API endpoint.

## Zones

| Endpoint | Request | Response and behavior |
| --- | --- | --- |
| `GET /zones` | None | `200`: `{"zones": [...]}` containing full zone resources. |
| `POST /zones` | `{"name": "Living room"}` | `201`: new zone. Generates its string ID and a private random radio prefix; sends nothing. |
| `GET /zones/{id}` | None | `200`: one zone. |
| `PATCH /zones/{id}` | Any subset of `name`, `target_temperature`, `mode`, `eco_offset`, `unit` | `200` with the zone for name-only changes; `202` with no body once control changes are accepted and scheduled for dispatch. |
| `DELETE /zones/{id}` | None | `204`: deletes local configuration; does not unpair or switch off heaters. Returns `409` if RF work is pending for this zone. |

Zone response example:

```json
{
  "id": "zone_0123456789abcdef",
  "name": "Living room",
  "configuration": {
    "target_temperature": 21.0,
    "mode": "normal",
    "eco_offset": 0,
    "unit": "celsius"
  }
}
```

- Renaming does not change the zone's identity or association.
- The controller supports up to eight active zones. Creating a ninth returns `409` without changing existing zones.
- `configuration` contains the latest accepted settings, even if transmission fails. New zones start at 20°C, `normal` mode, zero eco offset, and `celsius` unit, ready for pairing. Creation sends nothing.
- `target_temperature` is the base setpoint; `mode` is `normal` or `eco`; `eco_offset` configures the energy-saving offset. Eco uses the base target plus this offset. Target limits are inclusive: 10–30 °C or 50–86 °F, in 0.5-degree steps. Eco offsets are integers from −9 through +9 in either selected unit.
- `unit` is `celsius` or `fahrenheit` and selects the heater's display unit and interpretation of temperature/offset values. Changing it does not automatically convert numeric values; supply updated values in the same PATCH when needed.

PATCH validates all fields before changing anything. A valid target is required for transmission and cannot be cleared to mean off. Supplying any control field schedules the complete resulting settings for dispatch, even if unchanged; reads and name-only updates send nothing. Unknown fields and unsupported values are rejected, not silently ignored or clamped. If validation or scheduling fails, return an error and leave the configuration unchanged.

Control updates from either the web interface or API are saved immediately, but radio dispatch waits for **10 seconds without another accepted control update to that zone**. Then send the complete configuration **five times, one second apart**. Each accepted control PATCH cancels remaining copies of older settings, replaces the pending configuration, and restarts the grace period, including unchanged values. Other zones have independent timers. Reads, renames, and rejected requests do not restart it. A packet already transmitting may finish.

Each configured zone also gets **one hourly refresh**, measured from its latest radio send. A refresh waits behind that zone's control/pairing work and uses the current configuration; it does not start a five-send burst. Startup and new zones wait a full hour before their first automatic refresh. Reboot discards unfinished bursts/pairing and restarts hourly timers. No catch-up bursts after delays. Radio ACK or missing ACK does not change these schedules; a local radio/storage fault suppresses dispatch.

## Pairing

| Endpoint | Request | Response and behavior |
| --- | --- | --- |
| `POST /zones/{id}/pairing` | `{"duration_seconds": 30}` | `202` with no body once repeated dispatch of the current configuration has been scheduled for a bounded period. |
| `DELETE /zones/{id}/pairing` | None | `204`: stops future pairing sends; harmless if already inactive. An in-flight send may finish. |

Place the heater in learning mode manually before starting. A configured target is required. Duration: 30 seconds by default, maximum 60; first send as soon as available, without the configuration grace period, then every 5 seconds. Pairing stops at its deadline, on cancellation, local failure, or restart—not on ACK. It does not change or restore the configuration.

Starting pairing while settings work is pending (including its grace period), or changing a zone during pairing, returns `409`. Cancellation remains allowed. Acceptance does not prove pairing succeeded; verify on the heater and with a later temperature change.

## Shared request and response rules

- `201` includes a `Location` header for the new zone.
- `202` means the controller accepted the instruction and scheduled it internally for dispatch. It does not wait for radio transmission or confirm heater behavior. No tracking ID, status endpoint, or radio diagnostics are returned; later dispatch failures do not change the successful acceptance response.
- For overlapping changes to the same field, the latest accepted update wins; omitted fields remain unchanged.
- Retries are treated as new requests and may repeat actions, including creating another zone.
- Errors use `{"error": {"code": "invalid_field", "message": "Unsupported mode", "field": "mode"}}`; `field` is optional.

| Status | Meaning |
| --- | --- |
| `400` / `422` | Malformed request or unknown field / unsupported or out-of-range value. |
| `401` | Missing, invalid, or revoked bearer token. |
| `403` | Forbidden request, such as a rejected browser origin. |
| `404` / `409` | Resource not found / conflicting request or zone capacity reached. |
| `413` / `415` | Oversized body / unsupported content type. |
| `429` / `503` | Request capacity exceeded / required local service unavailable. |
