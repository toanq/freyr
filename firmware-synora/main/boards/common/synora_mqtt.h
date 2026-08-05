#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <mqtt_client.h>

// Thin wrapper around esp-mqtt for the device<->server link (telemetry,
// commands, voice control/audio) -- replaces the REST-polling/WebSocket
// transport per the 2026-08-01 MQTT migration decision. All topics are
// scoped under synora/devices/{deviceId}/ where deviceId is the server-
// assigned Guid from DeviceActivation, matching the server's topic scheme.
class SynoraMqtt {
public:
    static SynoraMqtt& GetInstance();

    // No-ops if already started or DeviceActivation has no device id yet
    // (nothing to scope topics under). Safe to call repeatedly -- e.g. once
    // from SynoraBoard's constructor (a returning, already-activated device)
    // and once more right after BleProvisioning's ConnectAndActivate()
    // succeeds (first-time pairing).
    void EnsureStarted();

    // Publishes to synora/devices/{deviceId}/{topic_suffix}.
    void Publish(const std::string& topic_suffix, const std::string& payload, int qos = 1, bool retain = false);

    // Subscribes to synora/devices/{deviceId}/{topic_suffix}; callback fires
    // (on the MQTT client's own task) with the raw payload on every message.
    void Subscribe(const std::string& topic_suffix, std::function<void(const std::string& payload)> callback);

    // Fires whenever the retained `state` topic's `schedules` field arrives
    // (every fresh subscribe re-delivers it, so this covers first-connect and
    // every reconnect) -- passed the schedules array re-serialized as its own
    // JSON string. A settable hook rather than a fixed internal handler (like
    // ApplyIsPaired) since watering scheduling isn't intrinsic to the MQTT
    // client itself; SynoraBoard wires this to WateringScheduler.
    void SetSchedulesUpdateCallback(std::function<void(const std::string& schedules_json)> callback);

    // Fires whenever the retained `state` topic's `serverTimeUnixMs` field
    // arrives (same redelivery-on-subscribe timing as the schedules
    // callback above) -- this is the device's only time source (no public
    // NTP, no RTC yet). SynoraBoard wires this to WateringScheduler::ApplyServerTime.
    void SetTimeUpdateCallback(std::function<void(int64_t unix_time_ms)> callback);

    // Server-side mute toggle (Device.Muted, admin/app-controlled), from the
    // retained `state` topic's `muted` field -- see HandleState(). Read by
    // SynoraBoard::IsMuted() for the dashboard's header speaker icon.
    bool IsMuted() const { return is_muted_.load(); }

    SynoraMqtt(const SynoraMqtt&) = delete;
    SynoraMqtt& operator=(const SynoraMqtt&) = delete;

private:
    SynoraMqtt() = default;
    ~SynoraMqtt() = default;

    static void EventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);

    // Server-confirmed pairing state, from the retained `state` topic's
    // isPaired field -- see ApplyIsPaired().
    void ApplyIsPaired(bool is_paired);

    // The internal "state" topic subscription's handler -- parses isPaired
    // and muted (always) and forwards schedules to schedules_update_callback_
    // (if set).
    void HandleState(const std::string& payload);

    esp_mqtt_client_handle_t client_ = nullptr;
    std::atomic<bool> is_muted_ = false;
    bool started_ = false;
    std::string device_id_;

    // Keyed by full topic string -- looked up on MQTT_EVENT_DATA.
    std::map<std::string, std::function<void(const std::string&)>> subscriptions_;
    std::function<void(const std::string&)> schedules_update_callback_;
    std::function<void(int64_t)> time_update_callback_;
};
