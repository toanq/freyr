#include "synora_mqtt.h"

#include "ble_provisioning.h"
#include "device_activation.h"
#include "mqtt_topics.h"
#include "boards/synora/synora_voice_session.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_system.h>

#include <utility>

static const char* TAG = "SynoraMqtt";

SynoraMqtt& SynoraMqtt::GetInstance() {
    static SynoraMqtt instance;
    return instance;
}

void SynoraMqtt::EnsureStarted() {
    if (started_) {
        // Every subscription's full topic string was resolved against
        // device_id_ at the time Subscribe() was called (see DeviceTopic()
        // below) -- if the device has since been re-activated to a
        // genuinely different server-side id (e.g. re-paired after being
        // deleted server-side, all without an intervening reboot), this
        // already-running client has no way to re-scope itself: it stays
        // subscribed to the OLD id's topics forever, including "state", so
        // it can never receive the retained isPaired:true that would
        // otherwise confirm the fresh pairing. Reboot for a clean slate
        // rather than silently getting stuck showing the pairing screen
        // despite the server correctly considering the device paired --
        // confirmed live: this exact mismatch left a device paired
        // server-side but stuck on the TFT's pairing screen indefinitely.
        std::string current_device_id = DeviceActivation::GetInstance().GetDeviceId();
        if (!current_device_id.empty() && current_device_id != device_id_) {
            ESP_LOGW(TAG, "device_id changed (%s -> %s) after MQTT already started -- restarting for a clean slate",
                     device_id_.c_str(), current_device_id.c_str());
            esp_restart();
        }
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
    // esp-mqtt's own default (MQTT_TASK_STACK, 6*1024) is what MQTT_EVENT_DATA
    // dispatch runs on -- i.e. every Subscribe() callback here, including
    // EventHandler() -> SynoraVoiceSession::OnControlDown()'s cJSON parse +
    // display updates for control/down. Confirmed live: a stack overflow in
    // "mqtt_task" hit on the first sizeable payload (a tts:sentence_start
    // reply, longer than the hello/stt/llm messages that had already come
    // through fine) -- doubling here is a small, affordable ask against
    // internal RAM (unlike the audio_input fix, this task has no PSRAM-stack
    // option in esp_mqtt_client_config_t) for a callback chain that's JSON
    // parsing and a few function calls, not DSP.
    config.task.stack_size = 12288;

    client_ = esp_mqtt_client_init(&config);
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
    esp_mqtt_client_start(client_);
    started_ = true;
    publish_ack_semaphore_ = xSemaphoreCreateBinary();
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

    // Only reached once device_id_ is real -- see SetDeviceReadyCallback()'s
    // doc comment for why this can't just run unconditionally at boot.
    if (device_ready_callback_) {
        device_ready_callback_();
    }
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

void SynoraMqtt::SetDeviceReadyCallback(std::function<void()> callback) {
    device_ready_callback_ = std::move(callback);
}

int SynoraMqtt::Publish(const std::string& topic_suffix, const std::string& payload, int qos, bool retain) {
    if (client_ == nullptr) {
        return -1;
    }
    auto topic = DeviceTopic(device_id_, topic_suffix);
    return esp_mqtt_client_publish(client_, topic.c_str(), payload.data(), static_cast<int>(payload.size()), qos,
                                    retain);
}

bool SynoraMqtt::PublishAndWaitForAck(const std::string& topic_suffix, const std::string& payload, int qos,
                                      int timeout_ms) {
    if (publish_ack_semaphore_ == nullptr) {
        return false;
    }
    // Drain any stale "give" from an earlier, already-timed-out wait --
    // otherwise this call could immediately (and wrongly) report success on
    // an ack that belonged to a previous, unrelated publish.
    xSemaphoreTake(publish_ack_semaphore_, 0);

    int msg_id = Publish(topic_suffix, payload, qos, /*retain=*/false);
    if (msg_id < 0) {
        return false;
    }
    pending_publish_msg_id_.store(msg_id);
    // The PUBACK can land before the store above -- EventHandler records
    // every acked id, so check for that instead of waiting out the timeout.
    if (last_acked_msg_id_.load() == msg_id) {
        return true;
    }
    return xSemaphoreTake(publish_ack_semaphore_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
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
        case MQTT_EVENT_PUBLISHED:
            self->last_acked_msg_id_.store(event->msg_id);
            if (event->msg_id == self->pending_publish_msg_id_.load()) {
                xSemaphoreGive(self->publish_ack_semaphore_);
            }
            break;
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
