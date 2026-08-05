#ifndef SYNORA_BOARD_H_
#define SYNORA_BOARD_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <driver/i2c_master.h>
#include <esp_event.h>

#include "boards/common/button.h"
#include "dashboard_status.h"
#include "led_indicator.h"
#include "pump_driver.h"
#include "sensor_readings.h"
#include "sht31_sensor.h"
#include "soil_moisture_sensor.h"
#include "vl53l1x_water_level_sensor.h"
#include "watering_scheduler.h"
#include "wifi_manager.h"

// Synora device board layer (ESP32-S3 N16R8). Grows per build-out phase --
// today it owns the BOOT-button unpair action, brings up BLE provisioning +
// device activation, reads the sensor suite (temp/humidity/soil/water
// level) for SynoraDisplay's dashboard screen, and drives the pump/"find my
// device" LED in response to the server's MQTT commands.
class SynoraBoard {
public:
    SynoraBoard();

    // Synchronous I2C/ADC reads, updates the cached readings below. Called
    // from SynoraDisplay's AnimationTask on its own (slower-than-60fps)
    // cadence -- not on a dedicated task, matching this codebase's general
    // one-task-per-concern style (see synora_display.cc's AnimationTask).
    void RefreshSensors();
    SensorReadings GetSensorReadings() const;

    // Publishes the cached readings to synora/devices/{deviceId}/telemetry.
    // Every publish updates the server's Device.LastSeenAt (see
    // MqttService.HandleTelemetryAsync), which is also what the app's
    // "online" badge is computed from (ComputeIsOnline: LastSeenAt within
    // the last 60s) -- so TelemetryTask calling this on a
    // TELEMETRY_REPORT_INTERVAL_SEC cadence is what keeps a device that
    // isn't actively doing anything else from showing as offline. Public
    // (not just TelemetryTask-internal) so the UART test console can also
    // trigger an out-of-cycle publish on demand.
    void PublishTelemetry();

    // For the dashboard's auto-water status bar -- true if the device has
    // any enabled watering schedule (server-pushed, persisted locally so it
    // survives a reboot; see watering_scheduler_).
    bool IsAutoWateringEnabled() const { return watering_scheduler_.HasEnabledSchedule(); }

    // For the dashboard's top-left WiFi status icon. Reuses TileStatus's
    // existing green/amber/red semantics (dashboard_status.h) rather than a
    // bespoke WiFi-specific enum: kGood = connected, kWarn = scanning/
    // connecting, kBad = disconnected/never connected.
    TileStatus GetWifiStatus() const;

    // For the dashboard header's speaker/mute icon -- delegates to
    // SynoraMqtt's own state (server-driven, Device.Muted) rather than
    // caching a copy here, since SynoraMqtt is already the source of truth
    // and there's no board-local logic layered on top (unlike GetWifiStatus).
    bool IsMuted() const;

    // For SynoraDisplay::GetScreen() -- true once WiFi has been down long
    // enough (WIFI_LOST_BLE_FALLBACK_SEC) while paired that CheckWifiLossRecovery()
    // has resumed BLE advertising, so the dashboard can switch to a "WiFi
    // lost, reconnect via the app" screen instead of showing stale sensor
    // tiles as if nothing were wrong.
    bool IsWifiLostRecoveryActive() const { return ble_resumed_for_wifi_loss_.load(); }

private:
    void InitializeFactoryResetButton();
    void InitializeVoiceButton();
    void InitializeSensors();
    void InitializeCommandHandlers();
    void InitializeMcpTools();
    void InitializeWifiStatusTracking();
    void InitializeDnsFallback();
    void FactoryReset();
    void HandleWaterCommand(const std::string& payload);

    // Called every TelemetryTask tick -- resumes BLE advertising
    // (BleProvisioning::Init(), already idempotent) once WiFi has been down
    // for WIFI_LOST_BLE_FALLBACK_SEC while paired, and turns it back off
    // (Deinit()) once WiFi reconnects. No-op while unpaired -- BLE's on/off
    // state in that case is already owned by the constructor's boot check
    // and SynoraMqtt::ApplyIsPaired().
    void CheckWifiLossRecovery();

    // Shared between the MQTT commands/water|ping handlers and the
    // self.do_watering/self.find_device MCP tools -- see InitializeMcpTools().
    void WaterFor(int duration_seconds);
    void TriggerFindMe();

    static void TelemetryTask(void* arg);
    static void OnGotIp(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    Button boot_button_;

    i2c_master_bus_handle_t board_i2c_bus_ = nullptr;  // BOARD_I2C_PORT: SHT31 only, see config.h
    std::unique_ptr<Sht31Sensor> sht31_;
    std::unique_ptr<WaterLevelSensor> water_level_;  // owns its own I2C_NUM_1 bus internally
    std::unique_ptr<SoilMoistureSensor> soil_moisture_;

    mutable std::mutex sensor_readings_mutex_;
    SensorReadings sensor_readings_;

    PumpDriver pump_;
    LedIndicator find_me_led_;
    WateringScheduler watering_scheduler_;

    // Updated from WifiManager's event callback (fires on the WiFi event
    // task) -- read from SynoraDisplay's own task via GetWifiStatus(), so
    // this needs to be atomic rather than plain state.
    std::atomic<WifiEvent> wifi_event_ = WifiEvent::Disconnected;

    // Millisecond timestamp (esp_timer_get_time()/1000 base) of the leading
    // edge of the current disconnected streak; 0 while connected. Latched by
    // the same WiFi event callback as wifi_event_, read by
    // CheckWifiLossRecovery() on TelemetryTask's task.
    std::atomic<int64_t> wifi_disconnected_since_ms_ = 0;

    // True once CheckWifiLossRecovery() has resumed BLE advertising for a
    // sustained WiFi loss while paired -- lets it know to Deinit() again once
    // WiFi reconnects, and lets IsWifiLostRecoveryActive() drive the
    // dashboard's "WiFi lost" screen.
    std::atomic<bool> ble_resumed_for_wifi_loss_ = false;
};

#endif // SYNORA_BOARD_H_
