# Web interface guide

Open **http://open-connex.local/** on your home network, or use the controller's IP address from your router. The interface works on desktop and mobile.

**Zones** is the dashboard for temperature and mode controls. **Settings** contains Zones, Security, Wi-Fi, and Controller tabs. All displayed settings are requested values; the controller does not receive room temperatures or heater connectivity.

## Setup and sign-in

On first boot, join **OpenConnex-Setup-&lt;suffix&gt;** and open its captive page or **http://192.168.4.1/**. Select a network from the scan results or enter an SSID manually, including hidden networks. A blank Wi-Fi password selects an open network.

Choose and confirm a unique web password of 10–128 UTF-8 bytes. There is no default password. First enrollment is available only while the setup portal is active.

The web password saves before the Wi-Fi connection trial. If that trial fails, retry Wi-Fi using the password already established. The trial times out after 30 seconds. A successful connection shows instructions to rejoin the home network and closes the hotspot after five seconds.

Sign in with the web password. Sessions last 30 days from sign-in; activity does not extend them. Reboot, password change, and setup reset end sessions sooner. **Settings → Security → Session** signs out the current session.

HTTP and the setup hotspot are unencrypted. Use a trusted network and keep the controller off the public Internet.

## Dashboard

Each zone card shows its room icon, name, temperature, half-degree minus/plus controls, direct numeric entry, and a Normal/Eco toggle.

- **Normal** displays the base target.
- **Eco** displays the base target plus the Eco offset, with the regular target below it. Editing the displayed Eco temperature adjusts the base target while retaining the offset.

The base-target range is 10–30°C or 50–86°F. Changes save immediately. After ten seconds without another control update to that zone, the controller sends the complete settings five times, one second apart. It also refreshes each zone once hourly after its latest send.

A newer edit replaces pending settings. If a response is lost, further edits to that zone pause until a read-only refresh reconciles the saved state. The browser does not automatically retry uncertain writes.

## Zone settings

**Settings → Zones** lists up to eight zones. **Add zone** asks for a name and room icon. New zones start at 20°C, Normal mode, and zero Eco offset.

The zone editor provides:

| Setting | Behavior |
| --- | --- |
| Name | Changes the display name without changing the heater association. |
| Room icon | Living room, bedroom, office, kitchen, bathroom, or hallway; display-only metadata. |
| Unit | Celsius or Fahrenheit. The editor previews a converted base target rounded to a half-degree step. |
| Eco offset | An integer from −9 through +9 in the selected unit. Review it when changing units; it is not automatically converted. |

Icon changes save separately from heater settings. If only one save succeeds, the interface reports partial success.

### Pair a heater

Put the heater in learning mode using its own controls. In the zone editor, select **Pair a heater → Start pairing**. The controller sends the zone's current settings immediately and every five seconds for 30 seconds.

Check the heater's display, then verify with a temperature change after pairing ends. The countdown is not heater confirmation. **Stop pairing** cancels future pairing sends. Closing the dialog does not cancel pairing; Stop remains available when it is reopened.

Pending control changes can temporarily prevent pairing. Pairing in progress prevents changes to that zone.

### Delete a zone

Deletion requires confirmation and removes the controller's zone configuration. It does not switch off or unpair heaters. Pending radio work can temporarily prevent deletion.

## Security settings

Change the web password by providing the current password and confirming the replacement. A successful change signs out all browser sessions.

Generate, rotate, copy, or revoke the API token for [REST API clients](rest-api-spec.md). The token remains available in Security settings. Rotation replaces it immediately; revocation requires confirmation. Both take effect for API clients immediately and leave web sign-in available.

Passwords are stored as salted hashes. Browser sessions use HttpOnly cookies; credentials are not stored in browser local or session storage. See the [firmware guide](firmware.md#access-control) for implementation limits.

## Wi-Fi settings

Wi-Fi changes require sign-in. Scan or enter a network, supply its password, and connect. The controller retains the previous network until its replacement succeeds and returns to it on failure. Saved passwords are not displayed.

During a connection trial, competing submissions are disabled. If the browser disconnects, reopen the page to check progress before submitting again. Normal Wi-Fi outages retry the saved network and do not open a setup hotspot.

## Button, status, and recovery

| Indication | Meaning |
| --- | --- |
| Pulsing blue | Setup active. |
| Green | Connected and operating normally. |
| Flashing red | Disconnected from Wi-Fi or a local fault. |
| Solid red | Controller standby. |

A short press of the setup button toggles standby. Standby cancels queued radio work and pairing, while keeping settings and network access. It does not switch off heaters. Press again to resume; canceled work is not replayed. A restart also resumes normal operation.

Hold the button for five seconds to clear Wi-Fi credentials and the web password and reopen setup. Zones, icons, device/radio identities, heater settings, and the API token remain. Complete setup with a new web password before reusing the retained token. The hold also wakes a controller in standby.
