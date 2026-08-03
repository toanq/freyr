# Firmware Features

Record of features added to the greenfield Synora firmware, in landing order.
See `README.md` for the overall build-out phase plan; features here can land
ahead of strict phase order when explicitly requested.

## Tooling: click-to-talk simulator (`sim/`)

Not a firmware feature -- a host-native (Linux) simulator standing in for
the real firmware where it can't run (no BLE/WiFi/I2S/SPI/display on
ESP-IDF's `linux` target, no ESP32-S3 QEMU here). Reuses
`boards/synora/voice_messages.cc` and `boards/common/mqtt_topics.h`, talks to
the real broker/server via `libmosquitto`, TUI (`ncurses`) stands in for the
eyes display, a command prompt (`click`/`hold`/`quit`) stands in for the
BOOT button. No audio (no mic/speaker on a host) -- control-message flow
only. See `sim/README.md` for usage.

## 1. Synora board + config, GPIO0 hold-to-factory-reset

**What:** Seeds the `boards/synora` board layer:

- `main/boards/synora/config.h` -- full device pin map: audio (INMP441 mic /
  MAX98357A amp, already wired in `main.cc`), BOOT button (GPIO0), call
  button (GPIO2, TTP223, not yet wired to any logic), shared I2C bus
  (SDA=9/SCL=8) for SHT31 (0x44) + VL53L0X (0x29), capacitive soil ADC
  (GPIO1), ST7735S SPI display, pump MOSFET (GPIO38). Only the audio pins and
  `BOOT_BUTTON_GPIO`/`FACTORY_RESET_HOLD_MS` have code behind them today --
  the rest are declared for later features (display/sensors/pump land in
  Phase 3/4).
- `main/boards/common/button.h/.cc` -- thin wrapper around the
  `espressif/button` (iot_button) component, ported from the old
  `firmware/main/boards/common/button.{h,cc}` (MIT) but trimmed to only
  `OnLongPress`, the one callback this feature needs.
- `main/boards/synora/synora_board.h/.cc` -- `SynoraBoard` class. Holding the
  BOOT button (GPIO0) for `FACTORY_RESET_HOLD_MS` (10s) originally called
  `nvs_flash_erase()` + `esp_restart()`; superseded by feature 2 below (now
  unpairs via `DeviceActivation`, not a blind wipe).

**Build:** verified with
`docker run --rm -v $PWD/firmware-synora:/project -w /project espressif/idf:v6.0.2 idf.py set-target esp32s3 build`.
Required adding `espressif/button: ~4.2.0` to `main/idf_component.yml` and
`esp_driver_i2c` + `button` to `main/CMakeLists.txt`'s `PRIV_REQUIRES`
(`MINIMAL_BUILD` means nothing is pulled in implicitly).

**Not done / open:** the second physical button in the pin map
(`CALL_BUTTON_GPIO`), the display, all three sensors, and the pump are
config-only stubs with no driver code yet -- each is its own future feature.

## 2. BLE provisioning + device activation (pairing flow)

**What:** Ports the reference firmware's BLE GATT WiFi-provisioning/pairing
flow (`firmware/main/boards/common/ble_provisioning.{h,cc}` +
`device_activation.{h,cc}`) into the greenfield tree, started ahead of the
click-to-talk voice feature since that feature's WiFi connectivity now
depends on real pairing rather than hardcoded test credentials (explicit
user decision):

- `main/boards/common/device_activation.{h,cc}` -- `DeviceActivation`
  singleton: `POST {server_url}/devices/activation` (device row keyed by MAC,
  server-side dedup), `POST {server_url}/devices/{id}/unlink`, NVS-persisted
  device id/activation code/server URL. Rewritten to call `esp_http_client`
  directly (synchronous perform + an event-handler-accumulated response body)
  instead of the reference's `Board::GetNetwork()->CreateHttp()`, since no
  Board/Network abstraction exists yet in this tree.
- `main/boards/common/ble_provisioning.{h,cc}` -- `BleProvisioning`
  singleton: NimBLE GATT server advertising as `Synora-XXXX`, exposing the
  exact same 128-bit characteristic UUIDs as DESIGN.md section 2
  (`wifi_ssid`/`wifi_password`/`wifi_scan`/`server_url`/`device_info`/
  `settings`/`activation`/`control`/`status`) so the already-built Flutter
  `ble_pairing_screen.dart` works against this device unmodified. `control:
  commit` stages WiFi credentials into `SsidManager`, connects, then calls
  `DeviceActivation::TryActivate()` and notifies the phone with the resulting
  activation code. Diverges from the reference on purpose: BLE is **never**
  torn down after pairing (`Deinit()` is never called from the pairing flow
  itself) -- see `project_ble_always_on`, BLE must stay on permanently
  alongside WiFi.
