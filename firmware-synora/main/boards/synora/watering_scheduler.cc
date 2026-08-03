#include "watering_scheduler.h"

#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>
#include <sys/time.h>
#include <utility>

#define TAG "WateringScheduler"

namespace {
constexpr int kCheckIntervalMs = 20000;

// Vietnam is UTC+7 year-round (no DST) -- matches this product's market;
// hardcoded rather than configurable since there's no per-user/per-device
// timezone setting yet (server-side default for now, see project notes).
constexpr int kVietnamOffsetSeconds = 7 * 3600;
}  // namespace

void WateringScheduler::Start(std::function<void(int)> water_callback) {
    water_callback_ = std::move(water_callback);
    LoadPersistedSchedules();
    xTaskCreate(CheckTask, "watering_sched", 4096, this, 2, nullptr);
}

// The device's own MQTT server pushes its UTC time on every retained "state"
// delivery (device boot/reconnect, or whenever an owner/admin action
// republishes it) -- reusing the same trusted channel this device already
// depends on for everything else, rather than adding a second external
// dependency (a public NTP pool) that this bench's network has already shown
// DNS trouble reaching reliably.
//
// settimeofday() is given unix_time_ms shifted by the (hardcoded, see above)
// Vietnam offset -- i.e. the system clock stores *local* time, mislabeled as
// UTC. Every reader (CheckOnce() below, the dashboard's clock) then just
// calls gmtime_r() and gets local wall time directly, with zero POSIX TZ
// string / newlib-IANA-database involvement anywhere in this codebase.
void WateringScheduler::ApplyServerTime(int64_t unix_time_ms) {
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_time_ms / 1000) + kVietnamOffsetSeconds;
    tv.tv_usec = static_cast<suseconds_t>((unix_time_ms % 1000) * 1000);
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "Clock set from server (unix_time_ms=%lld)", static_cast<long long>(unix_time_ms));
}

void WateringScheduler::LoadPersistedSchedules() {
    Settings settings("watering", /*read_write=*/false);
    std::string json = settings.GetString("schedules", "[]");

    std::lock_guard<std::mutex> lock(schedules_mutex_);
    schedules_ = ParseSchedulesJson(json);
    ESP_LOGI(TAG, "Loaded %u persisted schedule(s)", static_cast<unsigned>(schedules_.size()));
}

void WateringScheduler::PersistSchedules(const std::vector<ScheduleEntry>& schedules) {
    Settings settings("watering", /*read_write=*/true);
    settings.SetString("schedules", SerializeSchedulesJson(schedules));
}

void WateringScheduler::ApplySchedulesFromServer(const std::string& schedules_json) {
    auto schedules = ParseSchedulesJson(schedules_json);
    ESP_LOGI(TAG, "Applying %u schedule(s) from server", static_cast<unsigned>(schedules.size()));

    // Persisted before the in-memory swap -- a crash/power-loss between the
    // two still leaves NVS holding the latest list on next boot.
    PersistSchedules(schedules);
    std::lock_guard<std::mutex> lock(schedules_mutex_);
    schedules_ = std::move(schedules);
}

bool WateringScheduler::HasEnabledSchedule() const {
    std::lock_guard<std::mutex> lock(schedules_mutex_);
    for (const auto& entry : schedules_) {
        if (entry.enabled) {
            return true;
        }
    }
    return false;
}

void WateringScheduler::CheckTask(void* arg) {
    auto* self = static_cast<WateringScheduler*>(arg);
    while (true) {
        self->CheckOnce();
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
    }
}

void WateringScheduler::CheckOnce() {
    time_t now = time(nullptr);
    if (!IsTimeValid(now)) {
        return;  // no RTC, and the server hasn't pushed a time since boot yet -- nothing to check against
    }

    struct tm local_time;  // gmtime_r, not localtime_r -- see ApplyServerTime's doc comment
    gmtime_r(&now, &local_time);
    int minute_of_day = local_time.tm_hour * 60 + local_time.tm_min;
    if (minute_of_day == last_triggered_minute_of_day_) {
        return;  // already fired this exact minute -- don't refire on every ~20s tick within it
    }

    int duration_seconds = 0;
    {
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        int index = FindDueScheduleIndex(schedules_, local_time.tm_hour, local_time.tm_min);
        if (index < 0) {
            return;
        }
        duration_seconds = schedules_[index].duration_seconds;
    }

    last_triggered_minute_of_day_ = minute_of_day;
    ESP_LOGI(TAG, "Schedule due at %02d:%02d -- watering for %ds", local_time.tm_hour, local_time.tm_min,
             duration_seconds);
    water_callback_(duration_seconds);
}
