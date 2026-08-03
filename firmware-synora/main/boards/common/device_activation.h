#pragma once

#include <string>

// Handles the Synora server's device activation call (POST
// {server_url}/devices/activation). Ported from firmware/main/boards/common/
// device_activation.{h,cc} -- API unchanged, only the HTTP transport differs
// (esp_http_client directly here, since the greenfield tree has no Board/
// Network abstraction yet).
class DeviceActivation {
public:
    static DeviceActivation& GetInstance();

    // Loads a previously-activated device id/server URL from NVS, if any. Call
    // once at boot, before TryActivate().
    void Load();

    // No-op (returns true immediately) if already activated, whether from this
    // call or a previous boot via Load(). Otherwise POSTs to
    // `{server_url}/devices/activation` and persists the result to NVS so it
    // survives reboots and isn't re-requested every time.
    bool TryActivate(const std::string& server_url);

    // Best-effort POST to `{server_url}/devices/{device_id}/unlink`, clearing
    // the device's owner on the server so it becomes claimable again. No-op
    // if never activated (no device_id_ yet).
    void Unlink();

    // Clears device_id_/activation_code_/server_url_ (in-memory and NVS) so
    // the next TryActivate() call requests a genuinely fresh device row
    // instead of returning the old cached one -- necessary because the old
    // row can become orphaned (e.g. deleted server-side) with no way for the
    // device to detect that on its own. Called by the "hold to unpair"
    // button gesture right after Unlink(), since a stale local id is useless
    // once the server side is unlinked/gone anyway.
    void Reset();

    // Server-confirmed pairing state, persisted to NVS so it survives a
    // reboot -- lets SynoraBoard's constructor skip bringing BLE up at all
    // for a returning already-paired device, instead of Init()ing it every
    // boot only to Deinit() it again moments later once SynoraMqtt hears the
    // retained state confirm it. Set by SynoraMqtt::ApplyIsPaired(), read at
    // boot right after Load(). Implicitly cleared back to false by Reset()'s
    // EraseAll() (same NVS namespace), so an unpair always leaves the next
    // boot free to bring BLE up again.
    bool IsPaired() const { return is_paired_; }
    void SetPaired(bool paired);

    const std::string& GetDeviceId() const { return device_id_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    const std::string& GetServerUrl() const { return server_url_; }

    DeviceActivation(const DeviceActivation&) = delete;
    DeviceActivation& operator=(const DeviceActivation&) = delete;

private:
    DeviceActivation() = default;
    ~DeviceActivation() = default;

    std::string device_id_;
    std::string activation_code_;
    std::string server_url_;
    bool is_paired_ = false;
};
