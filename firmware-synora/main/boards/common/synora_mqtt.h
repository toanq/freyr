#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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

    // Publishes to synora/devices/{deviceId}/{topic_suffix}. Fire-and-forget
    // -- returns esp_mqtt_client_publish()'s message id (or -1 if not
    // connected/queue full), but nothing here confirms the broker actually
    // received it. Fine for most callers (telemetry/commands, where a missed
    // one is harmless and the next one supersedes it), but see
    // PublishAndWaitForAck() below for anything that needs to actually know.
    int Publish(const std::string& topic_suffix, const std::string& payload, int qos = 1, bool retain = false);

    // Like Publish(), but blocks (up to timeout_ms) for the broker's PUBACK
    // (QoS 1) before returning -- true only if it actually arrived in time.
    // qos must be 1 (or 2); QoS 0 has no ack to wait for. Confirmed live this
    // matters: a large one-shot payload (mictest's WAV upload) published fine
    // by Publish()'s own accounting (a valid msg_id came back) yet never
    // reached the server at all, because the public WSS tunnel path
    // disconnects/reconnects every 20-100s and the publish landed in a dead
    // window -- Publish() has no way to detect that, only this does.
    bool PublishAndWaitForAck(const std::string& topic_suffix, const std::string& payload, int qos, int timeout_ms);

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

    // Fires once EnsureStarted() has actually resolved a real device_id_ and
    // started the client -- i.e. NOT on the no-op call a not-yet-paired
    // device's constructor makes first. SynoraBoard wires this to
    // InitializeCommandHandlers(), whose commands/water|ping Subscribe()
    // calls otherwise get permanently bound to an empty-device-id topic if
    // registered (as they used to be) only once, right after that first
    // no-op call -- confirmed live: the server-published command never
    // matched the device's subscribed topic, silently, no error either side.
    void SetDeviceReadyCallback(std::function<void()> callback);

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

    // PublishAndWaitForAck()'s wait/signal pair -- created once in
    // EnsureStarted(). Single-slot (one pending ack at a time) is
    // deliberate: today's only caller (mictest's upload) is one-shot and
    // sequential, not worth a full msg_id-keyed map for.
    SemaphoreHandle_t publish_ack_semaphore_ = nullptr;
    std::atomic<int> pending_publish_msg_id_{-1};
    std::atomic<int> last_acked_msg_id_{-1};

    // Keyed by full topic string -- looked up on MQTT_EVENT_DATA.
    std::map<std::string, std::function<void(const std::string&)>> subscriptions_;
    std::function<void(const std::string&)> schedules_update_callback_;
    std::function<void(int64_t)> time_update_callback_;
    std::function<void()> device_ready_callback_;
};
