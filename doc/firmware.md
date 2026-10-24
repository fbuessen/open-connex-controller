# Firmware guide

Open Connex Controller provides eight heater zones, a local web interface, and a token-protected REST API. The sketch is [src/firmware/](../src/firmware/); its entry point is `firmware.ino`. For initial setup and pairing, start with the [README](../README.md).

## Hardware

Use **Arduino Nano ESP32 pin labels**, not ESP32 GPIO numbers. Disconnect power before wiring.

| nRF24L01+ pin | Nano ESP32 pin |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| CE | D7 |
| CSN / CS | D8 |
| SCK | D13 |
| MOSI | D11 |
| MISO | D12 |
| IRQ | Not connected |

Use an nRF24L01+ module supporting 250 kbit/s. Connect a normally open button between **D2 and GND**; the firmware enables the internal pull-up. USB supplies power, and the controller operates without a connected computer.

Connect a **common-anode RGB LED** with current limiting:

| LED connection | Nano ESP32 pin |
| --- | --- |
| Common anode (+) | 3V3 |
| Red cathode (−) | D3 |
| Green cathode (−) | D4 |
| Blue cathode (−) | D5 |

For a bare LED, use a separate series resistor for each color. An illuminated button can combine the switch and LED, but its switch contacts remain separate. Keep radio power and the LED anode at **3.3 V**; do not connect them to 5 V. The onboard and external LEDs display the same status.

## Build and checks

