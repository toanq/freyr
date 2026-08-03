#include "watering_schedule.h"

#include <cJSON.h>

std::vector<ScheduleEntry> ParseSchedulesJson(const std::string& json) {
    std::vector<ScheduleEntry> schedules;

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return schedules;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        if (static_cast<int>(schedules.size()) >= kMaxSchedules) {
            break;
        }

        ScheduleEntry entry;
        cJSON* hour = cJSON_GetObjectItem(item, "hour");
        cJSON* minute = cJSON_GetObjectItem(item, "minute");
        cJSON* duration = cJSON_GetObjectItem(item, "durationSeconds");
        cJSON* enabled = cJSON_GetObjectItem(item, "enabled");

        if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || !cJSON_IsNumber(duration)) {
            continue;  // missing required fields -- skip this entry, keep parsing the rest
        }
        entry.hour = hour->valueint;
        entry.minute = minute->valueint;
        entry.duration_seconds = duration->valueint;
        entry.enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
        schedules.push_back(entry);
    }

    cJSON_Delete(root);
    return schedules;
}

std::string SerializeSchedulesJson(const std::vector<ScheduleEntry>& schedules) {
    cJSON* root = cJSON_CreateArray();
    for (const auto& entry : schedules) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "hour", entry.hour);
        cJSON_AddNumberToObject(item, "minute", entry.minute);
        cJSON_AddNumberToObject(item, "durationSeconds", entry.duration_seconds);
        cJSON_AddBoolToObject(item, "enabled", entry.enabled);
        cJSON_AddItemToArray(root, item);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return result;
}

int FindDueScheduleIndex(const std::vector<ScheduleEntry>& schedules, int current_hour, int current_minute) {
    for (size_t i = 0; i < schedules.size(); i++) {
        const auto& entry = schedules[i];
        if (entry.enabled && entry.hour == current_hour && entry.minute == current_minute) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
