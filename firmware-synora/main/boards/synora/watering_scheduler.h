#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "watering_schedule.h"

// Runs watering schedules locally, independent of server/MQTT connectivity --
// the whole point being that scheduled watering keeps happening while the
// device is offline. The board has no battery-backed RTC (yet -- a hardware
// add-on is planned separately), so wall-clock time only exists once the
// server has pushed one at least once since boot (see
// ApplyServerTime()/CheckOnce() -- a device that reboots while offline just
// pauses scheduled watering until it reconnects, rather than firing at a
// bogus time). Whatever eventually reads a real RTC at boot would feed into
// exactly the same settimeofday() call ApplyServerTime already makes.
class WateringScheduler {
public:
    // Loads any NVS-persisted schedule list and spawns the periodic check
    // task. water_callback is invoked with a schedule's duration_seconds
    // when it's due -- SynoraBoard binds this to WaterFor() (the same
    // helper the MQTT commands/water handler and the self.do_watering MCP
    // tool already share, so every watering trigger goes through one path).
    void Start(std::function<void(int)> water_callback);

    // Called from SynoraMqtt's retained "state" topic handler whenever the
    // server pushes an updated schedule list -- parses, persists to NVS
    // (so it survives a reboot without the server), and swaps the live list.
    void ApplySchedulesFromServer(const std::string& schedules_json);

    // Called from the same "state" handler whenever the server pushes its
    // current UTC time (serverTimeUnixMs) -- sets the device's clock. See
    // the .cc for why this is the time source instead of public NTP, and
    // why it stores local time "pretending" to be UTC.
    void ApplyServerTime(int64_t unix_time_ms);

    // For the dashboard's auto-water status bar.
    bool HasEnabledSchedule() const;

    WateringScheduler(const WateringScheduler&) = delete;
    WateringScheduler& operator=(const WateringScheduler&) = delete;
    WateringScheduler() = default;

private:
    void LoadPersistedSchedules();
    void PersistSchedules(const std::vector<ScheduleEntry>& schedules);
    static void CheckTask(void* arg);
    void CheckOnce();

    std::function<void(int)> water_callback_;
    mutable std::mutex schedules_mutex_;
    std::vector<ScheduleEntry> schedules_;
    // Minute-of-day (0-1439) of the last schedule this task fired -- guards
    // against refiring the same schedule on every ~20s tick within the
    // minute it matched. Plain member (not atomic): only ever touched from
    // CheckTask's own task, never concurrently.
    int last_triggered_minute_of_day_ = -1;
};
