namespace Synora.Voice;

/// <summary>
/// The voice behaviour behind a connection: what happens when the device opens a
/// session, starts/stops listening, streams mic Opus, or aborts. The transport
/// (<see cref="VoiceMqttDispatcher"/>, driven by MQTT control/audio messages)
/// drives these; implementations decide what to send back via the
/// <see cref="VoiceSession"/>.
///
/// The real pipeline (ASR -> LLM -> TTS) and the <see cref="LoopbackPipeline"/>
/// both implement this, so the endpoint doesn't change when the backend does.
/// </summary>
public interface IVoicePipeline
{
    /// <summary>Session established (server hello already sent).</summary>
    Task OnSessionStartAsync(VoiceSession session, CancellationToken ct);

    /// <summary>Device began capturing (mode = "auto" | "manual" | "realtime").</summary>
    Task OnListenStartAsync(VoiceSession session, string mode, CancellationToken ct);

    /// <summary>One Opus frame of mic audio from the device.</summary>
    Task OnAudioAsync(VoiceSession session, ReadOnlyMemory<byte> opus, CancellationToken ct);

    /// <summary>Device stopped capturing — the turn is complete, produce a response.</summary>
    Task OnListenStopAsync(VoiceSession session, CancellationToken ct);

    /// <summary>Device asked to abort the current response (e.g. wake word barge-in).</summary>
    Task OnAbortAsync(VoiceSession session, CancellationToken ct);
}