- `SynoraBoard`'s BOOT-button 10s hold now calls
  `DeviceActivation::Unlink()` (tells the server to clear ownership) +
  `Reset()` (forgets the local device id/code) + `esp_restart()` -- an
  **unpair**, not a blind NVS wipe, matching the BLE `control:
  factory_reset` command (both call the same two `DeviceActivation` methods
  directly rather than one depending on the other, to avoid a cycle between
  the board and BLE provisioning layers). Re-pairing (same or a different
  account) needs no new server/web code -- `POST /devices/pair` already
  treats a same-owner re-pair as a no-op success and a different-owner pair
  as a straight ownership reassignment, and the web admin console already
  renders the current owner (or "unpaired").
- Scope reductions, disclosed rather than silently dropped:
  - `TelemetryReporter`/`WateringScheduler` integration (feeding the newly
    activated device id/server URL to them) is commented out with a TODO --
    neither exists yet in this tree (Phase 3 sensors/pump work).
  - The `device_info` characteristic's `isPaired` field is hardcoded `false`
    (was sourced from `TelemetryReporter`'s cached heartbeat in the
    reference) -- UX-only simplification (app falls back to the full WiFi
    form either way), not a correctness bug.
  - `Application::GetInstance().Schedule(...)` (the reference's task-queue
    framework, not built yet here) is replaced by a small local
    `ScheduleOnTask()` helper in `ble_provisioning.cc` that just spawns a
    one-shot FreeRTOS task -- deferring blocking GATT-callback work (WiFi
    connect, HTTP activation, a full scan) off NimBLE's own host task.

**Dependency gotcha worth remembering:** the published `78/esp-wifi-connect`
component (as pulled via the component manager, version `3.2.2`) does NOT
have `WifiManager`/`WifiStation` methods the pairing flow needs
(`GetLastScanResults`/`ScanAndWait`/`Disconnect`/`ConnectToKnownNetwork`) --
the reference firmware's own vendored copy, same version number, is a local
hand-patch adding exactly these (needed because `esp_wifi_scan_get_ap_records()`
frees the driver's scan buffer on first read, so something has to cache
results at scan-done time for a second consumer to see them later). Copied
that patch's `wifi_manager.{h,cc}` + `wifi_station.{h,cc}` over the
freshly-fetched files in `managed_components/78__esp-wifi-connect/` (noted
inline there: **a fresh `idf.py fullclean` + component re-fetch will silently
overwrite this patch** -- reapply from `firmware/managed_components/...` if
that happens).

**Build:** verified with the same docker `espressif/idf:v6.0.2` command as
feature 1. Needed `78/esp-wifi-connect: ~3.2.2` added to
`main/idf_component.yml`, and `bt` (NimBLE), `esp_wifi`, `esp_netif`,
`esp_event`, `esp_http_client`, `esp_app_format`, `78__esp-wifi-connect`
added to `main/CMakeLists.txt`'s `PRIV_REQUIRES`.

**Not done / open:** `test_wifi` and `factory_reset`-via-BLE's finer points
were already TODOs in the reference and remain so; `settings` characteristic
is still an in-memory placeholder (not persisted); no LE Secure Connections
pairing/bonding yet (DESIGN.md's security section calls for it on the
always-on control path) -- tracked as a follow-up, not silently skipped.

## 3. MQTT client (telemetry/commands) -- device<->server link, part 1

**What:** `main/boards/common/synora_mqtt.{h,cc}` -- `SynoraMqtt` singleton
wrapping `espressif/mqtt` (esp-mqtt). Started ahead of the originally-planned
WS/HTTP protocol client per the 2026-08-01 decision to move the whole
device<->server link (telemetry/commands AND voice/audio) to MQTT via
Mosquitto, replacing REST polling and the `/v1/` WebSocket. Topics are scoped
`synora/devices/{deviceId}/...` using the server-assigned Guid from
`DeviceActivation` (matches the server side, being migrated in parallel).

- `EnsureStarted()` no-ops until a device id exists (called from
  `SynoraBoard`'s constructor for a returning already-paired device, and
  again from `BleProvisioning::ConnectAndActivate()` right after first-time
  activation succeeds); on first real start it also registers baseline
  subscriptions: `state` (retained muted/schedules push), `commands/water`,
  `commands/ping` -- all log-only for now, since there's no pump/buzzer
  driver yet (Phase 3).
