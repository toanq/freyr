#include "ble_provisioning.h"

#include <cstring>
#include <functional>
#include <vector>

#include <cJSON.h>
#include <esp_bt.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "device_activation.h"
#include "ssid_manager.h"
#include "synora_mqtt.h"
#include "wifi_manager.h"

static const char* TAG = "BleProvisioning";

// Custom 128-bit UUIDs (DESIGN.md section 2's BLE GATT service table) --
// unchanged from the reference firmware so the already-built Flutter app
// (ble_pairing_screen.dart) works against this without any app-side change.
static const ble_uuid128_t kServiceUuid =
    BLE_UUID128_INIT(0xe7, 0xe4, 0x21, 0x77, 0x36, 0x08, 0x53, 0x82, 0x67, 0x4d, 0x1c, 0xa5, 0xa0, 0x0b, 0x27, 0xd2);
static const ble_uuid128_t kWifiSsidUuid =
    BLE_UUID128_INIT(0xc6, 0x55, 0x76, 0xa5, 0x1e, 0xaa, 0x81, 0xa9, 0x3a, 0x44, 0x77, 0x84, 0xf8, 0x2a, 0xcc, 0xf0);
static const ble_uuid128_t kWifiPasswordUuid =
    BLE_UUID128_INIT(0x20, 0x26, 0x9d, 0x85, 0xbb, 0x30, 0xa4, 0x96, 0xcc, 0x4c, 0x10, 0x3b, 0xdd, 0x94, 0x8c, 0x51);
static const ble_uuid128_t kWifiScanUuid =
    BLE_UUID128_INIT(0x76, 0x44, 0x8f, 0x97, 0xc7, 0xc5, 0x3c, 0x98, 0xc0, 0x4a, 0x5d, 0x0b, 0x6f, 0x3f, 0x23, 0xb5);
static const ble_uuid128_t kServerUrlUuid =
    BLE_UUID128_INIT(0x1a, 0xbf, 0x5d, 0x53, 0x8e, 0x6d, 0x88, 0x9c, 0x91, 0x49, 0xfd, 0xe1, 0x99, 0xdc, 0x13, 0x14);
static const ble_uuid128_t kDeviceInfoUuid =
    BLE_UUID128_INIT(0x09, 0xb0, 0x27, 0x09, 0x44, 0x96, 0x4e, 0x93, 0x6b, 0x45, 0xe1, 0x2b, 0x8a, 0xf4, 0x17, 0xca);
static const ble_uuid128_t kSettingsUuid =
    BLE_UUID128_INIT(0xd3, 0xa3, 0x49, 0x31, 0x0c, 0xbc, 0x3c, 0x93, 0x2a, 0x40, 0x07, 0x1c, 0x90, 0xde, 0xb5, 0xee);
static const ble_uuid128_t kActivationUuid =
    BLE_UUID128_INIT(0x72, 0x75, 0xf1, 0x16, 0xff, 0xa4, 0x49, 0xa9, 0x64, 0x40, 0x95, 0xc4, 0x52, 0x90, 0xaf, 0xb2);
static const ble_uuid128_t kControlUuid =
    BLE_UUID128_INIT(0xaa, 0x45, 0x0e, 0x43, 0x1b, 0xb1, 0xfb, 0x97, 0x88, 0x42, 0x9d, 0xfd, 0xcf, 0x8b, 0x12, 0x0d);
static const ble_uuid128_t kStatusUuid =
    BLE_UUID128_INIT(0xca, 0x3c, 0x76, 0x63, 0x98, 0xfc, 0x11, 0xa5, 0x90, 0x49, 0x14, 0xa0, 0x05, 0x3f, 0x3d, 0x48);

// Advertise for a limited window, then restart (see GapEventHandler's
// BLE_GAP_EVENT_ADV_COMPLETE case) -- kept up indefinitely while still
// inited_, this window is just how NimBLE's ble_gap_adv_start() itself is
// called. Deinit() (see SynoraMqtt::ApplyIsPaired) stops this for good once
// the server confirms pairing.
static constexpr uint32_t kAdvertiseWindowMs = 5 * 60 * 1000;

