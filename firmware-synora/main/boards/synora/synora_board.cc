#include "synora_board.h"

#include "config.h"

#include "boards/common/ble_provisioning.h"
#include "boards/common/device_activation.h"
#include "boards/common/synora_mqtt.h"
#include "mcp_server.h"
#include "synora_display.h"
#include "synora_voice_session.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

#define TAG "SynoraBoard"

SynoraBoard::SynoraBoard()
    : boot_button_(BOOT_BUTTON_GPIO, /*active_high=*/false, FACTORY_RESET_HOLD_MS),
      pump_(PUMP_GPIO),
      find_me_led_(BUILTIN_LED_GPIO) {
    // Must run before BleProvisioning::Init() and SynoraMqtt::EnsureStarted()
    // (calls esp_mqtt_client_start(), which needs the LWIP core task) --
    // neither brings up esp_netif/esp_wifi itself.
    if (!WifiManager::GetInstance().Initialize(WifiManagerConfig{})) {
        ESP_LOGE(TAG, "WifiManager::Initialize failed");
    }
    // StartStation() actually connects using SsidManager's saved network
    // list (WifiManager::Initialize() only brings up the driver, it doesn't
    // start scanning/connecting on its own). Must run here, unconditionally
    // -- BleProvisioning::Init() also calls it (idempotently, for the
    // "just received credentials over BLE, connect right now" case), but
    // Init() is deliberately skipped for an already-paired device (see the
    // IsPaired() branch below), which left a returning/rebooted paired
    // device's WiFi station NEVER started: WifiManager logged "Initialized"
    // and nothing else, every DNS lookup failed because there was no network
    // at all, and it looked like a DNS problem until a full boot log showed
    // no "Connecting to WiFi"/"Got IP" lines ever appearing.
    WifiManager::GetInstance().StartStation();
    InitializeWifiStatusTracking();
    InitializeDnsFallback();

    InitializeFactoryResetButton();
    InitializeVoiceButton();

    DeviceActivation::GetInstance().Load();
    // Skipped entirely for a returning already-paired device (persisted via
    // SynoraMqtt::ApplyIsPaired -> DeviceActivation::SetPaired(), read back
    // here via IsPaired()) -- no need to bring BLE up and immediately Deinit()
    // it again once the retained MQTT state confirms what NVS already knew.
    // Unpaired devices (including right after an unpair, which clears this
    // via Reset()'s EraseAll()) still Init() here and advertise for pairing.
    // SynoraMqtt::ApplyIsPaired() brings BLE back with Init() if the server
    // ever reports unpaired again without a reboot in between.
    if (!DeviceActivation::GetInstance().IsPaired()) {
        ESP_ERROR_CHECK(BleProvisioning::GetInstance().Init());
    }

    // Registered before EnsureStarted() so the very first "state" delivery
    // (retained, redelivered on every fresh subscribe) doesn't race the
    // callback being wired up.
    SynoraMqtt::GetInstance().SetSchedulesUpdateCallback(
        [this](const std::string& schedules_json) { watering_scheduler_.ApplySchedulesFromServer(schedules_json); });
    SynoraMqtt::GetInstance().SetTimeUpdateCallback(
        [this](int64_t unix_time_ms) { watering_scheduler_.ApplyServerTime(unix_time_ms); });

    // No-ops if this is a first boot before any pairing (no device id yet) --
    // BleProvisioning::ConnectAndActivate() calls this again once activation
    // succeeds. A returning, already-activated device starts MQTT right away.
    SynoraMqtt::GetInstance().EnsureStarted();
    InitializeCommandHandlers();

    InitializeSensors();
    InitializeMcpTools();

    // Loads any NVS-persisted schedule from a previous ApplySchedulesFromServer
    // call and starts SNTP -- runs unconditionally (even unpaired/offline;
    // an unpaired device just has an empty schedule list, a no-op) so
    // scheduled watering resumes as soon as the clock re-syncs after a
    // reboot, without waiting on any other board init step.
    watering_scheduler_.Start([this](int seconds) { WaterFor(seconds); });

    // Started unconditionally -- PublishTelemetry() -> SynoraMqtt::Publish()
    // is already a no-op until EnsureStarted() has a device id, so this is
    // harmless before pairing and starts reporting immediately once paired.
    xTaskCreate(TelemetryTask, "synora_telemetry", 4096, this, 3, nullptr);

    // Eyes start idle/closed; SetState(kListening/kTalking) is driven by
    // SynoraVoiceSession (see InitializeVoiceButton() below).
    ESP_ERROR_CHECK(SynoraDisplay::GetInstance().Init(this));
}

