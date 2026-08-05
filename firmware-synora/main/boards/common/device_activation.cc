#include "device_activation.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "settings.h"
#include "synora_mqtt.h"

static const char* TAG = "DeviceActivation";
static constexpr const char* kSettingsNamespace = "synora_dev";

namespace {

std::string GetMacAddressString() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

// Accumulates the response body into *user_data (a std::string*).
// esp_http_client_perform() already de-chunks internally before firing
// HTTP_EVENT_ON_DATA -- evt->data/data_len is always the logical body,
// regardless of wire-level Transfer-Encoding -- so there's no reason to skip
// chunked responses here. The server_url this talks to (Cloudflare-fronted)
// serves chunked responses to HTTP/1.1 clients, and skipping them left
// response_body permanently empty ("Failed to parse activation response: "
// with nothing after the colon) despite the request succeeding with a 200.
esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// POSTs `body` to `url`, returns the HTTP status code (or -1 on a transport
// failure that never reached the server) and fills *response_body.
int HttpPostJson(const std::string& url, const std::string& body, std::string* response_body) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.event_handler = HttpEventHandler;
    config.user_data = response_body;
    // Server-cert verification is mandatory in recent ESP-IDF -- esp-tls
    // hard-fails ("No server verification option set") rather than silently
    // connecting insecurely if neither this nor cacert_buf/skip_cert_common_
    // name_check is set. Uses ESP-IDF's bundled Mozilla CA list
    // (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, already enabled), which covers
    // whatever public CA actually terminates TLS for the configured
    // server_url -- Cloudflare's edge cert when tunneled, same as production.
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), body.size());

    esp_err_t err = esp_http_client_perform(client);
    int status_code = -1;
    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGE(TAG, "HTTP POST %s failed: %s", url.c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return status_code;
}

} // namespace

DeviceActivation& DeviceActivation::GetInstance() {
    static DeviceActivation instance;
    return instance;
}

void DeviceActivation::Load() {
    Settings settings(kSettingsNamespace, false);
    device_id_ = settings.GetString("device_id");
    activation_code_ = settings.GetString("activation_code");
    server_url_ = settings.GetString("server_url");
    is_paired_ = settings.GetBool("is_paired", false);

    if (!device_id_.empty()) {
        ESP_LOGI(TAG, "Loaded existing activation: deviceId=%s", device_id_.c_str());
    }
}

bool DeviceActivation::TryActivate(const std::string& server_url) {
    if (!device_id_.empty()) {
        return true; // Already activated (this session or a previous boot via Load()).
    }
    if (server_url.empty()) {
        ESP_LOGW(TAG, "No server_url set (see the BLE `server_url` characteristic) -- cannot activate");
        return false;
    }

    std::string url = server_url + "/devices/activation";
    std::string request_body = R"({"macAddress":")" + GetMacAddressString() + R"("})";
    std::string response_body;
    ESP_LOGI(TAG, "Activation POST starting: %s", url.c_str());
    int status_code = HttpPostJson(url, request_body, &response_body);
    ESP_LOGI(TAG, "Activation POST returned: status=%d", status_code);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Activation request failed, status=%d", status_code);
        return false;
    }

    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse activation response: %s", response_body.c_str());
        return false;
    }

    cJSON* device_id = cJSON_GetObjectItem(root, "deviceId");
    cJSON* code = cJSON_GetObjectItem(root, "activationCode");
    bool ok = cJSON_IsString(device_id) && cJSON_IsString(code);
    if (ok) {
        device_id_ = device_id->valuestring;
        activation_code_ = code->valuestring;
        server_url_ = server_url;

        Settings settings(kSettingsNamespace, true);
        settings.SetString("device_id", device_id_);
        settings.SetString("activation_code", activation_code_);
        settings.SetString("server_url", server_url_);
        ESP_LOGI(TAG, "Activated: deviceId=%s code=%s", device_id_.c_str(), activation_code_.c_str());
    } else {
        ESP_LOGE(TAG, "Activation response missing deviceId/activationCode: %s", response_body.c_str());
    }

    cJSON_Delete(root);
    return ok;
}

// Publishes over MQTT (synora/devices/{device_id_}/unlink), not an HTTP POST
// -- per the 2026-08-01 MQTT migration, the server subscribes to this topic
// instead of exposing POST /devices/{id}/unlink. Activation itself
// (TryActivate() above) stays REST: it's how the device gets a device id and
// MQTT topics are scoped by that id, so there's nothing to publish under
// before activation succeeds.
void DeviceActivation::Unlink() {
    if (device_id_.empty()) {
        return;
    }
    // This is the actual "become unpaired" action -- Reset() (below) no
    // longer touches is_paired_ itself, since 2 of its 4 call sites
    // (ConnectAndActivate()/HandleReactivate()) don't reboot and were never
    // meant to unpair. Both of Unlink()'s own callers (SynoraBoard's
    // BOOT-button hold, BleProvisioning's control:factory_reset) do reboot
    // right after, so clearing it here is safe/final rather than leaving a
    // window where local state says unpaired but nothing forced a fresh
    // retained-state redelivery to confirm it either way.
    SetPaired(false);
    SynoraMqtt::GetInstance().EnsureStarted();
    SynoraMqtt::GetInstance().Publish("unlink", "{}");
    // Unlike the old HTTP POST (blocking until the response arrived), a QoS
    // 1 MQTT publish only enqueues the message; give the client task a
    // moment to actually hand it to the TCP stack before the reboot cuts the
    // connection.
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Unlink published over MQTT");
}

void DeviceActivation::Reset() {
    device_id_.clear();
    activation_code_.clear();
    server_url_.clear();

    // Deliberately leaves is_paired_/the persisted "is_paired" key alone --
    // this is called from ConnectAndActivate()/HandleReactivate() too
    // (neither reboots afterward), not just HandleFactoryReset() (which
    // does). Clearing it here used to locally "unpair" an already-paired
    // device the moment either of those non-rebooting callers ran (e.g. a
    // stray/duplicate BLE commit arriving after pairing already succeeded),
    // with no reboot or fresh MQTT subscribe to re-deliver the retained
    // isPaired:true and correct it -- the TFT would keep showing the pairing
    // screen indefinitely despite the server still correctly considering the
    // device paired. Actual unpair is Unlink()'s job, and its only two
    // callers both esp_restart() right after.
    Settings settings(kSettingsNamespace, true);
    settings.EraseKey("device_id");
    settings.EraseKey("activation_code");
    settings.EraseKey("server_url");
}

void DeviceActivation::SetPaired(bool paired) {
    is_paired_ = paired;
    Settings settings(kSettingsNamespace, true);
    settings.SetBool("is_paired", paired);
}