namespace {

// Runs `fn` on a fresh one-shot FreeRTOS task, then tears the task down.
// Stands in for the reference firmware's Application::GetInstance().Schedule()
// (no app/task-queue framework exists yet in this greenfield tree) -- GATT
// access callbacks run on NimBLE's own host task, and several of these
// handlers block for seconds (Wi-Fi connect, HTTP activation, a full scan),
// which would otherwise stall the BLE host.
void ScheduleOnTask(std::function<void()> fn) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    xTaskCreate(
        [](void* arg) {
            auto* heap_fn = static_cast<std::function<void()>*>(arg);
            (*heap_fn)();
            delete heap_fn;
            vTaskDelete(nullptr);
        },
        "ble_prov_task", 4096, heap_fn, 5, nullptr);
}

// Copies an mbuf chain into a std::string. Shared by every write-type access callback.
std::string ReadOmToString(os_mbuf* om) {
    std::string value(OS_MBUF_PKTLEN(om), '\0');
    ble_hs_mbuf_to_flat(om, value.data(), value.size(), nullptr);
    return value;
}

int WriteStringReply(ble_gatt_access_ctxt* ctxt, const std::string& value) {
    int rc = os_mbuf_append(ctxt->om, value.data(), value.size());
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

} // namespace

BleProvisioning& BleProvisioning::GetInstance() {
    static BleProvisioning instance;
    return instance;
}

esp_err_t BleProvisioning::Init() {
    if (inited_) {
        return ESP_OK;
    }

    // Bring the Wi-Fi radio up in station mode now (idempotent) so
    // esp_wifi_scan_start() in ScanWifiNetworksJson()'s callers has a started
    // driver to work with -- the phone reads wifi_scan right after BLE
    // connect, well before it submits credentials via commit.
    WifiManager::GetInstance().StartStation();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_nimble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = OnHostReset;
    ble_hs_cfg.sync_cb = OnHostSync;
    ble_hs_cfg.gatts_register_cb = GattSvrRegisterCb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ret = RegisterGattService();
    if (ret != ESP_OK) {
        return ret;
    }

    // esp_nimble_enable() takes a plain `void*` in its public signature (a C-ism
    // that rejects the implicit function-pointer-to-void* conversion in C++);
    // nimble_port_freertos_init() is the properly-typed wrapper IDF's own NimBLE
    // examples call instead.
    nimble_port_freertos_init(HostTask);

    state_ = ProvisioningState::kIdle;
    inited_ = true;
    return ESP_OK;
}

esp_err_t BleProvisioning::Deinit() {
    if (!inited_) {
        return ESP_OK;
    }

    ble_gap_adv_stop();
    esp_err_t ret = nimble_port_stop();
    if (ret == ESP_OK) {
        esp_nimble_deinit();
    }
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    inited_ = false;
    return ESP_OK;
}

void BleProvisioning::OnHostReset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

void BleProvisioning::OnHostSync() {
    // ble_hs_id_infer_auto's 2nd arg is an OUT-param NimBLE writes through, not
    // optional.
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    GetInstance().StartAdvertising();
}

void BleProvisioning::HostTask(void* param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t BleProvisioning::StartAdvertising() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char device_name[32];
    snprintf(device_name, sizeof(device_name), "Synora-%02X%02X", mac[4], mac[5]);
    ble_svc_gap_device_name_set(device_name);

    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(device_name);
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, kAdvertiseWindowMs, &adv_params, GapEventHandler, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising as \"%s\"", device_name);
    return ESP_OK;
}

