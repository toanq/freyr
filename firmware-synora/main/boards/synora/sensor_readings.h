#pragma once

#include <algorithm>
#include <array>

#include <cJSON.h>

#include "dashboard_status.h"

// Cached snapshot of the last successful read from each sensor -- pure data,
// no ESP-IDF dependency, so it's host-testable like eyes_geometry.h.
// Field names deliberately echo the server's telemetry contract
// (TemperatureCelsius/HumidityPercent/SoilMoisturePercent/WaterLevelPercent)
// so a future telemetry publisher can reuse this struct without renaming.
struct SensorReadings {
    bool temperature_valid = false;
    float temperature_celsius = 0.0f;

    bool humidity_valid = false;
    float humidity_percent = 0.0f;

    bool soil_moisture_valid = false;
    float soil_moisture_percent = 0.0f;

    bool water_level_valid = false;
    float water_level_percent = 0.0f;
};

// Same field list/shape SynoraBoard::PublishTelemetry() sends over MQTT --
// shared with the self.get_sensor_readings MCP tool so the two don't drift.
// Fields are omitted (not sent as null/0) when a sensor read failed, same
// reasoning as PublishTelemetry's own doc comment. Caller owns the returned
// cJSON* (cJSON_Delete it, or hand it to something that takes ownership --
// e.g. McpTool::Call() takes and deletes a cJSON* ReturnValue).
//
// Each reading also gets a "...Status" good/warn/bad/unknown alongside its
// raw number, and the whole thing gets an overall "potStatus" -- the worst
// status among the currently-valid readings (bad > warn > good), or
// "unknown" if none are valid yet. Reusing dashboard_status.h's thresholds
// (rather than inventing separate ones here) is deliberate: it's the same
// judgment already driving the dashboard tile colors, so the LLM's answer to
// "how's my plant doing" can't disagree with what the device's own screen is
// showing.
inline cJSON* BuildSensorReadingsJson(const SensorReadings& r) {
    cJSON* root = cJSON_CreateObject();

    std::array<TileStatus, 4> statuses = {
        TileStatus::kUnknown, TileStatus::kUnknown, TileStatus::kUnknown, TileStatus::kUnknown};
    int n = 0;

    if (r.temperature_valid) {
        cJSON_AddNumberToObject(root, "temperatureCelsius", r.temperature_celsius);
        statuses[n++] = TemperatureTileStatus(true, r.temperature_celsius);
        cJSON_AddStringToObject(root, "temperatureStatus", TileStatusToString(statuses[n - 1]));
    }
    if (r.humidity_valid) {
        cJSON_AddNumberToObject(root, "humidityPercent", r.humidity_percent);
        statuses[n++] = HumidityTileStatus(true, r.humidity_percent);
        cJSON_AddStringToObject(root, "humidityStatus", TileStatusToString(statuses[n - 1]));
    }
    if (r.soil_moisture_valid) {
        cJSON_AddNumberToObject(root, "soilMoisturePercent", r.soil_moisture_percent);
        statuses[n++] = SoilMoistureTileStatus(true, r.soil_moisture_percent);
        cJSON_AddStringToObject(root, "soilMoistureStatus", TileStatusToString(statuses[n - 1]));
    }
    if (r.water_level_valid) {
        cJSON_AddNumberToObject(root, "waterLevelPercent", r.water_level_percent);
        statuses[n++] = WaterLevelTileStatus(true, r.water_level_percent);
        cJSON_AddStringToObject(root, "waterLevelStatus", TileStatusToString(statuses[n - 1]));
    }

    TileStatus overall = TileStatus::kUnknown;
    for (int i = 0; i < n; i++) {
        if (statuses[i] > overall) {
            overall = statuses[i];
        }
    }
    cJSON_AddStringToObject(root, "potStatus", TileStatusToString(overall));

    return root;
}
