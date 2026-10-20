#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <esp_err.h>
#include <esp_ota_ops.h>

#include "sha256.h"

struct ble_gatt_access_ctxt;
struct ble_gatt_register_ctxt;
struct ble_gap_event;

// BLE GATT provisioning service (NimBLE). Advertises as "Synora-XXXX" and
// exposes the characteristics from DESIGN.md section 2 (wifi_ssid,
// wifi_password, wifi_scan, server_url, device_info, settings, activation,
// control, status), handing committed Wi-Fi credentials to SsidManager
// (78/esp-wifi-connect) and driving DeviceActivation for the server-side
// activation flow. Deinit() is called once SynoraMqtt sees the server confirm
// pairing (retained `state` topic, isPaired:true) -- not from this class
// itself, since local activation success alone doesn't mean the phone has
// actually called /devices/pair yet. Init() runs again if pairing is ever
// undone (unlink), so the device stays re-pairable.
class BleProvisioning {
public:
    static BleProvisioning& GetInstance();

    esp_err_t Init();
    esp_err_t Deinit();

    bool IsCentralConnected() const { return conn_handle_ != 0; }

    BleProvisioning(const BleProvisioning&) = delete;
    BleProvisioning& operator=(const BleProvisioning&) = delete;

private:
    BleProvisioning() = default;
    ~BleProvisioning() = default;

    // Mirrors the `status` characteristic's documented state machine.
    enum class ProvisioningState {
        kIdle,
        kJoining,
        kOnline,
        kActivated,
        kError,
    };

    static void OnHostReset(int reason);
    static void OnHostSync();
    static void HostTask(void* param);

    esp_err_t StartAdvertising();
    static int GapEventHandler(ble_gap_event* event, void* arg);

    esp_err_t RegisterGattService();

    // Captures status_val_handle_/activation_val_handle_/wifi_scan_val_handle_ as
    // NimBLE assigns them, since ble_gatts_add_svcs() doesn't hand them back any
    // other way -- this is what makes NotifyStatus()/NotifyActivation() work.
    static void GattSvrRegisterCb(ble_gatt_register_ctxt* ctxt, void* arg);

