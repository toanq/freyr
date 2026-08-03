#!/bin/sh
# Builds and runs the host-side unit tests for the ESP-IDF-hardware-free
# pieces of the firmware (see main.cc for what is and isn't covered here).
# No idf.py, no target device or QEMU -- just a host g++.
set -e
cd "$(dirname "$0")"

g++ -std=c++17 -Wall \
    -I. \
    -I../managed_components/espressif__cjson/cJSON \
    main.cc \
    test_eyes_geometry.cc \
    test_mqtt_topics.cc \
    test_voice_messages.cc \
    test_text_renderer.cc \
    test_dashboard_status.cc \
    test_watering_schedule.cc \
    ../main/boards/synora/eyes_geometry.cc \
    ../main/boards/synora/voice_messages.cc \
    ../main/boards/synora/text_renderer.cc \
    ../main/boards/synora/dashboard_status.cc \
    ../main/boards/synora/watering_schedule.cc \
    ../managed_components/espressif__cjson/cJSON/cJSON.c \
    -o /tmp/synora_run_tests

/tmp/synora_run_tests
