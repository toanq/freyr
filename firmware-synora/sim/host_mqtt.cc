#include "host_mqtt.h"

#include "../main/boards/common/mqtt_topics.h"

#include <cstdio>
#include <cstring>

HostMqtt::HostMqtt(std::string host, int port, std::string username, std::string password, std::string device_id)
    : host_(std::move(host)),
      port_(port),
      username_(std::move(username)),
      password_(std::move(password)),
      device_id_(std::move(device_id)) {
    mosquitto_lib_init();
    mosq_ = mosquitto_new(nullptr, true, this);
    mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());
    mosquitto_connect_callback_set(mosq_, OnConnect);
    mosquitto_disconnect_callback_set(mosq_, OnDisconnect);
    mosquitto_message_callback_set(mosq_, OnMessage);
}

HostMqtt::~HostMqtt() {
    Stop();
    mosquitto_destroy(mosq_);
    mosquitto_lib_cleanup();
}

bool HostMqtt::Connect() {
    int rc = mosquitto_connect(mosq_, host_.c_str(), port_, /*keepalive=*/30);
    return rc == MOSQ_ERR_SUCCESS;
}

void HostMqtt::StartLoop() {
    mosquitto_loop_start(mosq_);
}

void HostMqtt::Stop() {
    mosquitto_loop_stop(mosq_, true);
    mosquitto_disconnect(mosq_);
}

void HostMqtt::Publish(const std::string& topic_suffix, const std::string& payload, int qos, bool retain) {
    auto topic = DeviceTopic(device_id_, topic_suffix);
    mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(), qos, retain);
}

void HostMqtt::Subscribe(const std::string& topic_suffix, std::function<void(const std::string&)> callback) {
    auto topic = DeviceTopic(device_id_, topic_suffix);
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        subscriptions_[topic] = std::move(callback);
    }
    mosquitto_subscribe(mosq_, nullptr, topic.c_str(), 1);
}

void HostMqtt::OnConnect(struct mosquitto*, void* user_data, int rc) {
    auto* self = static_cast<HostMqtt*>(user_data);
    self->connected_ = (rc == 0);
    // Re-subscribe on every (re)connect, matching SynoraMqtt's own pattern --
    // the broker doesn't remember a non-persistent session's subscriptions.
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(self->subscriptions_mutex_);
        for (const auto& [topic, callback] : self->subscriptions_) {
            mosquitto_subscribe(self->mosq_, nullptr, topic.c_str(), 1);
        }
    }
}

void HostMqtt::OnDisconnect(struct mosquitto*, void* user_data, int) {
    static_cast<HostMqtt*>(user_data)->connected_ = false;
}

void HostMqtt::OnMessage(struct mosquitto*, void* user_data, const struct mosquitto_message* msg) {
    auto* self = static_cast<HostMqtt*>(user_data);
    std::string topic(msg->topic);
    std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);

    std::function<void(const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(self->subscriptions_mutex_);
        auto it = self->subscriptions_.find(topic);
        if (it != self->subscriptions_.end()) {
            callback = it->second;
        }
    }
    if (callback) {
        callback(payload);
    }
}
