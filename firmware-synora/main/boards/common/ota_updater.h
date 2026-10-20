#pragma once

#include <atomic>
#include <string>

// WiFi OTA: downloads a new firmware image over HTTPS and flashes it to the
// other OTA slot via esp_https_ota. Triggered by SynoraBoard::HandleOtaCommand
// (synora/devices/{id}/commands/ota) -- the BLE fallback path for
// unprovisioned devices lives in BleProvisioning instead (a separate, local
// transfer with no HTTPS/MQTT involved).
class OtaUpdater {
public:
    // Spawns a one-shot FreeRTOS task (same 8192-byte stack as
    // ble_provisioning.cc's ScheduleOnTask, proven sufficient there for an
    // equally TLS-handshake-heavy HTTPS call) that downloads `url` via
    // esp_https_ota() and, on success, esp_restart()s into the new slot.
    // Returns immediately; failures are logged only -- there's no ack channel
    // back to the server, same fire-and-forget trust model as commands/water.
    // Refused (logged) while any OTA -- WiFi or BLE -- already holds the claim.
    static void ApplyUpdate(const std::string& url);

    // Process-wide claim on the inactive OTA partition, shared by this WiFi
    // path and BleProvisioning's BLE push so the two can never write the same
    // slot concurrently. TryClaim() returns false if already held.
    static bool TryClaim();
    static void Release();
    static bool IsInProgress() { return in_progress_.load(); }

private:
    static void UpdateTask(void* arg);

    static std::atomic<bool> in_progress_;
};
