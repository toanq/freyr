# VAD playback test app's main component. Reuses Synora's own audio codec /
# AFE engine / settings / button sources directly from ../../../main
# (relative paths) rather than duplicating them -- single source of truth
# for the audio stack and the board's I2S/button pin map (config.h). Board
# MQTT/BLE/sensor code is intentionally NOT pulled in: this is a standalone
# test binary, not a slimmed-down copy of the real app. text_renderer.cc +
# font_noto_sans_vi.cc ARE pulled in (unlike synora_display.cc) -- they're
# pure framebuffer/font code with no board/MQTT/sensor dependency, needed for
# main.cc's own minimal on-TFT status screen (live rms/level/VAD state,
# useful since there's no way to watch UART output while also pressing the
# physical button and talking into the mic).
set(SYNORA_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../main")

set(SOURCES
    "main.cc"
    "${SYNORA_MAIN}/settings.cc"
    "${SYNORA_MAIN}/audio/audio_codec.cc"
    "${SYNORA_MAIN}/audio/codecs/no_audio_codec.cc"
    "${SYNORA_MAIN}/audio/engines/afe_audio_engine.cc"
    "${SYNORA_MAIN}/audio/wake_words/wake_word_audio_cache.cc"
    "${SYNORA_MAIN}/boards/common/button.cc"
    "${SYNORA_MAIN}/boards/synora/text_renderer.cc"
    "${SYNORA_MAIN}/boards/synora/font_noto_sans_vi.cc"
)

set(INCLUDE_DIRS
    "."
    "${SYNORA_MAIN}"
    "${SYNORA_MAIN}/audio"
    "${SYNORA_MAIN}/audio/engines"
    "${SYNORA_MAIN}/audio/wake_words"
    "${SYNORA_MAIN}/boards/common"
    "${SYNORA_MAIN}/boards/synora"  # config.h (I2S/button/display pins) + text_renderer/font headers
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    # esp_driver_i2c/esp_adc are pulled in only because config.h (reused
    # as-is from the real firmware, for the I2S/button pin map) also has
    # unrelated sensor macros behind <driver/i2c_types.h>/<hal/adc_types.h>
    # includes -- this app doesn't touch I2C or ADC itself.
    PRIV_REQUIRES nvs_flash esp_hw_support esp_system esp_psram esp_timer esp_driver_i2s esp_driver_i2c
                  esp_driver_gpio esp_adc esp-sr button console esp_driver_spi esp_lcd esp_lcd_ili9341
)

# afe_audio_engine.cc's voice-processing/VAD output path is gated behind
# CONFIG_USE_AUDIO_PROCESSOR, a plain #if rather than a real Kconfig symbol
# anywhere in this codebase (the real firmware never defines it, since its
# pipeline is batch STT, not local-VAD-driven -- see
# ../../main/boards/synora/synora_voice_session.cc's Toggle() comment).
# Defined directly as a build flag here (after idf_component_register, which
# is where the component's actual target exists) instead of adding Kconfig
# plumbing just for this one test app.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_USE_AUDIO_PROCESSOR=1)
