# Dev Log

## 2026-07-20 / 2026-07-21 (earlier same session)

### Device pairing & identity
- **Orphaned activation code (404 on pair)**: the app's cached activation code could refer to a device row deleted server-side (e.g. via the web admin console), leaving no way to recover without a factory reset. Root-caused via `psql` (empty `Devices` row) + logcat. Fixed with a new BLE `"reactivate"` control command — the app catches the `/devices/pair` 404, tells the device to mint a fresh activation code on the spot via BLE, and retries once.
- **Device identity/serial from MAC**: `POST /devices/activation` rewritten to look up an existing Device row by MAC address first (dedup) and reuse it (rotating the activation code) instead of always inserting a new row. Added a `DeviceSerial(macAddress)` helper deriving a last-2-MAC-bytes serial (e.g. `FD66`) for default device naming, matching the firmware's own `Synora-XXXX` BLE/AP name convention. `POST /devices/pair` accepts an optional `MacAddress` as a defensive backfill.
- **"I can re-pair the same device, that shouldn't happen"** — flagged as a requirement; addressed initially via the MAC-dedup above, though the deeper fix (the `TryActivate()` one-time-guard bug) wasn't found until the 2026-07-21 session (see above section).

### App bugs
- **Logout button not working**: `UserProfileScreen` is pushed on top of `DeviceListScreen`, so flipping `isLoggedInProvider` (which only affects `MaterialApp.home`) had no visible effect. Fixed by popping back to the root navigator after logout.
- **Avatar "not working" (device + user)**: root-caused via server logs showing successful `AvatarPath` updates with no errors — uploads always worked. The real bug was Flutter's `NetworkImage` cache never refetching since the URL never changes after upload. Fixed with explicit `imageCache.evict(...)` after a successful upload, on both the device detail screen and the user profile screen.
- Email/password update issues reported ("can not update email/password on app") were never reproduced live (server endpoints tested fine directly via curl with a valid token) — no fix applied, revisit if reported again with live evidence.
- Percentage inputs (low-water warning/critical thresholds) capped to 0–100 via `_PercentInputFormatter`; critical is now the first input, editing it auto-raises warning to match if warning would otherwise be lower than the new critical value.
- Pairing screen: replaced the Wi-Fi dropdown with a fixed form (SSID/password fields, connect button) plus a scrollable selectable list of nearby networks below it; tapping a network fills the SSID field. "Quét lại" (rescan) changed from a text button to an icon-only button, colored to match the surrounding text.

### Deployment
- Added release build support to the Flutter dev-compose workflow (`flutter build apk --release` alongside `--debug`).
- Built and deployed both `dev.apk` and `release.apk` to the web server's `/downloads/{dev,release}` paths (nginx `Content-Disposition`/MIME-type config for extension-less APK downloads), with a public download page (anonymous access, no auth).
- `firmware/docker-compose.dev.yml`: added a persistent `idf-dev` service (`tail -f /dev/null`, ccache volume) so ESP-IDF builds don't re-run the ~30s environment setup on every invocation. Initially missing `/dev/ttyACM0` device passthrough, which only surfaced later when flashing (not building) was attempted through it — fixed by adding `devices: - /dev/ttyACM0:/dev/ttyACM0` (see 2026-07-21 section).

### Notable debugging/tooling lessons from this stretch
- Concurrent `docker run --device=/dev/ttyACM0 ... cat` serial captures left running without cleanup silently blocked later captures from seeing anything.
- `stty -F /dev/ttyACM0 115200 raw -echo` could hang indefinitely under certain conditions, blocking any command chained after it with `;` — fixed by wrapping with `timeout 3`.
- `wait <pid>` doesn't work across separate Bash tool calls (the PID isn't a child of the new shell) — switched to `docker wait <container-id>` for reliably blocking on a background container's completion.
- A `BASH_ENV=/opt/esp/idf/export.sh` auto-source attempt on the persistent `idf-dev` container caused a process explosion (every nested bash shell `idf.py` spawns re-sourced the activation script recursively) — fixed by removing `BASH_ENV` and requiring explicit `. export.sh &&` prefixing per top-level command instead.
- Device uptime (ms since boot, ESP-IDF logs) vs. wall-clock (`adb logcat` timestamps) require a freshly-computed anchor each time; several rounds of "no data arrived" conclusions were actually just misaligned timestamps or an already-expired capture window.

