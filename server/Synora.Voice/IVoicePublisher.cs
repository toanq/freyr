namespace Synora.Voice;

/// <summary>
/// Publishes voice control/audio messages downstream to a specific device's
/// MQTT "down" topics (<c>synora/devices/{deviceId}/voice/control/down</c> and
/// <c>.../voice/audio/down</c>). <see cref="VoiceSession"/> depends only on
/// this abstraction, not on MQTTnet directly -- the real implementation
/// (backed by the server's single long-lived MQTT client) lives in
/// Synora.Api, which is the only project that wires up the broker
/// connection.
/// </summary>
public interface IVoicePublisher
{
    /// <summary>Publish a control JSON message (hello/stt/llm/tts/system/alert) to a device, QoS 1.</summary>
    Task PublishControlAsync(Guid deviceId, string json, CancellationToken ct);

    /// <summary>Publish one raw Opus payload to a device, QoS 0 (no framing -- MQTT already frames each publish).</summary>
    Task PublishAudioAsync(Guid deviceId, ReadOnlyMemory<byte> opus, CancellationToken ct);
}