Install Arduino CLI, Python 3.10 or newer, a C++17 compiler (`g++`), and Node.js 22 or newer. The [dependency manifest](../src/firmware/dependencies.json) pins the board package and libraries:

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install ArduinoJson@7.4.3 RF24@1.6.2
```

From the repository root, run:

```sh
python3 -B scripts/check_firmware.py
python3 -B scripts/check_firmware.py --build --output-dir build/firmware
```

The first command validates dependencies, runs native and JavaScript tests, checks generated web assets, and validates documentation links. The second also compiles for `esp32:esp32:nano_nora`. Use `--arduino-cli <executable>` if Arduino CLI is not on your PATH, and `--asan` to add AddressSanitizer to the native tests.

The helper stages the sketch in a temporary directory because the ESP32 core also exports artifacts beside the sketch. Build outputs belong outside `src/firmware/`. The helper does not upload firmware or access hardware.

### Upload

Discover the connected board before uploading:

```sh
arduino-cli board list --format json
arduino-cli upload --fqbn esp32:esp32:nano_nora --port <discovered-port> --input-dir build/firmware src/firmware
```

Keep the board defaults for Arduino pin numbering and USB. On WSL, compile with the Linux CLI and use the Windows CLI for discovery and upload, converting sketch and build paths with `wslpath -w`.

Uploading restarts the controller. It resumes normal operation, including hourly refreshes, and ends browser sessions.

### Web development

Edit files in `src/firmware/web/`, then regenerate the embedded assets:

```sh
node scripts/build_firmware_web.cjs
node scripts/check_firmware_browser.cjs <chromium-executable>
```

The browser check uses the production assets with a loopback HTTP fixture. It exercises desktop and mobile flows without contacting a controller. Add `--screenshots` to generate documentation images using fictional data in a temporary directory.

## Operation

The [web guide](web-interface-spec.md) covers setup, zone management, and access settings. The [API reference](rest-api-spec.md) defines request validation and responses.

| Action | Radio behavior |
| --- | --- |
| Boot or create a zone | No immediate send; hourly refresh starts after a full hour. |
| Read, rename, or delete a zone | No send. Deleting does not unpair or switch off heaters. |
| Update temperature, mode, unit, or offset | Save immediately, wait for ten seconds without another control update to that zone, then send five times at one-second intervals. |
| Pair | Send current settings immediately, then every five seconds for the requested window. |
| Hourly refresh | One send of current settings, measured from the latest send; deferred behind controls or pairing. |
| Enter standby | Cancel pending commands and pairing; suspend refreshes and power down the radio. |

New control updates replace pending settings and cancel remaining older copies. A packet already on air may finish. Delays do not cause catch-up bursts. Restart discards pending work and starts new hourly timers. Losing Wi-Fi does not itself stop scheduled radio work.

The controller stores and displays requested settings; it receives no room-temperature or heater-state telemetry.

## Status light and button

| Light | Meaning |
| --- | --- |
| Pulsing blue | Setup hotspot active. |
| Solid green | Connected to Wi-Fi and operating normally. |
| Solid red | Controller standby. |
| Flashing red | Wi-Fi disconnected or a local hardware, storage, or service fault. |

Standby takes priority over other indications, then faults, then setup. Blue pulses over two seconds; red flashes at 500 ms intervals.

A **short press and release** toggles standby. Wi-Fi, the web interface, and read-only API access remain available. Heater-control writes and pairing starts return `503`. Standby does not switch off heaters: they can continue heating to their last received target. Press again to resume; canceled work is not replayed. Standby is not retained across reboot.

A **five-second hold** wakes the controller, clears Wi-Fi credentials and the web password, and reopens setup. It retains zones, room icons, device/radio identities, and the API token. Complete setup with a new web password before reusing that token. Releasing the long hold does not also toggle standby.

## Upgrades and storage

Use **application-only uploads**, without erasing flash, to preserve configuration. NVS records are versioned and checksummed. The installed firmware must support the stored layout; incompatible or corrupt records fail closed and are not automatically erased or migrated. A Wi-Fi/password reset does not repair incompatible application records.

The controller supports eight active zones and a 64-entry retirement table that prevents reusing deleted zone identities. Exhausting that table prevents further zone allocation/deletion. Renaming an existing zone preserves its identity.

Device identity, zone settings, room icons, and access credentials persist. Browser sessions, pairing windows, pending transmissions, and standby state are volatile.

## Access control

- Web passwords use PBKDF2-HMAC-SHA256 with 10,000 iterations and a random 128-bit salt. The password is not retained in plaintext. Supported older 100,000-iteration records are rehashed at successful login.
- At most four random browser sessions are held in RAM. Sessions expire 30 days after sign-in, or sooner on reboot, password change, or setup reset.
- Cookies are HttpOnly, SameSite=Strict, host-only, and scoped to `/`. Browser mutations require a nonce and pass Host/Origin checks. There is no CORS access or browser local/session storage of credentials.
- Five password verification/enrollment attempts are admitted per minute globally. Hashing runs in a background worker. Password requests can take up to a minute; the browser waits without automatically retrying.
- Every `/api/v1/` endpoint requires a bearer token. Browser cookies do not authorize the public API. The random 256-bit API token is retrievable in Settings and stored in NVS.
- Successful credential changes require verified persistence. Indeterminate writes suspend access instead of issuing new sessions or accepting credentials.

**Use a trusted network.** HTTP, the setup hotspot, NVS contents, and radio commands are not encrypted by this firmware. Password hashing provides limited protection against offline guessing; use a long, unique password. No secure boot, flash encryption, or authenticated radio protocol is supplied.

During setup, network services may be reachable through both the hotspot and the home network. Enrollment is restricted to an active setup portal, but it does not authenticate physical ownership. Ordinary Wi-Fi outages retry the saved network without opening the hotspot. The controller does not route traffic between networks.

## Tests

The [firmware workflow](../.github/workflows/firmware.yml) runs the same checks on GitHub, builds the Nano firmware, and exercises the browser interface.

| Suite | Coverage |
| --- | --- |
| Native C++ | Payload encoding, limits, API/JSON handling, schedules, pairing, identities, storage/radio failure policy, button behavior, Wi-Fi policy, DNS parsing, access control, and timer rollover. |
| JavaScript | Asynchronous setup and zone-write behavior, error handling, and generated asset freshness. |
| Browser | Production HTML/CSS/JS, setup, sign-in, token/password management, zone controls, uncertain responses, and desktop/mobile layout against a simulated server. |

Native access tests use injected cryptography and storage. Browser tests simulate the backend. These checks do not replace device testing of mbedTLS, HTTP integration, NVS durability, RF range, or heater behavior.
