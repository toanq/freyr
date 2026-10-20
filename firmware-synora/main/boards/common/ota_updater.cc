#include "ota_updater.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_system.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "OtaUpdater";

std::atomic<bool> OtaUpdater::in_progress_{false};

bool OtaUpdater::TryClaim() {
    bool expected = false;
    return in_progress_.compare_exchange_strong(expected, true);
}

void OtaUpdater::Release() {
    in_progress_.store(false);
}

void OtaUpdater::ApplyUpdate(const std::string& url) {
    if (!TryClaim()) {
        ESP_LOGW(TAG, "OTA already in progress (WiFi or BLE) -- ignoring update request");
        return;
    }
    auto* heap_url = new std::string(url);
    if (xTaskCreate(UpdateTask, "ota_update", 8192, heap_url, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        delete heap_url;
        Release();
    }
}

void OtaUpdater::UpdateTask(void* arg) {
    auto* heap_url = static_cast<std::string*>(arg);
    std::string url = *heap_url;
    delete heap_url;

    ESP_LOGI(TAG, "Starting OTA download: %s", url.c_str());

    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.timeout_ms = 30000;
    // Same cert-bundle pattern as device_activation.cc's HttpPostJson --
    // esp-tls hard-fails without an explicit server-verification option.
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting into new image");
        esp_restart();
    } else {
        // esp_https_ota() aborts internally on failure (invalid image, write
        // error, connection drop) without touching the boot partition, so the
        // device just keeps running the current image -- no rollback needed.
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        Release();
    }

    vTaskDelete(nullptr);
}
