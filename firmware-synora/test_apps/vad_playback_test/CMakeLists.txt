# Standalone test app: press the BOOT button to record, AFE's VAD stops the
# recording once the user goes quiet, then it plays back through the
# speaker. Exercises Synora's audio codec + esp-sr AFE VAD engine on real
# hardware, independent of the real firmware (no MQTT/display/BLE/sensors) --
# see main/main.cc for why (synora_voice_session.cc's press-to-talk design
# doesn't use local VAD-triggered auto-stop today; this is what that would
# actually sound/behave like, to evaluate before investing in changing it).
cmake_minimum_required(VERSION 3.16)

# esp-sr / IDF headers use designated initializers with missing fields.
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.1.0")
project(vad_playback_test)
