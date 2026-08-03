# Synora firmware (greenfield). Formerly a xiaozhi-esp32 fork; now an owned
# ESP-IDF project that pulls the audio DSP (esp-sr / esp_audio_codec) as a
# component rather than vendoring it. See README.md for provenance/attribution.
cmake_minimum_required(VERSION 3.16)

# esp-sr / IDF headers use designated initializers with missing fields.
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Trim the build to the minimal set of components main actually depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.1.0")
project(synora)
