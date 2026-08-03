namespace Synora.Voice;

/// <summary>
/// One live device voice session, keyed by the device's server-assigned
/// <see cref="Guid"/>. Owns the "send" side (serialized behind a lock, since
/// control JSON and binary Opus can be produced from different tasks) via an
/// injected <see cref="IVoicePublisher"/> -- MQTT already frames each publish
/// as a discrete message, so there's no wire-framing left for this type to
/// own (unlike the old WebSocket-based version, which encoded/decoded the
/// xiaozhi binary protocol header).
/// </summary>
public sealed class VoiceSession
{
    private readonly IVoicePublisher _publisher;
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    public Guid DeviceId { get; }
    public string SessionId { get; }
    public int ProtocolVersion { get; }
    public int SampleRate { get; }
    public int FrameDuration { get; }

    public VoiceSession(Guid deviceId, string sessionId, int protocolVersion,
                        int sampleRate, int frameDuration, IVoicePublisher publisher)
    {
        DeviceId = deviceId;
        SessionId = sessionId;
        ProtocolVersion = protocolVersion;
        SampleRate = sampleRate;
        FrameDuration = frameDuration;
        _publisher = publisher;
    }

    /// <summary>Send a control message (already-serialized JSON) on <c>voice/control/down</c>.</summary>
    public async Task SendJsonAsync(string json, CancellationToken ct)
    {
        await _sendLock.WaitAsync(ct);
        try
        {
            await _publisher.PublishControlAsync(DeviceId, json, ct);
        }
        finally { _sendLock.Release(); }
    }

    /// <summary>
    /// Send one Opus payload on <c>voice/audio/down</c>. <paramref name="timestamp"/> is
    /// unused now that MQTT frames each publish as a discrete message (it was only ever
    /// needed for the old xiaozhi binary header) -- kept so call sites in
    /// <c>LlmVoicePipeline</c>/<c>LoopbackPipeline</c> didn't need to change.
    /// </summary>
    public async Task SendAudioAsync(ReadOnlyMemory<byte> opus, uint timestamp, CancellationToken ct)
    {
        await _sendLock.WaitAsync(ct);
        try
        {
            await _publisher.PublishAudioAsync(DeviceId, opus, ct);
        }
        finally { _sendLock.Release(); }
    }
}
