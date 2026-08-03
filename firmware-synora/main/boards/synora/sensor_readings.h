#pragma once

#include <cJSON.h>

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
inline cJSON* BuildSensorReadingsJson(const SensorReadings& r) {
    cJSON* root = cJSON_CreateObject();
    if (r.temperature_valid) {
        cJSON_AddNumberToObject(root, "temperatureCelsius", r.temperature_celsius);
    }
    if (r.humidity_valid) {
        cJSON_AddNumberToObject(root, "humidityPercent", r.humidity_percent);
    }
    if (r.soil_moisture_valid) {
        cJSON_AddNumberToObject(root, "soilMoisturePercent", r.soil_moisture_percent);
    }
    if (r.water_level_valid) {
        cJSON_AddNumberToObject(root, "waterLevelPercent", r.water_level_percent);
    }
    return root;
}
