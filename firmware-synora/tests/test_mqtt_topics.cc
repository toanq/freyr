#include "test_util.h"

#include "../main/boards/common/mqtt_topics.h"

int RunMqttTopicsTests() {
    TEST_SUITE_BEGIN();

    TEST_CHECK_EQ(DeviceTopic("abc123", "telemetry"), std::string("synora/devices/abc123/telemetry"));
    TEST_CHECK_EQ(DeviceTopic("abc123", "voice/control/up"), std::string("synora/devices/abc123/voice/control/up"));
    TEST_CHECK_EQ(DeviceTopic("", "state"), std::string("synora/devices//state")); // empty device id -- caller's job to avoid, not this function's

    TEST_SUITE_END();
}