    static int WifiSsidAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int WifiPasswordAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int WifiScanAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int ServerUrlAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int DeviceInfoAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int SettingsAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int ActivationAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int ControlAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);
    static int StatusAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);

    // Write-only. Raw firmware bytes (not UTF-8-safe), so this does NOT use
    // ReadOmToString like every other write characteristic -- see its .cc
    // definition. Only meaningful between an "ota_start" and the matching
    // "ota_commit"/failure on `control` below.
    static int FirmwareAccessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);

    // `control` characteristic commands.
    void HandleCommit();
    void HandleReboot();
    void HandleFactoryReset();
    void HandleTestWifi();
    void HandleRescan();
    void HandleMoreAps();
    void HandleReactivate();
    void HandleReconnect();

    // BLE firmware push (fallback OTA path for an unprovisioned/no-WiFi
    // device -- see ota_updater.h for the WiFi/MQTT-triggered path instead).
    // `control: "ota_start:<size>:<sha256-hex>"` and `control: "ota_commit"`;
    // the actual bytes go to FirmwareAccessCb above. Scoped to the
    // provisioning window only -- Deinit() already tears this whole service
    // down once paired, no extra gating needed.
    void HandleOtaStart(size_t size, const std::string& sha256_hex);
    void HandleOtaCommit();

    // Drops any in-flight BLE OTA (esp_ota_abort() if `abort` and a handle
    // is open) and releases OtaUpdater's partition claim. No-op while the
    // commit task owns the state (ota_committing_) -- see HandleOtaCommit.
    void ResetOtaState(bool abort);
    // Unconditional version of the above, for the commit task itself.
    void ClearOtaState(bool abort);
    // Runs on a ScheduleOnTask task: esp_ota_end()'s full image verify and
    // esp_ota_set_boot_partition() are too slow/stack-heavy for NimBLE's host task.
    void FinishOtaCommit();

    // Runs off the BLE callback's task context (scheduled from HandleCommit):
    // connects to the newly-committed Wi-Fi, then activates against the
    // server and notifies the phone with the resulting activation code.
    void ConnectAndActivate();

    // Reconnects to whatever network we were on before a rescan-triggered
    // Disconnect(), if the user's subsequent commit didn't end up on a
    // working network (Wi-Fi connect timeout, activation failure) or never
    // happened at all (BLE disconnected mid-flow). No-op if
    // pre_rescan_ssid_ is empty.
    void RevertToPreRescanNetworkIfNeeded();

    void SetState(ProvisioningState state);
    void NotifyStatus();
    void NotifyActivation();
    void NotifyWifiScan();

    // Returns cached_scan_chunk_ verbatim -- the GATT read callback for
    // `wifi_scan`. Does not compute anything itself (see BuildNextScanChunk).
    std::string ScanWifiNetworksJson();

    // Computes the next chunk of the cached last-scan results as JSON
    // starting from scan_cursor_, advances the cursor past it, and stores
    // the result in cached_scan_chunk_ -- called once per explicit
    // "rescan"/"more_aps" control command, never from the read callback
    // itself. A single BLE GATT attribute value is hard-capped at 512 bytes
    // (Bluetooth Core Spec Vol 3, Part F, 3.2.9), so this paginates:
    // {"more":bool,"items":[[ssid,rssi],...]}.
    void BuildNextScanChunk();

    bool inited_ = false;
    uint16_t conn_handle_ = 0;
    uint16_t status_val_handle_ = 0;
    uint16_t wifi_scan_val_handle_ = 0;
    uint16_t activation_val_handle_ = 0;

    // Set right before ConnectAndActivate() deliberately drops the BLE
    // central connection to free the radio for the Wi-Fi connect attempt
    // (Wi-Fi/BLE coexistence contention otherwise fails the connect
    // outright). Tells GapEventHandler's BLE_GAP_EVENT_DISCONNECT case this
    // specific disconnect is expected and not a "user backed out mid-rescan"
    // one, so it skips RevertToPreRescanNetworkIfNeeded().
    bool intentional_disconnect_ = false;

    // Guards against a second `control: commit` (e.g. the app's own "Thử
    // lại" retry, submitted before the phone realizes the prior attempt is
    // still working) starting a second, overlapping ConnectAndActivate()
    // while one is already running. Without this, two concurrent
    // TryActivate() calls each mint their own fresh server-side
    // ActivationCode for the same device row (the server always overwrites
    // it), so whichever one the phone actually read off BLE could easily be
    // the one that's since been superseded -- POST /devices/pair then 404s
    // or pairs the wrong attempt, and the device never reaches a real
    // "paired" state even though Wi-Fi/activation individually succeeded.
    //
    // A deadline (esp_timer_get_time()/1000 at start, 0 = not running), not
    // a plain bool -- HttpPostJson's esp_http_client_config_t.timeout_ms
    // only bounds the socket-level transfer, NOT DNS resolution (a
    // documented recurring pain point on this project -- see the
    // Cloudflare/Google DNS-fallback override in SynoraBoard), so
    // TryActivate() inside this function can in principle hang far longer
    // than its own nominal timeouts suggest. Without a deadline here, that
    // would permanently wedge this flag true and lock out every future
    // pairing attempt until a reboot -- a plain bool has no way to recover.
    std::atomic<int64_t> connect_and_activate_started_ms_ = 0;

    ProvisioningState state_ = ProvisioningState::kIdle;

    // Wi-Fi credentials staged via wifi_ssid/wifi_password writes; only handed to
    // SsidManager on a `control: commit`, so a partial/aborted pairing session
    // never touches stored Wi-Fi config.
    std::string pending_ssid_;
    std::string pending_password_;
    std::string server_url_;
    std::string activation_code_;

    // Index into the (RSSI-sorted) cached scan results of the next entry to
    // serve -- reset to 0 by HandleRescan() (a fresh scan), advanced by
    // BuildNextScanChunk() each time "rescan"/"more_aps" is explicitly
    // handled.
    size_t scan_cursor_ = 0;

    // The SSID we were connected to right before HandleRescan() disconnected
    // to get a clean (non-degraded) full scan -- empty if not currently in
    // that state. Cleared on a successful ConnectAndActivate().
    std::string pre_rescan_ssid_;

    // The chunk computed by the most recent BuildNextScanChunk() call --
    // ScanWifiNetworksJson() (the GATT read callback) just returns this
    // fixed string verbatim, however many times it gets invoked, rather than
    // computing a fresh (and cursor-advanced) one on every call (NimBLE/
    // Android's own multi-part read handling can invoke the read callback
    // twice per logical app-level read).
    std::string cached_scan_chunk_ = R"({"more":false,"items":[]})";

    // BLE firmware-push state (see HandleOtaStart/FirmwareAccessCb/
    // HandleOtaCommit). ota_partition_ is the slot ota_start targeted --
    // kept around until ota_commit so esp_ota_set_boot_partition() doesn't
    // have to re-derive it (and risk picking a different slot if something
    // else touched OTA state in between, which nothing currently does, but
    // re-deriving it would be the kind of implicit coupling this avoids).
    std::atomic<bool> ota_in_progress_{false};
    // True while FinishOtaCommit() runs off-task; host-task paths leave every
    // ota_* member (including ota_error_) alone until it clears.
    std::atomic<bool> ota_committing_{false};
    esp_ota_handle_t ota_handle_ = 0;
    const esp_partition_t* ota_partition_ = nullptr;
    Sha256Context ota_sha256_ctx_;
    size_t ota_expected_size_ = 0;
    std::string ota_expected_sha256_; // lowercase hex, from ota_start
    size_t ota_bytes_written_ = 0;

    // Surfaced via the `status` characteristic (see StatusAccessCb) ahead of
    // the existing idle/joining/online/activated/error states, since an OTA
    // in progress is a more specific and more urgent thing for the app to
    // show than whatever provisioning state happened to precede it.
    // ota_progress_percent_ is only meaningful while ota_in_progress_; a
    // non-empty ota_error_ latches until the next ota_start.
    std::atomic<int> ota_progress_percent_{0};
    std::string ota_error_;
};
