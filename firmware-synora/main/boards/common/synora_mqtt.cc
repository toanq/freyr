#include "synora_mqtt.h"

#include "ble_provisioning.h"
#include "device_activation.h"
#include "mqtt_topics.h"
#include "boards/synora/synora_voice_session.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>

#include <utility>

static const char* TAG = "SynoraMqtt";

SynoraMqtt& SynoraMqtt::GetInstance() {
    static SynoraMqtt instance;
    return instance;
}

void SynoraMqtt::EnsureStarted() {
    if (started_) {
        return;
    }
    device_id_ = DeviceActivation::GetInstance().GetDeviceId();
    if (device_id_.empty()) {
        return; // Not activated yet -- nothing to scope topics under.
    }

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = CONFIG_SYNORA_MQTT_BROKER_URI;
    config.credentials.username = CONFIG_SYNORA_MQTT_USERNAME;
    config.credentials.authentication.password = CONFIG_SYNORA_MQTT_PASSWORD;
    // esp_mqtt_client_config_t has its own independent TLS config (separate
    // struct from esp_http_client_config_t, see device_activation.cc) --
    // fixing that one didn't cover this one. Same mandatory-verification
    // requirement applies: esp-tls hard-fails without either this or
    // cert_pem/skip_cert_common_name_check set, now that the broker URI is
    // wss:// (Cloudflare-fronted) instead of the old plain mqtt://mosquitto.
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

    client_ = esp_mqtt_client_init(&config);
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
    esp_mqtt_client_start(client_);
    started_ = true;
    ESP_LOGI(TAG, "Starting, device id=%s", device_id_.c_str());

    // Baseline subscription every device wants, regardless of what board
    // features exist yet. Registered here (not by an external caller) so
    // EnsureStarted() is the one place that has to get subscribe-vs-device-
    // id-ordering right (see the class doc comment). "commands/water" and
    // "commands/ping" are NOT registered here -- SynoraBoard::
    // InitializeCommandHandlers() owns those now that pump_/find_me_led_
    // actually exist to act on them (see synora_board.cc).
    Subscribe("state", [](const std::string& payload) { GetInstance().HandleState(payload); });
    Subscribe("voice/control/down", [](const std::string& payload) {
        SynoraVoiceSession::GetInstance().OnControlDown(payload);
    });
    Subscribe("voice/audio/down", [](const std::string& payload) {
        SynoraVoiceSession::GetInstance().OnAudioDown(payload);
    });
}

// Turns BLE off once the server confirms pairing (POST /devices/pair ->
// retained state republish with isPaired:true, per Synora.Api/Program.cs),
// and brings it back if pairing is ever undone (unlink), so the device stays
// re-pairable. Not triggered by BleProvisioning's own local kActivated state,
// which only means the phone has an activation code -- it hasn't necessarily
// called /devices/pair with it yet, and tearing BLE down before that call
// would drop the connection the phone still needs to read that code over.
//
// DeviceActivation::IsPaired() (not a local member here) is the transition
// check, since it's also the persisted copy SynoraBoard reads at boot to
// decide whether to bring BLE up at all -- one source of truth for both.
void SynoraMqtt::ApplyIsPaired(bool is_paired) {
    bool was_paired = DeviceActivation::GetInstance().IsPaired();
    DeviceActivation::GetInstance().SetPaired(is_paired);

    if (is_paired && !was_paired) {
        ESP_LOGI(TAG, "Server confirmed pairing -- turning BLE off");
        BleProvisioning::GetInstance().Deinit();
    } else if (!is_paired && was_paired) {
        ESP_LOGI(TAG, "Server reports no longer paired -- turning BLE back on");
        BleProvisioning::GetInstance().Init();
    }
}

void SynoraMqtt::HandleState(const std::string& payload) {
    ESP_LOGI(TAG, "state (retained): %s", payload.c_str());
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        return;
    }
    ApplyIsPaired(cJSON_IsTrue(cJSON_GetObjectItem(root, "isPaired")));
    is_muted_.store(cJSON_IsTrue(cJSON_GetObjectItem(root, "muted")));

    if (schedules_update_callback_) {
        cJSON* schedules = cJSON_GetObjectItem(root, "schedules");
        if (cJSON_IsArray(schedules)) {
            char* schedules_json = cJSON_PrintUnformatted(schedules);
            schedules_update_callback_(std::string(schedules_json));
            cJSON_free(schedules_json);
        }
    }

    if (time_update_callback_) {
        cJSON* server_time = cJSON_GetObjectItem(root, "serverTimeUnixMs");
        if (cJSON_IsNumber(server_time)) {
            time_update_callback_(static_cast<int64_t>(server_time->valuedouble));
        }
    }
    cJSON_Delete(root);
}

void SynoraMqtt::SetSchedulesUpdateCallback(std::function<void(const std::string&)> callback) {
    schedules_update_callback_ = std::move(callback);
}

void SynoraMqtt::SetTimeUpdateCallback(std::function<void(int64_t)> callback) {
    time_update_callback_ = std::move(callback);
}

void SynoraMqtt::Publish(const std::string& topic_suffix, const std::string& payload, int qos, bool retain) {
    if (client_ == nullptr) {
        return;
    }
    auto topic = DeviceTopic(device_id_, topic_suffix);
    esp_mqtt_client_publish(client_, topic.c_str(), payload.data(), static_cast<int>(payload.size()), qos, retain);
}

void SynoraMqtt::Subscribe(const std::string& topic_suffix, std::function<void(const std::string&)> callback) {
    auto topic = DeviceTopic(device_id_, topic_suffix);
    subscriptions_[topic] = std::move(callback);
    if (client_ != nullptr) {
        esp_mqtt_client_subscribe(client_, topic.c_str(), 1);
    }
}

void SynoraMqtt::EventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* self = static_cast<SynoraMqtt*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_t*>(event_data);

    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected -- (re)subscribing to %d topic(s)", static_cast<int>(self->subscriptions_.size()));
            // Re-issue every subscription on (re)connect -- the broker
            // doesn't remember them across a session that wasn't persistent,
            // and this also covers the very first connect.
            for (const auto& [topic, callback] : self->subscriptions_) {
                esp_mqtt_client_subscribe(self->client_, topic.c_str(), 1);
            }
            break;
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            auto it = self->subscriptions_.find(topic);
            if (it != self->subscriptions_.end()) {
                it->second(std::string(event->data, event->data_len));
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected -- esp-mqtt will auto-reconnect");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default:
            break;
    }
}