## 2026-07-21

### Pairing / activation
- **`/devices/activation` silently unpaired owned devices.** It unconditionally cleared `OwnerUserId`/`PairedAt` on every call for a known MAC, assuming it only ever ran on first-boot/factory-reset. In practice the device also re-activates any time the app re-runs Wi-Fi provisioning (e.g. a stale on-device `isPaired` cache), which was quietly unpairing already-owned devices. Fixed to only clear pairing state if it wasn't already paired.
- **`/devices/pair` now idempotent for the same owner** — a redundant pairing attempt by the device's actual owner returns success instead of a 409 conflict.
- **`DeviceActivation::TryActivate()` never refreshed after the first activation**, even across reboots (persisted to NVS) — every subsequent "commit" replayed the same original activation code, which 404s the instant that DB row is deleted server-side. Fixed by calling `Reset()` before every explicit BLE pairing commit (`ConnectAndActivate()`), since a "commit" is always a deliberate user action and the server-side MAC dedup already preserves an existing pairing safely.
- Added diagnostic `SYNORA_DEBUG` logging around the activation-code/pair-call path in `ble_pairing_screen.dart` to trace the above.

### Wi-Fi scan / provisioning
- **Reconnect churn**: `WifiStation::HandleScanResult()` called `StartConnect()` (disconnect+reconnect) on every scan that found a known SSID, even while already connected to it — causing periodic TCP/TLS failures that broke in-flight activation/telemetry requests. Fixed to skip reconnecting when already connected.
- **Scan-while-connected degradation**: a station still associated to an AP gets a severely degraded scan result (confirmed live: a ~17-network environment returned just the connected network, or nothing). `HandleRescan()` now disconnects first (`WifiStation::Disconnect()`/`ConnectToKnownNetwork()`, new APIs with an auto-reconnect-suppression flag) to get a clean full scan, then reconnects to the previous network if the user's subsequent commit doesn't pan out (Wi-Fi timeout, activation failure, or BLE disconnects before committing).
- Added a boot-window retry in `HandleRescan()`: a scan landing 0 APs (common right after boot or mid-reconnect) retries once after a short delay before serving an empty list.
- Hedge in `BuildNextScanChunk()`: always include the currently-connected (or just-disconnected-for-rescan) network in the first chunk, even if the raw scan result missed it.
- Root-caused the original empty-Wi-Fi-list saga (multi-session): 15s timeout race → `lastValueStream` replay bug (fixed by switching to `onValueReceived`) → 512-byte GATT attribute cap (fixed with pagination) → read-callback-invoked-twice bug (fixed by splitting `ScanWifiNetworksJson()`/`BuildNextScanChunk()`) → the reconnect-churn/scan-while-connected issues above.

### App fixes
- **Device detail screen crash** ("BoxConstraints has non-normalized height constraints"): the metric-tile `GridView.count(childAspectRatio: 1.3)` forced a fixed cell height derived only from width, while tile content sizes to its actual text. Replaced with a plain `Row`/`Expanded` layout so tiles size to their natural height.
- **Watering-schedule time picker crash** (same error signature): Flutter's `TimePickerEntryMode.input` (the keyboard-toggle button) throws on this SDK. Fixed by setting `TimePickerEntryMode.dialOnly` to remove the toggle; added quick-select time presets (06:00/07:00/12:00/18:00/20:00 + custom) via a bottom sheet so the picker is rarely needed at all.
- **`Switch` inside `ListTile.trailing`** hedge: added `materialTapTargetSize: MaterialTapTargetSize.shrinkWrap` defensively (same crash class, different trigger).
- Tap a schedule row to edit its time/duration in place.
- Removed the redundant "Đã ghép nối / Chưa ghép nối" label under the device avatar in the device list.
- Password field auto-focuses and shows the keyboard after picking a Wi-Fi network during BLE pairing.