int BleProvisioning::GapEventHandler(ble_gap_event* event, void* arg) {
    auto& self = GetInstance();

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self.conn_handle_ = event->connect.conn_handle;
                self.SetState(ProvisioningState::kJoining);
                ESP_LOGI(TAG, "BLE central connected, conn_handle=%d", self.conn_handle_);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE central disconnected, reason=%d", event->disconnect.reason);
            self.conn_handle_ = 0;
            if (self.inited_) {
                self.StartAdvertising();
            }
            if (self.intentional_disconnect_) {
                // ConnectAndActivate() dropped this connection on purpose to
                // free the radio for its own in-progress Wi-Fi connect
                // attempt -- let it run to completion instead of reverting
                // to the pre-rescan network here, which would race with it.
                self.intentional_disconnect_ = false;
            } else {
                ScheduleOnTask([]() { GetInstance().RevertToPreRescanNetworkIfNeeded(); });
            }
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "Subscribe event, attr_handle=%d", event->subscribe.attr_handle);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            // ble_gap_adv_start()'s duration (kAdvertiseWindowMs) elapsed with no
            // connection -- restart so the device keeps advertising indefinitely.
            ESP_LOGI(TAG, "Advertising window elapsed, restarting");
            if (self.inited_) {
                self.StartAdvertising();
            }
            break;
        default:
            break;
    }

    return 0;
}

esp_err_t BleProvisioning::RegisterGattService() {
    static const ble_gatt_chr_def wifi_characteristics[] = {
        {
            .uuid = &kWifiSsidUuid.u,
            .access_cb = WifiSsidAccessCb,
            .flags = BLE_GATT_CHR_F_WRITE,
        },
        {
            .uuid = &kWifiPasswordUuid.u,
            .access_cb = WifiPasswordAccessCb,
            .flags = BLE_GATT_CHR_F_WRITE,
        },
        {
            .uuid = &kWifiScanUuid.u,
            .access_cb = WifiScanAccessCb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        },
        {
            .uuid = &kServerUrlUuid.u,
            .access_cb = ServerUrlAccessCb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        },
        {
            .uuid = &kDeviceInfoUuid.u,
            .access_cb = DeviceInfoAccessCb,
            .flags = BLE_GATT_CHR_F_READ,
        },
        {
            .uuid = &kSettingsUuid.u,
            .access_cb = SettingsAccessCb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        },
        {
            .uuid = &kActivationUuid.u,
            .access_cb = ActivationAccessCb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        },
        {
            .uuid = &kControlUuid.u,
            .access_cb = ControlAccessCb,
            .flags = BLE_GATT_CHR_F_WRITE,
        },
        {
            .uuid = &kStatusUuid.u,
            .access_cb = StatusAccessCb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        },
        {0}, // terminator
    };

    static const ble_gatt_svc_def gatt_services[] = {
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &kServiceUuid.u,
            .characteristics = wifi_characteristics,
        },
        {0}, // terminator
    };

    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void BleProvisioning::GattSvrRegisterCb(ble_gatt_register_ctxt* ctxt, void* arg) {
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR) {
        return;
    }

    auto& self = GetInstance();
    const ble_uuid_t* uuid = ctxt->chr.chr_def->uuid;

    if (ble_uuid_cmp(uuid, &kStatusUuid.u) == 0) {
        self.status_val_handle_ = ctxt->chr.val_handle;
    } else if (ble_uuid_cmp(uuid, &kActivationUuid.u) == 0) {
        self.activation_val_handle_ = ctxt->chr.val_handle;
    } else if (ble_uuid_cmp(uuid, &kWifiScanUuid.u) == 0) {
        self.wifi_scan_val_handle_ = ctxt->chr.val_handle;
    }
}

int BleProvisioning::WifiSsidAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    GetInstance().pending_ssid_ = ReadOmToString(ctxt->om);
    return 0;
}

int BleProvisioning::WifiPasswordAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    GetInstance().pending_password_ = ReadOmToString(ctxt->om);
    return 0;
}

int BleProvisioning::WifiScanAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return WriteStringReply(ctxt, GetInstance().ScanWifiNetworksJson());
}

int BleProvisioning::ServerUrlAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    auto& self = GetInstance();
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        self.server_url_ = ReadOmToString(ctxt->om);
        return 0;
    }
    return WriteStringReply(ctxt, self.server_url_);
}