void SynoraBoard::TelemetryTask(void* arg) {
    auto* self = static_cast<SynoraBoard*>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_REPORT_INTERVAL_SEC * 1000));
        self->RefreshSensors();
        self->PublishTelemetry();
        self->CheckWifiLossRecovery();
    }
}

void SynoraBoard::PublishTelemetry() {
    SensorReadings r = GetSensorReadings();

    // Fields omitted (rather than sent as null) when a sensor read failed --
    // TelemetryPayload's fields are all nullable double? server-side, so a
    // partial/empty object still deserializes fine and still counts as the
    // heartbeat that updates LastSeenAt (see the doc comment on the header).
    cJSON* root = cJSON_CreateObject();
    if (r.temperature_valid) {
        cJSON_AddNumberToObject(root, "temperatureCelsius", r.temperature_celsius);
    }
    if (r.humidity_valid) {
        cJSON_AddNumberToObject(root, "humidityPercent", r.humidity_percent);
    }
    if (r.soil_moisture_valid) {
        cJSON_AddNumberToObject(root, "soilMoisturePercent", r.soil_moisture_percent);
    }
    if (r.water_level_valid) {
        cJSON_AddNumberToObject(root, "waterLevelPercent", r.water_level_percent);
    }

    char* json = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Publishing telemetry: %s", json);
    SynoraMqtt::GetInstance().Publish("telemetry", json);
    cJSON_free(json);
    cJSON_Delete(root);
}

void SynoraBoard::InitializeSensors() {
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA_GPIO;
    bus_config.scl_io_num = BOARD_I2C_SCL_GPIO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &board_i2c_bus_));

    sht31_ = std::make_unique<Sht31Sensor>(board_i2c_bus_, SHT31_I2C_ADDR);
    soil_moisture_ = std::make_unique<SoilMoistureSensor>(SOIL_MOISTURE_ADC_UNIT, SOIL_MOISTURE_ADC_CHANNEL);
    // Its own dedicated I2C_NUM_1 bus, not board_i2c_bus_ -- see config.h.
    water_level_ = std::make_unique<WaterLevelSensor>(WATER_LEVEL_SDA_GPIO, WATER_LEVEL_SCL_GPIO, WATER_LEVEL_I2C_PORT,
                                                       WATER_LEVEL_EMPTY_DISTANCE_MM, WATER_LEVEL_FULL_DISTANCE_MM);

    // One-shot read at boot, logged, so sensor wiring can be verified before
    // the display's dashboard screen exists to show it (construction above
    // succeeds even if a sensor isn't actually reachable on the bus --
    // I2cDevice::I2cDevice() only registers the address, it doesn't talk to
    // the device -- so this is the first real communication check).
    RefreshSensors();
    SensorReadings r = GetSensorReadings();
    ESP_LOGI(TAG, "Sensor check: temp=%s%.1fC humidity=%s%.1f%% soil=%s%.1f%% water=%s%.1f%%",
             r.temperature_valid ? "" : "(failed) ", r.temperature_celsius,
             r.humidity_valid ? "" : "(failed) ", r.humidity_percent,
             r.soil_moisture_valid ? "" : "(failed) ", r.soil_moisture_percent,
             r.water_level_valid ? "" : "(failed) ", r.water_level_percent);
}

void SynoraBoard::RefreshSensors() {
    SensorReadings readings;
    readings.temperature_valid =
        sht31_->Read(readings.temperature_celsius, readings.humidity_percent);
    readings.humidity_valid = readings.temperature_valid;  // one SHT31 read yields both
    readings.soil_moisture_valid = soil_moisture_->ReadPercent(readings.soil_moisture_percent);
    readings.water_level_valid = water_level_->ReadPercent(readings.water_level_percent);

    std::lock_guard<std::mutex> lock(sensor_readings_mutex_);
    sensor_readings_ = readings;
}

SensorReadings SynoraBoard::GetSensorReadings() const {
    std::lock_guard<std::mutex> lock(sensor_readings_mutex_);
    return sensor_readings_;
}

// Subscribes to the two device-actuator commands the server can push over
// MQTT (see synora_mqtt.cc's EnsureStarted() -- these two used to be
// log-only placeholders there, now superseded by real handlers here since
// pump_/find_me_led_ exist). Safe to call every EnsureStarted()-adjacent
// boot path: Subscribe() itself is idempotent per topic (overwrites the map
// entry) and works whether or not the MQTT client has connected yet.
void SynoraBoard::InitializeCommandHandlers() {
    SynoraMqtt::GetInstance().Subscribe("commands/water", [this](const std::string& payload) {
        HandleWaterCommand(payload);
    });
    SynoraMqtt::GetInstance().Subscribe("commands/ping", [this](const std::string&) {
        TriggerFindMe();
    });
}