### Server
- **AI care suggestion moved server-side**: was duplicated client-side logic (`_statusFor` thresholds + suggestion text) in `device_detail_screen.dart`; now computed once in `Program.cs` (`ComputeCareSuggestion`) and returned as `careSuggestion` on `GET /devices` and `GET /devices/{id}`.
- **Swagger/OpenAPI added** (Swashbuckle.AspNetCore), reachable at `/swagger` (or `/api/swagger` through the public domain), with a Bearer-auth button wired up.
  - Fixed anonymous-type response shapes that broke Swagger UI rendering entirely ("Unable to render this definition") — Swashbuckle can't produce a valid `$ref` name for an anonymous type wrapping another anonymous type. Replaced with named DTOs (`DeviceSummaryDto`, `SensorReadingDto`, `UserSummaryDto`).
  - Fixed the Swagger UI spec URL being hardcoded absolute (`/swagger/v1/swagger.json`), which breaks under Traefik's `/api`-prefix-stripping (routes to the web SPA instead of the API). Changed to a relative URL.
  - Cloudflare was caching the Swagger static JS/CSS by file extension for hours, serving stale pre-fix config after each redeploy. Added `Cache-Control: no-store` on `/swagger/*` responses; required a one-time manual Cloudflare cache purge to clear the already-cached copy.
- `/devices/activation` and `/devices/pair` fixes above.

### Firmware/infra tooling
- `docker-compose.dev.yml`'s persistent `idf-dev` service needed `devices: - /dev/ttyACM0:/dev/ttyACM0` added to flash (not just build) from the long-lived container.

### Hardware review (Wokwi diagram) & pin reassignments
- Reviewed a Wokwi circuit diagram against the real `config.h`/`synora_board.cc` pin assignments. Found and had corrected, over a few iterations: the mic (INMP441) was wired with 6 pins instead of 3, duplicating the speaker's DOUT/BCLK/LRCK pins onto the same GPIOs (42/15/16) — a real short/bus-contention bug, not just a labeling issue. Final corrected version: mic on WS/SCK/DIN (41/39/40) only, speaker on DOUT/BCLK/RLCK (42/15/16) only, no shared pins. Everything else (VL53L1X I2C, DHT11, soil sensor ADC, ST7735S SPI display, capacitive touch button, MOSFET pump gate drive with flyback diode) already matched the firmware correctly.
- Flagged hardware-improvement suggestions (not yet implemented in firmware, hardware-side items): bulk decoupling capacitor (220–470µF, 16V, electrolytic) right at the pump motor's VCC/GND to prevent inrush-current brownout of the ESP32 on a shared rail; soil moisture sensor corrosion risk from constant-voltage resistive probes (mitigate by only powering the sensor during its read window via a GPIO-switched supply instead of hardwired VCC); confirm the pump MOSFET is genuinely logic-level (fully enhances at 3.3V Vgs); no overcurrent protection (polyfuse) on the motor branch; no filter capacitor on the soil-moisture ADC line; no battery charge/protection circuitry represented if running off the 3.7V Li-ion pack mentioned separately.
- **GPIO reassignments** (config.h), each built and flashed to the connected board:
  - Speaker I2S DOUT/BCLK/LRCK: `GPIO42/15/16` → `GPIO4/5/6` (two intermediate steps: first moved DOUT alone to GPIO7, then all three to 4/5/6 together for cleaner PCB grouping).
  - VL53L1X (water level) I2C SCL: `GPIO9` → `GPIO18` (SDA stayed on GPIO8).
  - Declined a request to put I2C on `GPIO3`/`GPIO46` — both are ESP32-S3 strapping pins (boot-mode/ROM-log-control latches read at reset); a permanently-pulled-up I2C bus on either risks unpredictable boot behavior. Recommended sticking to genuinely general-purpose pins instead.
  - Removed the TFT backlight control entirely (`DISPLAY_BACKLIGHT_GPIO`/`GetBacklight()` override/`PwmBacklight`) since this panel's backlight is hardwired to VCC with no control pin — freed GPIO21. Confirmed safe via `Board::GetBacklight()`'s own `nullptr` default (existing precedent: several other boards in this codebase already omit the override) and existing null-checks at every call site (`mcp_server.cc`, `wifi_board.cc`, etc.).
- General note worth remembering: ESP32-S3's GPIO matrix lets nearly any digital peripheral signal (I2S/SPI/UART/I2C) move to any general-purpose pin freely; the real constraints are analog-only functions (ADC needs an ADC-capable pin, e.g. ADC1 = GPIO1–10), the fixed non-matrix-routed pins (embedded flash/PSRAM SPI bus — GPIO33-37 in this project's octal-PSRAM config; native USB D+/D- if ever used), and the four strapping pins (GPIO0, 3, 45, 46) which can misbehave with an external pull resistor (like an I2C bus) attached.