int BleProvisioning::DeviceInfoAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[13];
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t* app_desc = esp_app_get_description();

    // isPaired is hardcoded false for now -- the reference firmware sources
    // this from TelemetryReporter's cached last-heartbeat response, but
    // telemetry hasn't landed in this greenfield tree yet (Phase 3). Revisit
    // once it does: the app just falls back to a full Wi-Fi form either way,
    // so this is a UX-only simplification, not a correctness bug.
    char json[224];
    snprintf(json, sizeof(json), R"({"deviceId":"%s","firmwareVersion":"%s","model":"synora","isPaired":false})",
              device_id, app_desc->version);
    return WriteStringReply(ctxt, json);
}

int BleProvisioning::SettingsAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    // TODO: back this with the Settings class instead of an in-memory
    // placeholder, so volume/brightness/language/wake-sensitivity survive a
    // reboot -- deferred to the display/UI feature that actually consumes
    // these.
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ReadOmToString(ctxt->om);
        return 0;
    }
    return WriteStringReply(ctxt, R"({"volume":80,"brightness":100,"language":"vi-VN","wakeSensitivity":2})");
}

int BleProvisioning::ActivationAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    // Populated by ConnectAndActivate() once DeviceActivation::TryActivate
    // succeeds -- empty until then.
    return WriteStringReply(ctxt, GetInstance().activation_code_);
}

int BleProvisioning::ControlAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    auto command = ReadOmToString(ctxt->om);
    auto& self = GetInstance();

    if (command == "commit") {
        self.HandleCommit();
    } else if (command == "reboot") {
        self.HandleReboot();
    } else if (command == "factory_reset") {
        self.HandleFactoryReset();
    } else if (command == "test_wifi") {
        self.HandleTestWifi();
    } else if (command == "rescan") {
        self.HandleRescan();
    } else if (command == "more_aps") {
        self.HandleMoreAps();
    } else if (command == "reactivate") {
        self.HandleReactivate();
    } else {
        ESP_LOGW(TAG, "Unknown control command: %s", command.c_str());
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

int BleProvisioning::StatusAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    const char* status_str = "idle";
    switch (GetInstance().state_) {
        case ProvisioningState::kIdle: status_str = "idle"; break;
        case ProvisioningState::kJoining: status_str = "joining"; break;
        case ProvisioningState::kOnline: status_str = "online"; break;
        case ProvisioningState::kActivated: status_str = "activated"; break;
        case ProvisioningState::kError: status_str = "error"; break;
    }
    return WriteStringReply(ctxt, status_str);
}

void BleProvisioning::HandleCommit() {
    if (pending_ssid_.empty()) {
        ESP_LOGW(TAG, "commit received with no Wi-Fi SSID set (write the wifi_ssid characteristic first)");
        SetState(ProvisioningState::kError);
        NotifyStatus();
        return;
    }

    SsidManager::GetInstance().AddSsid(pending_ssid_, pending_password_);
    SetState(ProvisioningState::kJoining);
    NotifyStatus();

    ESP_LOGI(TAG, "Wi-Fi credentials committed for \"%s\" -- connecting", pending_ssid_.c_str());

    // Deferred off this GATT callback's task context -- connecting/activating
    // can block for tens of seconds.
    ScheduleOnTask([]() { GetInstance().ConnectAndActivate(); });
}