// Payload shape mirrors Synora.Api's PublishWaterCommandAsync exactly:
// {"durationSeconds": <int>}.
void SynoraBoard::HandleWaterCommand(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "commands/water: malformed JSON payload");
        return;
    }
    cJSON* duration = cJSON_GetObjectItem(root, "durationSeconds");
    if (cJSON_IsNumber(duration)) {
        WaterFor(duration->valueint);
    } else {
        ESP_LOGW(TAG, "commands/water: missing/invalid durationSeconds");
    }
    cJSON_Delete(root);
}

// Shared by HandleWaterCommand (MQTT commands/water) and the self.do_watering
// MCP tool -- both just need "run the pump for N seconds and mirror it on
// the display", the safety clamp itself lives in PumpDriver::RunFor.
void SynoraBoard::WaterFor(int duration_seconds) {
    int actual_seconds = pump_.RunFor(duration_seconds);
    if (actual_seconds > 0) {
        SynoraDisplay::GetInstance().ShowWatering(actual_seconds * 1000);
    }
}

// Shared by the MQTT commands/ping handler and the self.find_device MCP tool.
void SynoraBoard::TriggerFindMe() {
    find_me_led_.Blink(4);
    SynoraDisplay::GetInstance().ShowPing();
}

// Registers the device's MCP tools (see mcp_server.h) -- discovered by the
// server's McpBridge via tools/list once the device's hello advertises
// features.mcp (see voice_messages.cc's BuildHelloMessage), invoked via
// tools/call routed through SynoraVoiceSession::OnControlDown. Runs
// synchronously on the MQTT client task, same as the existing
// commands/water|ping handlers above.
void SynoraBoard::InitializeMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.get_sensor_readings",
        "Get the plant's current sensor readings: air temperature (Celsius), air humidity (percent), "
        "soil moisture (percent), and water tank level (percent). Use this to answer questions about "
        "how the plant or its environment is doing.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return BuildSensorReadingsJson(GetSensorReadings());
        });

    mcp.AddTool("self.do_watering",
        "Run the water pump to water the plant for the given number of seconds (1-300). "
        "Use this when the user asks to water the plant.",
        PropertyList({
            Property("duration_seconds", kPropertyTypeInteger, 1, 300)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            WaterFor(properties["duration_seconds"].value<int>());
            return true;
        });

    mcp.AddTool("self.find_device",
        "Blink the device's LED and show a ping animation, so the user can visually locate the device.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            TriggerFindMe();
            return true;
        });

    mcp.AddTool("self.get_device_status",
        "Get the device's own status: whether it's paired to an owner account, whether the water pump "
        "is currently running, and its firmware version.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "paired", DeviceActivation::GetInstance().IsPaired());
            cJSON_AddBoolToObject(json, "pumpRunning", pump_.IsRunning());
            cJSON_AddStringToObject(json, "firmwareVersion", esp_app_get_description()->version);
            return json;
        });
}

// The DHCP-assigned DNS server on at least one real network this device has
// been tested on can't resolve synora.carcise.dev for the ESP32 (esp-tls's
// getaddrinfo() consistently returns EAI_FAIL/202) even though the exact
// same hostname resolves fine for other clients (phone) on the same
// network -- confirmed not a network-wide/domain problem. Overriding with
// known-good public resolvers (Cloudflare main, Google backup -- lwIP tries
// DNS_MAIN first, DNS_BACKUP only if that fails, so this is genuinely
// "Cloudflare, falling back to Google") sidesteps whatever that DHCP
// resolver's problem is. Registered once; fires on every got-IP (initial
// connect, reconnect, DHCP lease renewal) so a renewal can't quietly revert
// this back to the broken DHCP-provided server.
void SynoraBoard::InitializeDnsFallback() {
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &SynoraBoard::OnGotIp, nullptr);
}

