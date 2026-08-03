using System.Collections.Concurrent;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// A stand-in <see cref="IVoicePipeline"/> that proves the full wire contract
/// end-to-end without any ASR/LLM/TTS backend: it buffers the mic Opus during a
/// listen turn, then on stop plays the whole sequence back as the "assistant"
/// response — exercising every server -> device message type (stt, llm, tts
/// start / sentence_start / stop) and the binary audio path in both directions.
///
/// Replaced by the real ASR -> LLM -> TTS pipeline in a later slice; the
/// endpoint and session are unaffected.
/// </summary>
public sealed class LoopbackPipeline : IVoicePipeline
{
    // Per-session capture buffer. Concurrent because audio frames and the
    // stop control message may arrive close together.
    private readonly ConcurrentDictionary<string, List<byte[]>> _capture = new();

    public Task OnSessionStartAsync(VoiceSession session, CancellationToken ct)
    {
        _capture[session.SessionId] = new List<byte[]>();
        return Task.CompletedTask;
    }

    public Task OnListenStartAsync(VoiceSession session, string mode, CancellationToken ct)
    {
        _capture[session.SessionId] = new List<byte[]>();
        return Task.CompletedTask;
    }

    public Task OnAudioAsync(VoiceSession session, ReadOnlyMemory<byte> opus, CancellationToken ct)
    {
        if (_capture.TryGetValue(session.SessionId, out var frames))
        {
            frames.Add(opus.ToArray());
        }
        return Task.CompletedTask;
    }

    public async Task OnListenStopAsync(VoiceSession session, CancellationToken ct)
    {
        _capture.TryGetValue(session.SessionId, out var frames);
        frames ??= new List<byte[]>();

        // Mimic a real turn: transcript, expression, then spoken response.
        await session.SendJsonAsync(ServerMessages.Stt("[loopback]", session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Llm("happy", session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Tts("start", session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Tts("sentence_start", session.SessionId, "(echo)"), ct);

        // Replay the captured Opus back as the "assistant" audio.
        uint ts = 0;
        foreach (var frame in frames)
        {
            await session.SendAudioAsync(frame, ts, ct);
            ts += (uint)session.FrameDuration;
        }

        await session.SendJsonAsync(ServerMessages.Tts("stop", session.SessionId), ct);
        frames.Clear();
    }

    public Task OnAbortAsync(VoiceSession session, CancellationToken ct)
    {
        if (_capture.TryGetValue(session.SessionId, out var frames))
        {
            frames.Clear();
        }
        return Task.CompletedTask;
    }
}