void BleProvisioning::ConnectAndActivate() {
    // Drop the active BLE connection (if any) for the duration of the actual
    // Wi-Fi connect attempt below -- an active BLE central connection during
    // this exact window is enough Wi-Fi/BLE coexistence contention to make
    // the connection itself fail outright. Advertising resumes automatically
    // once this disconnect completes (see GapEventHandler's
    // BLE_GAP_EVENT_DISCONNECT case).
    if (conn_handle_ != 0) {
        ESP_LOGI(TAG, "Disconnecting BLE central to free the radio for the Wi-Fi connect attempt");
        intentional_disconnect_ = true;
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // The station has been running (scanning) since Init(), so this is
    // normally a no-op.
    WifiManager::GetInstance().StartStation();

    // Explicit connect (WifiManager::ConnectToKnownNetwork -- part of this
    // component's local patch, see wifi_manager.h's doc comment, added
    // specifically for this call site) instead of forcing a scan and hoping
    // the station's own ambient scan-driven auto-reconnect state machine
    // picks up the credential HandleCommit() just added to SsidManager: that
    // ad-hoc path depends on scan-result matching and whatever
    // connect_queue_/backoff state the background reconnect logic happens to
    // be in at this exact moment, which was the likely source of "sometimes
    // just doesn't connect during pairing" even with correct credentials.
    // ConnectToKnownNetwork() resets that state and connects directly.
    WifiManager::GetInstance().ConnectToKnownNetwork(pending_ssid_, pending_password_);

    constexpr int kConnectTimeoutSec = 30;
    for (int waited = 0; waited < kConnectTimeoutSec && !WifiManager::GetInstance().IsConnected(); ++waited) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGW(TAG, "Wi-Fi connect timed out during BLE provisioning");
        SetState(ProvisioningState::kError);
        NotifyStatus();
        RevertToPreRescanNetworkIfNeeded();
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi connected, activating against server_url=\"%s\"", server_url_.c_str());
    SetState(ProvisioningState::kOnline);
    NotifyStatus();

    // Reset first -- every "commit" here is an explicit, user-initiated
    // (re-)pairing action via the BLE screen, so it should always ask the
    // server for the current code for this MAC rather than silently
    // replaying a code from a long-gone activation. /devices/activation's own
    // MAC-based dedup (server-side) safely reuses the existing DB row and
    // preserves ownership if it's already paired, so this is safe even for an
    // already-paired device going through this flow again.
    DeviceActivation::GetInstance().Reset();
    if (DeviceActivation::GetInstance().TryActivate(server_url_)) {
        activation_code_ = DeviceActivation::GetInstance().GetActivationCode();
        SynoraMqtt::GetInstance().EnsureStarted();
        // TODO: WateringScheduler::SetDeviceId/SetServerUrl once Phase 3
        // (pump) lands -- doesn't exist in this greenfield tree yet.
        SetState(ProvisioningState::kActivated);
        pre_rescan_ssid_.clear(); // Successfully on a working network -- no rollback needed.
    } else {
        SetState(ProvisioningState::kError);
        RevertToPreRescanNetworkIfNeeded();
    }
    NotifyStatus();
    NotifyActivation();

    // Deliberately does not Deinit() here -- reaching kActivated only means
    // the phone now has an activation code and needs to read it (still over
    // this BLE connection) before calling POST /devices/pair with it.
    // Tearing BLE down at this point would drop that read mid-flight. BLE
    // turns off once SynoraMqtt::ApplyIsPaired sees the server-confirmed
    // isPaired:true instead, which can only happen after that call succeeds.
}

void BleProvisioning::RevertToPreRescanNetworkIfNeeded() {
    if (pre_rescan_ssid_.empty()) {
        return;
    }
    std::string ssid = std::move(pre_rescan_ssid_);
    pre_rescan_ssid_.clear();

    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == ssid) {
            ESP_LOGI(TAG, "Reverting to previous network \"%s\"", ssid.c_str());
            WifiManager::GetInstance().ConnectToKnownNetwork(item.ssid, item.password);
            return;
        }
    }
    ESP_LOGW(TAG, "Previous network \"%s\" no longer known, cannot revert", ssid.c_str());
}

void BleProvisioning::HandleReboot() {
    ScheduleOnTask([]() { esp_restart(); });
}

// Same "unpair" action as SynoraBoard's GPIO0 10s-hold gesture (see
// synora_board.cc) -- both just call these two DeviceActivation primitives
// directly rather than one calling into the other, to avoid a dependency
// cycle between the board and BLE provisioning layers.
void BleProvisioning::HandleFactoryReset() {
    ESP_LOGW(TAG, "factory_reset requested over BLE -- unpairing");
    ScheduleOnTask([]() {
        DeviceActivation::GetInstance().Unlink();
        DeviceActivation::GetInstance().Reset();
        esp_restart();
    });
}