// Fires on the WiFi event task (see WifiManager::NotifyEvent's call sites in
// wifi_manager.cc) -- just latches the most recent event into wifi_event_,
// read back (from SynoraDisplay's own task) via GetWifiStatus().
void SynoraBoard::InitializeWifiStatusTracking() {
    WifiManager::GetInstance().SetEventCallback([this](WifiEvent event, const std::string&) {
        wifi_event_.store(event);
        if (event == WifiEvent::Connected) {
            wifi_disconnected_since_ms_.store(0);
        } else if (event == WifiEvent::Disconnected && wifi_disconnected_since_ms_.load() == 0) {
            // Leading edge only -- repeated Disconnected events during the
            // driver's own reconnect retries shouldn't keep pushing this
            // forward, or CheckWifiLossRecovery()'s threshold would never
            // trip.
            wifi_disconnected_since_ms_.store(esp_timer_get_time() / 1000);
        }
    });
}

TileStatus SynoraBoard::GetWifiStatus() const {
    switch (wifi_event_.load()) {
        case WifiEvent::Connected:
            return TileStatus::kGood;
        case WifiEvent::Scanning:
        case WifiEvent::Connecting:
            return TileStatus::kWarn;
        default:  // Disconnected, ConfigModeEnter/Exit (this board never uses config-AP mode)
            return TileStatus::kBad;
    }
}

bool SynoraBoard::IsMuted() const {
    return SynoraMqtt::GetInstance().IsMuted();
}

void SynoraBoard::CheckWifiLossRecovery() {
    if (!DeviceActivation::GetInstance().IsPaired()) {
        // Not our concern while unpaired -- the constructor's boot check and
        // SynoraMqtt::ApplyIsPaired() already own BLE's on/off state then.
        // Force-clear so this flag can't get stuck true across an
        // unpair/re-pair cycle: ConnectAndActivate() calls
        // DeviceActivation::Reset() (clearing IsPaired()) mid-flow while
        // submitting new credentials, which would otherwise trip this
        // early-return forever after the first such cycle.
        ble_resumed_for_wifi_loss_.store(false);
        return;
    }

    int64_t since = wifi_disconnected_since_ms_.load();
    if (since == 0) {
        if (ble_resumed_for_wifi_loss_.exchange(false)) {
            ESP_LOGI(TAG, "WiFi reconnected -- turning BLE back off");
            BleProvisioning::GetInstance().Deinit();
        }
        return;
    }

    int64_t disconnected_ms = esp_timer_get_time() / 1000 - since;
    if (disconnected_ms >= static_cast<int64_t>(WIFI_LOST_BLE_FALLBACK_SEC) * 1000 &&
        !ble_resumed_for_wifi_loss_.exchange(true)) {
        ESP_LOGW(TAG, "WiFi lost for %llds while paired -- resuming BLE advertising",
                 static_cast<long long>(disconnected_ms / 1000));
        BleProvisioning::GetInstance().Init();
    }
}

void SynoraBoard::OnGotIp(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    esp_netif_dns_info_t cloudflare_dns = {};
    cloudflare_dns.ip.type = ESP_IPADDR_TYPE_V4;
    cloudflare_dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
    esp_netif_set_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &cloudflare_dns);

    esp_netif_dns_info_t google_dns = {};
    google_dns.ip.type = ESP_IPADDR_TYPE_V4;
    google_dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    esp_netif_set_dns_info(event->esp_netif, ESP_NETIF_DNS_BACKUP, &google_dns);

    ESP_LOGI(TAG, "Overrode DHCP-provided DNS with Cloudflare (main) / Google (backup)");
}

void SynoraBoard::InitializeFactoryResetButton() {
    boot_button_.OnLongPress([this]() { FactoryReset(); });
}

// Same physical button as the 10s-hold unpair gesture -- a short click
// toggles the voice session (SynoraVoiceSession::Init() is called separately
// from main.cc once the audio codec exists; this only registers the
// callback, safe to do before that since a real button press can't happen
// before the rest of app_main finishes).
void SynoraBoard::InitializeVoiceButton() {
    boot_button_.OnClick([]() { SynoraVoiceSession::GetInstance().Toggle(); });
}

// Unpairs the device (tells the server to clear ownership, then forgets the
// local device id/activation code) and reboots -- NOT a blind NVS wipe. This
// is deliberately the same two calls as BleProvisioning::HandleFactoryReset()
// (the BLE `control: factory_reset` command); both call DeviceActivation
// directly rather than one calling into the other, to avoid a dependency
// cycle between the board and BLE provisioning layers.
void SynoraBoard::FactoryReset() {
    ESP_LOGW(TAG, "BOOT button held %d ms -- unpairing", FACTORY_RESET_HOLD_MS);
    DeviceActivation::GetInstance().Unlink();
    DeviceActivation::GetInstance().Reset();
    esp_restart();
}
