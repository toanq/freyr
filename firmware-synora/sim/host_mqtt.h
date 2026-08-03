#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <mosquitto.h>

// Host-native MQTT client (libmosquitto) for the click-to-talk simulator --
// same shape as boards/common/synora_mqtt.h's SynoraMqtt (Publish/Subscribe
// scoped under synora/devices/{deviceId}/), but talking to a real broker
// over a real TCP socket instead of esp-mqtt, since this runs on Linux, not
// an ESP32. See firmware-synora/sim/README.md for why the real firmware
// can't just run here directly.
class HostMqtt {
public:
    HostMqtt(std::string host, int port, std::string username, std::string password, std::string device_id);
    ~HostMqtt();

    bool Connect();  // Blocks briefly; returns false on failure.
    void StartLoop(); // Spawns libmosquitto's own background network thread.
    void Stop();

    void Publish(const std::string& topic_suffix, const std::string& payload, int qos = 1, bool retain = false);
    void Subscribe(const std::string& topic_suffix, std::function<void(const std::string& payload)> callback);

    bool IsConnected() const { return connected_; }

    HostMqtt(const HostMqtt&) = delete;
    HostMqtt& operator=(const HostMqtt&) = delete;

private:
    static void OnConnect(struct mosquitto* mosq, void* user_data, int rc);
    static void OnDisconnect(struct mosquitto* mosq, void* user_data, int rc);
    static void OnMessage(struct mosquitto* mosq, void* user_data, const struct mosquitto_message* msg);

    struct mosquitto* mosq_ = nullptr;
    std::string host_;
    int port_;
    std::string username_;
    std::string password_;
    std::string device_id_;
    bool connected_ = false;

    std::mutex subscriptions_mutex_;
    std::map<std::string, std::function<void(const std::string&)>> subscriptions_;
};
