# Sources grow per phase (see firmware-synora/README.md).
#   Phase 2 adds: protocols/synora_protocol.cc, audio/audio_service.cc (+opus)
#   Phase 3 adds: boards/synora/*sensor*.cc, pump/watering/telemetry
#     (boards/common/button.cc + boards/synora/synora_board.cc + BLE
#     provisioning/device activation started early -- see FEAT.md)
#   Phase 4 adds: application.cc, device_state_machine.cc, display/*, ota.cc
set(SOURCES
    "main.cc"
    "settings.cc"
    "mcp_server.cc"
    "audio/audio_codec.cc"
    "audio/codecs/no_audio_codec.cc"
    "audio/engines/afe_audio_engine.cc"
    "audio/wake_words/wake_word_audio_cache.cc"
    "boards/common/button.cc"
    "boards/common/device_activation.cc"
    "boards/common/ble_provisioning.cc"
    "boards/common/synora_mqtt.cc"
    "boards/common/i2c_device.cc"
    "boards/synora/synora_board.cc"
    "boards/synora/synora_display.cc"
    "boards/synora/dashboard_status.cc"
    "boards/synora/eyes_geometry.cc"
    "boards/synora/synora_voice_session.cc"
    "boards/synora/voice_messages.cc"
    "boards/synora/sht31_sensor.cc"
    "boards/synora/vl53l1x_water_level_sensor.cc"
    "boards/synora/soil_moisture_sensor.cc"
    "boards/synora/text_renderer.cc"
    "boards/synora/font_noto_sans_vi.cc"
    "boards/synora/qr_download.cc"
    "boards/synora/test_console.cc"
    "boards/synora/pump_driver.cc"
    "boards/synora/led_indicator.cc"
    "boards/synora/watering_schedule.cc"
    "boards/synora/watering_scheduler.cc"
    "boards/synora/logo_icon.cc"
    "boards/synora/sensor_icons.cc"
    "boards/synora/status_icons.cc"
)

set(INCLUDE_DIRS
    "."
    "audio"
    "audio/engines"
    "audio/wake_words"
    "boards/synora"
)

# MINIMAL_BUILD is ON (root CMakeLists), so main must declare the IDF
# components it uses -- they are not pulled in automatically.
#   nvs_flash       -> nvs_flash.h
#   esp_hw_support  -> esp_chip_info.h
#   esp_system      -> esp_system.h (esp_get_free_heap_size)
#   esp_psram       -> keeps the SPIRAM Kconfig menu present so CONFIG_SPIRAM=y
#                      applies (board has 8MB octal PSRAM); see Phase 0 notes.
#   esp_timer       -> esp_timer.h (afe engine)
#   esp_driver_i2s  -> driver/i2s_std.h (no_audio_codec)
#   esp_driver_i2c  -> driver/i2c_types.h + i2c_master.h (SHT31 bus, boards/common/i2c_device.cc)
#   esp_adc         -> esp_adc/adc_oneshot.h + adc_cali.h (soil_moisture_sensor.cc)
#   esp-sr          -> AFE + WakeNet (esp_afe_sr_models.h, model_path.h)
#   button          -> iot_button.h (BOOT-button factory reset, boards/common/button.cc)
#   bt              -> NimBLE host/controller (host/ble_hs.h etc, ble_provisioning.cc)
#   esp_wifi        -> esp_wifi.h (ble_provisioning.cc's direct esp_wifi_scan_start call)
#   esp_netif       -> required by 78__esp-wifi-connect's WifiStation
#   esp_event       -> required by 78__esp-wifi-connect's WifiStation (esp_event_handler)
#   esp_http_client -> device_activation.cc (server activation/unlink calls)
#   mbedtls         -> esp_crt_bundle.h / esp_crt_bundle_attach (device_activation.cc's TLS server-cert verification)
#   esp_app_format  -> esp_app_desc.h (ble_provisioning.cc's device_info characteristic)
#   mqtt            -> mqtt_client.h (synora_mqtt.cc; device<->server link per the MQTT migration)
#   esp_driver_spi  -> driver/spi_common.h (synora_display.cc's SPI bus init)
#   esp_lcd         -> esp_lcd_panel_vendor.h / esp_lcd_new_panel_st7789 (synora_display.cc)
#   esp_audio_codec -> esp_opus_enc.h / esp_opus_dec.h (synora_voice_session.cc)
#   78__esp-wifi-connect -> WifiManager/WifiStation/SsidManager (pulled dependency, see idf_component.yml)
#   console         -> esp_console.h (test_console.cc's UART REPL for manual test commands)
# log/heap/freertos are part of the always-available common requirements.
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    PRIV_REQUIRES nvs_flash esp_hw_support esp_system esp_psram esp_timer esp_driver_i2s esp_driver_i2c esp_driver_gpio
                  esp-sr button bt esp_wifi esp_netif esp_event esp_http_client esp_app_format mqtt esp_driver_spi esp_lcd
                  esp_audio_codec 78__esp-wifi-connect esp_adc saleca__vl53l1x_uld_esp_wrapper mbedtls console
)
