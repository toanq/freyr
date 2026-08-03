#pragma once

#include <ctime>
#include <string>
#include <vector>

// Pure schedule data/parsing, no ESP-IDF/hardware dependency -- kept separate
// from watering_scheduler.h (which owns NVS/SNTP/the actual check task) so
// it's host-testable, same reasoning as sensor_readings.h/voice_messages.h.
//
// Mirrors the server's per-schedule shape exactly (Synora.Core.WateringSchedule
// via MqttService's retained `state` topic): Hour/Minute/DurationSeconds/Enabled.
struct ScheduleEntry {
    int hour = 0;
    int minute = 0;
    int duration_seconds = 0;
    bool enabled = true;
};

// Parses a JSON array of {"hour":H,"minute":M,"durationSeconds":D,"enabled":B}
// objects (the server's `schedules` field, whether pulled live off the
// retained state topic or read back from this device's own NVS-persisted
// copy). Malformed/non-array input yields an empty list rather than
// asserting -- a schedule fetch/parse failure should mean "no schedules
// active", never a crash. Truncated to kMaxSchedules entries (logged by the
// caller, not here, since this file has no logging dependency) -- a sane
// bound on both the NVS blob size and FindDueScheduleIndex's per-tick scan.
constexpr int kMaxSchedules = 20;
std::vector<ScheduleEntry> ParseSchedulesJson(const std::string& json);

// Inverse of ParseSchedulesJson -- for persisting the current schedule list
// to NVS so it survives a reboot without needing the server again.
std::string SerializeSchedulesJson(const std::vector<ScheduleEntry>& schedules);

// Index of the first *enabled* entry whose hour/minute exactly matches
// (current_hour, current_minute), or -1 if none match. Callers are
// responsible for their own "already fired this minute" de-dup (see
// WateringScheduler) -- this function is a pure, stateless lookup.
int FindDueScheduleIndex(const std::vector<ScheduleEntry>& schedules, int current_hour, int current_minute);

// This board has no battery-backed RTC, so time(nullptr) reads back at/near
// the Unix epoch until SNTP has synced at least once since boot -- shared by
// WateringScheduler (skip schedule checks against nonsense time) and the
// dashboard's clock (show "--:--" instead of "1970" while unsynced) so the
// two don't drift onto different thresholds. 1700000000 ~= 2023-11-14,
// comfortably before this feature shipped and comfortably after "definitely
// unsynced".
constexpr time_t kMinValidUnixTime = 1700000000;
inline bool IsTimeValid(time_t unix_time) {
    return unix_time >= kMinValidUnixTime;
}