- `DeviceActivation::Unlink()` now **publishes** to `.../unlink` instead of
  `POST {server_url}/devices/{id}/unlink` -- `TryActivate()` (the initial
  `POST .../devices/activation`) stays REST, since the device has no Guid
  (and therefore no MQTT topics to use) until that call succeeds. Since a QoS
  1 publish only enqueues the message (unlike the old blocking HTTP POST),
  added a 300ms delay before the `esp_restart()` that both unpair call sites
  do right after, so the publish has a chance to actually reach the socket
  first.

**Build:** verified with the same docker command as prior features. Needed
`espressif/mqtt: ~1.0.0` added to `main/idf_component.yml` and `mqtt` added to
`main/CMakeLists.txt`'s `PRIV_REQUIRES`.

**Not done / open:** actual telemetry publishing (no sensors exist yet --
Phase 3) and real handling of the `state`/`commands/*` subscriptions (no
pump/buzzer/mute-aware code yet) are still just logging placeholders. Voice
control/audio topics (part 2 of this migration) are a separate,
still-in-progress feature -- see the task tracker.

## 4. Mochi-style blinking eyes display

**What:** `main/boards/synora/synora_display.{h,cc}` -- `SynoraDisplay`
singleton bringing up the ST7735S panel (config.h's `DISPLAY_*` pins, same
`esp_lcd_new_panel_st7789`-as-ST7735S-driver trick as the reference firmware)
and drawing a simple two-oval "Mochi" face directly into a small RGB565
framebuffer via `esp_lcd_panel_draw_bitmap` -- no LVGL/display framework yet
(the Phase 4 stub in `idf_component.yml` stays commented out; pulling in a
whole UI toolkit for two blinking circles would be pure speculative scope).

- `EyesState { kIdle, kListening, kTalking }` via `SetState()`. Idle = eyes
  closed (a thin eyelash-line oval). Listening/talking both render eyes open
  with a periodic blink (every 3s, closes for 150ms) -- there's no
  mouth/waveform yet to visually distinguish "listening" from "talking", so
  both use the same open-and-blink animation for now.
- A dedicated FreeRTOS task (`AnimationTask`) owns all drawing, polling the
  (mutex-guarded) state roughly every 200ms while idle, or driving the
  blink timing while active.
- Wired up by feature 5 below: `SynoraVoiceSession` calls `SetState()` as the
  voice session transitions.

**Build:** verified with the same docker command as prior features. Needed
`esp_driver_spi` and `esp_lcd` added to `main/CMakeLists.txt`'s
`PRIV_REQUIRES` (both are built into ESP-IDF core, no `idf_component.yml`
entry needed -- unlike the ST7735S/ST7789 vendor packages some *other* panels
need, `esp_lcd_new_panel_st7789` itself ships in the core `esp_lcd`
component).

## 5. Click-to-talk voice pipeline over MQTT

**What:** `main/boards/synora/synora_voice_session.{h,cc}` -- `SynoraVoiceSession`
singleton, the piece that finally makes the board *do* something: a click on
the BOOT button (`boards/common/button.cc`'s new `OnClick`, added alongside
the existing `OnLongPress`) starts a listen turn; a second click stops it and
waits for the server's reply. Talks to the server over the MQTT voice topics
that the server-side MQTT migration (this session, server half done via a
background agent) implements in `VoiceMqttDispatcher`/`LlmVoicePipeline`:

- `synora/devices/{deviceId}/voice/control/up` -- publishes JSON `hello`
  (once, at the start of a turn: version 2, `audio_params` 16kHz/mono/60ms
  opus) then `{"type":"listen","state":"start","mode":"manual"}`; on the
  second click, `{"type":"listen","state":"stop"}`.
