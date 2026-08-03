#pragma once

#include <string>

// Pure string building, no ESP-IDF/hardware dependency -- kept separate from
// synora_mqtt.h (which owns the actual esp-mqtt client) specifically so it
// can be exercised by a plain host unit test (see firmware-synora/tests/).
//
// All device<->server MQTT topics are scoped under synora/devices/{deviceId}/
// -- matches the server's VoiceMqttDispatcher/MqttService topic scheme
// exactly (see FEAT.md feature 3).
inline std::string DeviceTopic(const std::string& device_id, const std::string& topic_suffix) {
    return "synora/devices/" + device_id + "/" + topic_suffix;
}
