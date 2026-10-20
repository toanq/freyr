namespace Synora.Core;

public class Device
{
    public Guid Id { get; set; }
    public required string Name { get; set; }
    public required string ActivationCode { get; set; }
    public string? MacAddress { get; set; }
    public string? AvatarPath { get; set; }
    public Guid? OwnerUserId { get; set; }
    public User? Owner { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset? PairedAt { get; set; }

    // Updated on every device-trust-model request (telemetry POST,
    // pending-command GET, schedules GET) -- IsOnline in the API DTOs is
    // computed from this at query time, not persisted, so the "online"
    // threshold can change without a migration.
    public DateTimeOffset? LastSeenAt { get; set; }

    // Null/0 disables. When a reading is at or below this level, the app
    // shows an in-app warning (icon + yellow highlight) -- no push sent.
    public int? LowWaterWarningThresholdPercent { get; set; }

    // Null/0 disables. When a reading is at or below this level, a push
    // notification is sent. Must be <= LowWaterWarningThresholdPercent
    // whenever both are set (critical is the more severe, lower level).
    public int? LowWaterCriticalThresholdPercent { get; set; }

    // Null means alert once per drop below the critical threshold (no
    // repeats until it recovers and drops again); otherwise re-notify every
    // N minutes for as long as the reading stays at or below it.
    public int? LowWaterAlertIntervalMinutes { get; set; }

    // Null while not currently in a "below threshold" streak. Set to the
    // most recent alert's timestamp the moment one fires, reset to null once
    // a reading recovers above the threshold.
    public DateTimeOffset? LowWaterLastAlertedAt { get; set; }

    // Persistent setting reflecting current desired state (unlike the old
    // PendingWaterDurationSeconds/PendingPing one-shot polled fields, both
    // removed now that water/ping are published directly as MQTT commands
    // -- see synora/devices/{id}/commands/water and .../commands/ping).
    // Included in the retained synora/devices/{id}/state payload so a device
    // that (re)connects immediately gets the current muted state.
    public bool Muted { get; set; }

    // The 6-digit code from xiaozhi.me's own OTA activation flow (unrelated
    // to this app's own ActivationCode/pairing above -- see the firmware's
    // device_activation.h for why they're two separate systems that just
    // happen to share the word). Updated on every telemetry POST; null/empty
    // once xiaozhi.me no longer issues one (already linked to some account
    // there). Stored so the web UI can show it without reading the device's
    // own tiny screen.
    public string? XiaozhiActivationCode { get; set; }

    // The running firmware's esp_app_desc_t version string, updated on every
    // telemetry POST (see MqttService.HandleTelemetryAsync) -- lets the app
    // show "up to date" vs. the latest FirmwareBuild without asking the
    // device directly.
    public string? FirmwareVersion { get; set; }
}