- `synora/devices/{deviceId}/voice/audio/up` -- raw Opus bytes (QoS 0, no
  length-prefix framing -- MQTT already frames each publish as a discrete
  message, unlike the old WS endpoint's `XiaozhiBinaryFrame` header).
  `AudioInputTask` (`main.cc`) now feeds every mic chunk to both
  `AfeAudioEngine` (wake-word, always) and `SynoraVoiceSession::FeedMic()`
  (Opus-encode + publish, only while listening) -- two copies of each chunk,
  since `AfeAudioEngine::Feed()` moves its argument.
- `synora/devices/{deviceId}/voice/control/down` and `.../voice/audio/down`
  (subscribed from `SynoraMqtt::EnsureStarted()`, alongside the `state`/
  `commands/*` subscriptions from feature 3) -- JSON control messages are
  logged (no text UI yet -- `stt`/`llm` have nowhere to display to); a `tts`
  `stop` message flips the eyes back to idle. Audio frames are Opus-decoded
  and written straight to the speaker via the existing `AudioCodec`.
- Opus encode/decode via `espressif/esp_audio_codec` (`esp_opus_enc_*`/
  `esp_opus_dec_*`, called directly rather than through the generic
  registered-codec API -- matches the reference firmware's own
  `audio_service.cc` usage). Fixed at 16kHz/60ms/mono to match the server's
  audio_params exactly, so `config.h`'s `AUDIO_OUTPUT_SAMPLE_RATE` changed
  from 24000 to **16000** (was never actually load-bearing before this
  feature -- Phase 1 only used the input side) -- avoids needing a
  resampler (`esp_audio_effects`) on either leg.
- Eyes: `StartListening()` sets `EyesState::kListening`; `StopListening()`
  optimistically sets `kTalking` (a reply is expected imminently); a `tts
  stop` control message sets `kIdle`. Listening and talking render
  identically (open + periodic blink) -- no mouth/waveform to visually tell
  them apart yet.

**Not done / open:** no display of `stt`/`llm` text (no text UI exists);
no barge-in/abort wired to a gesture (the `abort` control message exists
server-side but nothing here publishes it); if a session's `hello` is lost
or reordered relative to `listen start` (MQTT doesn't guarantee cross-publish
ordering as strongly as a single TCP stream did), the server's session
idle-timeout (3 min) is the recovery path, not graceful retry logic here.

**Build:** verified with the same docker command as prior features. Needed
`espressif/esp_audio_codec: ~2.5.0` added to `main/idf_component.yml` and
`esp_audio_codec` added to `main/CMakeLists.txt`'s `PRIV_REQUIRES`.

## 6. Host unit tests for features 4/5's pure logic

**What:** most of this firmware is thin wiring around ESP-IDF hardware/
network drivers (BLE GATT callbacks, MQTT client, SPI display writes, GPIO
button events) -- not meaningfully unit-testable without mocking the entire
ESP-IDF driver surface, which nothing here does. But three pieces of pure
logic (no ESP-IDF/hardware includes) were extracted specifically so they
could be tested with a plain host compiler, no `idf.py`/target device/QEMU
needed:

- `main/boards/synora/eyes_geometry.{h,cc}` -- `FillEllipse()`, pulled out of
  `synora_display.cc` (feature 4). Takes the framebuffer/dimensions as
  parameters instead of reading `DISPLAY_WIDTH`/`HEIGHT` off a class member,
  so it has zero dependencies beyond `<cstdint>`.
- `main/boards/common/mqtt_topics.h` -- `DeviceTopic()`, pulled out of
  `synora_mqtt.cc` (feature 3). Header-only, `<string>` only.
- `main/boards/synora/voice_messages.{h,cc}` -- `BuildHelloMessage()`/
  `BuildListenMessage()`/`IsTtsStopMessage()`, pulled out of
  `synora_voice_session.cc` (feature 5). Uses cJSON, which is itself portable
  C with no ESP32 hardware dependency, so it compiles fine against the
  already-vendored `managed_components/espressif__cjson/cJSON/cJSON.c`
  source on the host.

`firmware-synora/tests/` -- no test framework dependency (Unity is an
on-target ESP-IDF thing; nothing here needs a target), just a `TEST_CHECK`/
`TEST_CHECK_EQ` macro pair (`test_util.h`) and one test file per extracted
piece. `tests/run.sh` builds with a plain host `g++ -std=c++17` and runs the
resulting binary -- `./firmware-synora/tests/run.sh`.

**Not done / open:** this is deliberately narrow -- BLE provisioning, the
MQTT client, the display driver's panel/SPI code, button GPIO wiring, and the
Opus encode/decode integration itself are all untested here and stay covered
only by the on-target docker build (compiles) and manual/on-device testing.
Broadening this would mean either a real ESP-IDF Unity test app (needs
hardware or QEMU, neither set up in this environment) or introducing mock
layers for the ESP-IDF driver APIs (a much bigger abstraction than anything
else in this codebase uses) -- an explicit scope decision, not an oversight.
