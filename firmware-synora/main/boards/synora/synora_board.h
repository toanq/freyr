#ifndef SYNORA_BOARD_H_
#define SYNORA_BOARD_H_

#include <memory>
#include <mutex>
#include <string>

#include <driver/i2c_master.h>
#include <esp_event.h>

#include "boards/common/button.h"
#include "led_indicator.h"
#include "pump_driver.h"
#include "sensor_readings.h"
#include "sht31_sensor.h"
#include "soil_moisture_sensor.h"
#include "vl53l1x_water_level_sensor.h"
#include "watering_scheduler.h"

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

private:
    void InitializeFactoryResetButton();
    void InitializeVoiceButton();
    void InitializeSensors();
    void InitializeCommandHandlers();
    void InitializeMcpTools();
    void InitializeDnsFallback();
    void FactoryReset();
    void HandleWaterCommand(const std::string& payload);

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
};

#endif // SYNORA_BOARD_H_
