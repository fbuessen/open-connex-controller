# Repository workflow

This repository ships firmware for an Arduino Nano ESP32 with an nRF24L01+ radio.

## Scope and documentation

- Keep production firmware in `src/firmware/`, build/check tools in `scripts/`, and firmware tests in `tests/`.
- Keep [README.md](README.md) focused on installation and everyday use. Put hardware/build details in [the firmware guide](doc/firmware.md), browser behavior in [the web guide](doc/web-interface-spec.md), and endpoint contracts in [the REST API reference](doc/rest-api-spec.md).
- Document current message semantics in [the application protocol](doc/connex-protocol-spec.md) and [radio framing](doc/connex-radio-framing.md). Keep experiment history and local evidence out of published documentation.
- Local archives, credentials, device backups, build artifacts, and machine-specific paths must not be committed. Use fictional data for examples and screenshots.
- Work on the active branch. Preserve unrelated and staged changes; do not stage, commit, publish, or upload firmware unless requested.
- Follow the user's environment instructions and discover tool locations rather than assuming a contributor's local setup.

## Build

- Use `src/firmware/dependencies.json` as the dependency source of truth. Board target: `esp32:esp32:nano_nora`.
- Run `python3 -B scripts/check_firmware.py` for dependency, native, web, and documentation checks. Add `--build --output-dir <output-directory>` to compile; this command never uploads.
- Keep build artifacts outside source directories. The core's export hooks write beside the sketch even with `--output-dir`; the check script stages a temporary sketch to prevent source-tree artifacts.
- Edit web assets in `src/firmware/web/`, then run `node scripts/build_firmware_web.cjs`. Do not hand-edit generated `web_assets.h`.

## Device work

- Discover and verify the current port with `arduino-cli board list --format json` before an authorized upload. Never assume a previous port still identifies the device.
- On WSL, compile with the Linux CLI and upload with the Windows CLI. Use `wslpath -w` for build/sketch paths.
- Build and documentation work does not authorize hardware access or RF transmission. A firmware restart can resume normal operation and hourly refreshes.
- Use application-only uploads for upgrades. Inspect and back up durable state before maintenance; do not erase NVS automatically.
- Budget nested NVS serialization/readback buffers against the Nano's 8 KiB loop stack. Use static or heap storage for large records; native tests do not reproduce this limit.

## Validation

- Run the offline firmware checks before hardware validation. Native tests use UndefinedBehaviorSanitizer; `--asan` adds AddressSanitizer.
- For UI changes, extend `tests/firmware_web_test.cjs` where appropriate and run `node scripts/check_firmware_browser.cjs <chromium-executable>`. Syntax checks alone do not verify asynchronous behavior.
- Browser fixtures must match firmware `Connection: close`. Simulate lost mutation responses by withholding them until client timeout; destroying a reused socket can trigger browser transport retries.
- Browser screenshots go to a temporary directory. Use `--screenshots` for synthetic documentation images, inspect them, then copy selected images into `doc/images/`. Never use real credentials.
- Check relative documentation links and payload examples when changed, and run `git diff --check`.
- Automated tests cover firmware logic and simulated browser behavior. Device acceptance remains necessary for hardware transport, storage durability, RF, and heater behavior.

## Workflow memory

Record new, verified development procedures here concisely. Consolidate durable practices into the sections above. Keep device state and experiment results out of this file.
