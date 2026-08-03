#include "test_util.h"

#include "../main/boards/synora/watering_schedule.h"

int RunWateringScheduleTests() {
    TEST_SUITE_BEGIN();

    // Parses the server's exact field shape, defaults "enabled" to true when
    // absent (matches WateringSchedule.Enabled's own C# default).
    {
        auto schedules = ParseSchedulesJson(
            R"([{"hour":6,"minute":30,"durationSeconds":15,"enabled":true},)"
            R"({"hour":18,"minute":0,"durationSeconds":10,"enabled":false},)"
            R"({"hour":7,"minute":0,"durationSeconds":5}])");
        TEST_CHECK_EQ(schedules.size(), static_cast<size_t>(3));
        if (schedules.size() == 3) {
            TEST_CHECK_EQ(schedules[0].hour, 6);
            TEST_CHECK_EQ(schedules[0].minute, 30);
            TEST_CHECK_EQ(schedules[0].duration_seconds, 15);
            TEST_CHECK(schedules[0].enabled);
            TEST_CHECK(!schedules[1].enabled);
            TEST_CHECK(schedules[2].enabled);  // defaulted, field absent
        }
    }

    // Malformed/non-array input yields an empty list, not a crash.
    {
        TEST_CHECK_EQ(ParseSchedulesJson("not json").size(), static_cast<size_t>(0));
        TEST_CHECK_EQ(ParseSchedulesJson("{}").size(), static_cast<size_t>(0));
        TEST_CHECK_EQ(ParseSchedulesJson("").size(), static_cast<size_t>(0));
        // An entry missing a required field is skipped, not fatal to the rest.
        auto schedules = ParseSchedulesJson(R"([{"hour":6},{"hour":7,"minute":0,"durationSeconds":5}])");
        TEST_CHECK_EQ(schedules.size(), static_cast<size_t>(1));
    }

    // Truncates at kMaxSchedules (20) entries rather than growing unbounded.
    {
        std::string json = "[";
        for (int i = 0; i < 30; i++) {
            if (i > 0) json += ",";
            json += R"({"hour":)" + std::to_string(i % 24) + R"(,"minute":0,"durationSeconds":5,"enabled":true})";
        }
        json += "]";
        auto schedules = ParseSchedulesJson(json);
        TEST_CHECK_EQ(schedules.size(), static_cast<size_t>(kMaxSchedules));
    }

    // Serialize/parse round-trips.
    {
        std::vector<ScheduleEntry> original = {
            {6, 30, 15, true},
            {18, 0, 10, false},
        };
        auto round_tripped = ParseSchedulesJson(SerializeSchedulesJson(original));
        TEST_CHECK_EQ(round_tripped.size(), static_cast<size_t>(2));
        if (round_tripped.size() == 2) {
            TEST_CHECK_EQ(round_tripped[0].hour, 6);
            TEST_CHECK_EQ(round_tripped[0].minute, 30);
            TEST_CHECK_EQ(round_tripped[0].duration_seconds, 15);
            TEST_CHECK(round_tripped[0].enabled);
            TEST_CHECK(!round_tripped[1].enabled);
        }
    }

    // FindDueScheduleIndex: matches only enabled entries at the exact
    // hour/minute; -1 when nothing matches.
    {
        std::vector<ScheduleEntry> schedules = {
            {6, 30, 15, true},
            {6, 30, 99, false},  // same time, but disabled -- must not match
            {18, 0, 10, true},
        };
        TEST_CHECK_EQ(FindDueScheduleIndex(schedules, 6, 30), 0);
        TEST_CHECK_EQ(FindDueScheduleIndex(schedules, 18, 0), 2);
        TEST_CHECK_EQ(FindDueScheduleIndex(schedules, 6, 31), -1);
        TEST_CHECK_EQ(FindDueScheduleIndex({}, 6, 30), -1);
    }

    // IsTimeValid: distinguishes a real synced clock from the near-epoch
    // default an unsynced (no-RTC) board reads back as.
    {
        TEST_CHECK(!IsTimeValid(0));                  // epoch -- definitely unsynced
        TEST_CHECK(!IsTimeValid(kMinValidUnixTime - 1));
        TEST_CHECK(IsTimeValid(kMinValidUnixTime));
        TEST_CHECK(IsTimeValid(kMinValidUnixTime + 100000));
    }

    TEST_SUITE_END();
}
