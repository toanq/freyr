# Click-to-talk simulator

Host-native (Linux) simulator for the click-to-talk voice feature. **This is
not the real firmware running on Linux** -- that isn't feasible: ESP-IDF's
`linux` target has no BLE/WiFi/I2S/SPI/display support, and there's no
ESP32-S3 QEMU set up in this environment either (its peripheral coverage is
limited even where it exists). Instead, this reuses the same pure-logic
message building (`../main/boards/synora/voice_messages.cc`) and talks to the
real MQTT broker/server over a real TCP socket via `libmosquitto`, with a TUI
standing in for the eyes display and a command prompt standing in for the
BOOT button.

**No audio.** There's no mic/speaker on a Linux host, so this only exercises
the control-message half of the voice session (`hello`/`listen start-stop`,
`stt`/`llm`/`tts`/`system`/`alert`). Opus encode/decode is ESP-IDF-only and is
covered by the on-target docker build plus the host unit tests for its own
message-building logic (`../tests/`).

## Prerequisites

The docker-compose stack must be up (from the repo root):

```
docker compose up -d postgres mosquitto server traefik
```

## Run

```
./run.sh                    # activates a fresh simulated device each run
./run.sh <existing-guid>    # reuses an existing device id, skips activation
```

Builds a throwaway docker image (`libmosquitto-dev`/`libncurses-dev`/
`python3`, cached after the first run) and runs the simulator joined to the
compose stack's network, so it reaches `mosquitto`/`server` by service name.

## TUI

- Status pane: MQTT connection state, device id, eyes state (text/ASCII),
  and a scrolling log of sent/received messages.
- Prompt line at the bottom. Commands:
  - `click` -- toggle listening (matches the BOOT-button click in the real
    firmware): first click publishes `hello` + `listen start`; second click
    publishes `listen stop` and waits for the server's `tts stop` to flip
    back to idle.
  - `hold` -- publishes to the `unlink` topic (matches the BOOT-button 10s
    hold / unpair gesture -- there's no real `DeviceActivation`/BLE state
    machine here, so this is just the MQTT side of that gesture).
  - `quit` / `exit`.

## Env vars (all optional, defaults match the compose service names)

`SIM_MQTT_HOST` (default `mosquitto`), `SIM_MQTT_PORT` (default `1883`),
`SIM_MQTT_USERNAME` (default `synora_device`), `SIM_MQTT_PASSWORD` (default
empty -- must match `docker-compose.yml`'s `MQTT_PASSWORD` if it's been
overridden from its own default), `SIM_SERVER_URL` (default
`http://server:8080` -- the server's internal container port, not through
Traefik, since this runs inside the compose network directly).
