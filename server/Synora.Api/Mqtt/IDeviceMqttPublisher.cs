namespace Synora.Api.Mqtt;

/// <summary>
/// Publishing side of the device-trust-model MQTT topics, used by the
/// authenticated REST endpoints in Program.cs that used to just flip a
/// polled field on <c>Device</c> (PendingWaterDurationSeconds/PendingPing)
/// or rely on the device's next poll to see updated muted/schedule state.
/// </summary>
public interface IDeviceMqttPublisher
{
    /// <summary>Whether the server's own MQTT client is currently connected to the broker.</summary>
    bool IsConnected { get; }

    /// <summary>
    /// Re-publish the retained <c>synora/devices/{id}/state</c> message
    /// (muted/isPaired/schedules) for a device. Call after any change to
    /// those fields (muted toggle, schedule CRUD, pair/unlink).
    /// </summary>
    Task PublishStateAsync(Guid deviceId, CancellationToken ct = default);

    /// <summary>Publish a one-shot watering pulse on <c>synora/devices/{id}/commands/water</c>.</summary>
    Task PublishWaterCommandAsync(Guid deviceId, int durationSeconds, CancellationToken ct = default);

    /// <summary>Publish a one-shot ping on <c>synora/devices/{id}/commands/ping</c>.</summary>
    Task PublishPingCommandAsync(Guid deviceId, CancellationToken ct = default);
}