void BleProvisioning::HandleTestWifi() {
    // TODO: attempt a connection with pending_ssid_/pending_password_ without
    // committing to SsidManager, and reflect success/failure via NotifyStatus().
    ESP_LOGI(TAG, "test_wifi requested for \"%s\" (not yet implemented)", pending_ssid_.c_str());
}

void BleProvisioning::HandleRescan() {
    // Deferred off this GATT callback's task context (matching HandleCommit's
    // pattern) since this blocks for the scan's full duration.
    ScheduleOnTask([]() {
        // 30s, not 15s -- a real scan can take ~16.5s in a busy RF
        // environment (full channel sweep), which would otherwise exceed
        // both this and the app's matching wait.
        auto& self = GetInstance();
        self.scan_cursor_ = 0; // Fresh scan restarts pagination from the top.

        // Scanning while still associated to an AP only gets a severely
        // degraded result back from the driver -- disconnect first so this
        // scan sees the whole picture. RevertToPreRescanNetworkIfNeeded()
        // falls back to this network if the user's subsequent commit doesn't
        // end up on a working one, or never happens.
        if (WifiManager::GetInstance().IsConnected()) {
            self.pre_rescan_ssid_ = WifiManager::GetInstance().GetSsid();
            WifiManager::GetInstance().Disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        if (!WifiManager::GetInstance().ScanAndWait(30000)) {
            ESP_LOGW(TAG, "Rescan timed out waiting for SCAN_DONE");
        }
        // A scan landing 0 APs right after boot (or mid-reconnect) is usually
        // just bad timing, not a genuinely empty environment -- give it one
        // short window to settle and try again before serving an empty list.
        if (WifiManager::GetInstance().GetLastScanResults().empty()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (!WifiManager::GetInstance().ScanAndWait(8000)) {
                ESP_LOGW(TAG, "Rescan retry timed out waiting for SCAN_DONE");
            }
        }
        self.BuildNextScanChunk();
        self.NotifyWifiScan();
    });
}

// Fetches the next chunk of an already-completed scan (scan_cursor_ picks up
// where the last BuildNextScanChunk() call left off) -- no new scan, just
// pagination. See HandleRescan() for starting a fresh scan.
void BleProvisioning::HandleMoreAps() {
    ScheduleOnTask([]() {
        auto& self = GetInstance();
        self.BuildNextScanChunk();
        self.NotifyWifiScan();
    });
}

// Lets the app ask the device to mint a fresh activation code on the spot and
// retry pairing with that, if its cached code refers to a device row that no
// longer exists server-side (e.g. deleted via the web admin console).
//
// Reset() clears only DeviceActivation's own cached device_id_/
// activation_code_/server_url_ (a separate NVS namespace from Wi-Fi
// credentials), so this does NOT touch the Wi-Fi connection or force an
// unpair -- BleProvisioning's own server_url_ (from the original commit) is
// passed straight back into TryActivate() below.
void BleProvisioning::HandleReactivate() {
    ScheduleOnTask([]() {
        auto& self = GetInstance();
        DeviceActivation::GetInstance().Reset();
        if (DeviceActivation::GetInstance().TryActivate(self.server_url_)) {
            self.activation_code_ = DeviceActivation::GetInstance().GetActivationCode();
            self.NotifyActivation();
        } else {
            ESP_LOGW(TAG, "Reactivate failed to get a new activation code");
            self.SetState(ProvisioningState::kError);
            self.NotifyStatus();
        }
    });
}

void BleProvisioning::SetState(ProvisioningState state) {
    state_ = state;
}

void BleProvisioning::NotifyStatus() {
    if (conn_handle_ == 0 || status_val_handle_ == 0) {
        return;
    }
    // ble_gatts_notify() re-invokes StatusAccessCb() (in read mode) to build the
    // payload, so there's exactly one place that formats the status string.
    ble_gatts_notify(conn_handle_, status_val_handle_);
}

void BleProvisioning::NotifyActivation() {
    if (conn_handle_ == 0 || activation_val_handle_ == 0 || activation_code_.empty()) {
        return;
    }
    ble_gatts_notify(conn_handle_, activation_val_handle_);
}

void BleProvisioning::NotifyWifiScan() {
    if (conn_handle_ == 0 || wifi_scan_val_handle_ == 0) {
        return;
    }
    ble_gatts_notify(conn_handle_, wifi_scan_val_handle_);
}

std::string BleProvisioning::ScanWifiNetworksJson() {
    return cached_scan_chunk_;
}

void BleProvisioning::BuildNextScanChunk() {
    auto ap_records = WifiManager::GetInstance().GetLastScanResults();
    ESP_LOGI(TAG, "wifi_scan chunk: %d cached AP record(s), cursor=%d", static_cast<int>(ap_records.size()),
             static_cast<int>(scan_cursor_));

    // Bluetooth's own Core Spec caps ANY GATT attribute value at 512 bytes,
    // hard, regardless of MTU or read chunking (Vol 3, Part F, 3.2.9). Rather
    // than dropping overflow (which could silently exclude the one network
    // the user actually wants), this paginates: fills one chunk from
    // scan_cursor_ onward, advances the cursor, and reports whether more
    // remain via "more" -- the phone sends "more_aps" while "more" is true to
    // fetch the complete list. Each entry is a compact [ssid, rssi] array
    // rather than an object, saving key-name overhead.
    constexpr size_t kMaxJsonBytes = 470; // headroom for the {"more":...,"items":[]} wrapper
    cJSON* items = cJSON_CreateArray();
    size_t estimated_bytes = 20; // wrapper overhead estimate

    // Hedge against scan unreliability around this device's own known
    // network: make sure a network we demonstrably already reach (or, mid-
    // rescan, just did) is in the very first chunk regardless of what this
    // scan's raw results found.
    if (scan_cursor_ == 0) {
        std::string candidate_ssid;
        int candidate_rssi = 0;
        if (WifiManager::GetInstance().IsConnected()) {
            candidate_ssid = WifiManager::GetInstance().GetSsid();
            candidate_rssi = WifiManager::GetInstance().GetRssi();
        } else if (!pre_rescan_ssid_.empty()) {
            candidate_ssid = pre_rescan_ssid_; // RSSI unknown while disconnected.
        }

        bool already_present = false;
        for (const auto& ap_record : ap_records) {
            if (candidate_ssid == reinterpret_cast<const char*>(ap_record.ssid)) {
                already_present = true;
                break;
            }
        }
        if (!already_present && !candidate_ssid.empty()) {
            cJSON* item = cJSON_CreateArray();
            cJSON_AddItemToArray(item, cJSON_CreateString(candidate_ssid.c_str()));
            cJSON_AddItemToArray(item, cJSON_CreateNumber(candidate_rssi));
            cJSON_AddItemToArray(items, item);
            estimated_bytes += 8 + candidate_ssid.size();
        }
    }

    size_t i = scan_cursor_;
    for (; i < ap_records.size(); i++) {
        const auto& ap_record = ap_records[i];
        if (ap_record.ssid[0] == '\0') {
            continue; // Skip hidden networks -- nothing for the picker to show.
        }
        const char* ssid = reinterpret_cast<const char*>(ap_record.ssid);
        size_t entry_estimate = 8 + strlen(ssid);
        if (estimated_bytes + entry_estimate > kMaxJsonBytes) {
            break;
        }
        estimated_bytes += entry_estimate;

        cJSON* item = cJSON_CreateArray();
        cJSON_AddItemToArray(item, cJSON_CreateString(ssid));
        cJSON_AddItemToArray(item, cJSON_CreateNumber(ap_record.rssi));
        cJSON_AddItemToArray(items, item);
    }
    bool has_more = i < ap_records.size();
    scan_cursor_ = i;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "more", has_more);
    cJSON_AddItemToObject(root, "items", items);

    char* json_chars = cJSON_PrintUnformatted(root);
    cached_scan_chunk_ = json_chars;
    ESP_LOGI(TAG, "wifi_scan chunk built: %s", json_chars);
    cJSON_free(json_chars);
    cJSON_Delete(root);
}
