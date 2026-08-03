// Host-side unit test runner for the pure-logic pieces of the firmware that
// don't touch ESP-IDF/hardware APIs (see eyes_geometry.h, mqtt_topics.h,
// voice_messages.h). Build/run with a plain host compiler -- no idf.py,
// no target device or QEMU:
//
//   g++ -std=c++17 -I. main.cc test_*.cc ../managed_components/espressif__cjson/cJSON/cJSON.c -o run_tests
//   ./run_tests
//
// Everything else in firmware-synora/main (BLE, MQTT client, display driver,
// button, Opus codec integration) is thin wiring around ESP-IDF hardware/
// network drivers and is only exercised by the on-target docker build +
// manual/on-device testing, not here.

#include <cstdio>

int RunEyesGeometryTests();
int RunMqttTopicsTests();
int RunVoiceMessagesTests();
int RunTextRendererTests();
int RunDashboardStatusTests();
int RunWateringScheduleTests();

int main() {
    int failures = 0;
    failures += RunEyesGeometryTests();
    failures += RunMqttTopicsTests();
    failures += RunVoiceMessagesTests();
    failures += RunTextRendererTests();
    failures += RunDashboardStatusTests();
    failures += RunWateringScheduleTests();

    if (failures == 0) {
        std::printf("All host tests passed.\n");
        return 0;
    }
    std::printf("%d host test failure(s).\n", failures);
    return 1;
}
