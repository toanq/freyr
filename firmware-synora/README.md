# Synora Firmware (greenfield)

An owned ESP-IDF firmware for the Synora device (ESP32-S3 N16R8). This replaces
the previous `firmware/` tree, which was a vendored snapshot of
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32).

## Why greenfield instead of a fork

The valuable audio DSP (AFE: AEC/noise-suppression/VAD + wake word, and the
Opus codec) is **not** xiaozhi's code — it is the Espressif `esp-sr` /
`esp_audio_codec` components, available to any ESP-IDF project via the IDF
Component Manager. And the Synora server (`../server`, a .NET solution incl.
`Synora.Protocol`) is already ours. So instead of maintaining a fork we:

- **Declare the DSP as a dependency** (`main/idf_component.yml`).
- **Copy only the integration glue** worth keeping from xiaozhi (the audio
  pipeline `audio_service` + `afe_audio_engine`), which sits behind clean
  ports (`AudioCodec`, `AudioEngine`, `Protocol`).
- **Write our own** app loop, protocol client, board layer, display, and OTA.

## Attribution

Portions copied from xiaozhi-esp32 are under the MIT License
(Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.). The upstream
`LICENSE` is retained for those files. See `NOTICE` when Phase 1 lands.

## Build-out phases

| Phase | Scope | Milestone |
|-------|-------|-----------|
| 0 | Scaffold: buildable project that boots and logs | **(current)** boots on S3, PSRAM detected |
| 1 | Audio path: esp-sr component + pipeline glue + INMP441 codec | wake word fires |
| 2 | `SynoraProtocol` matching `Synora.Protocol` server | audio round-trip to `Synora.Api` |
| 3 | Board + sensors (DHT11, soil, VL53L1X) + pump/watering/telemetry | sensors report |
| 4 | App state machine + ST7735S/LVGL UI + OTA | full assistant UX |
| 5 | Cutover: retarget web/app/CI, archive `firmware/`, drop `upstream` remote | fork retired |

Until Phase 5, `../firmware/` is kept as a working reference to copy from.

## Build

Requires the ESP-IDF toolchain (>= 5.5.2).

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```
