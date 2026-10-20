using System.Collections.Concurrent;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Synora.Core;
using Synora.Infrastructure;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// The real voice pipeline: the transcript comes from a real
/// <see cref="ISpeechToText"/> call (ASR sidecar, PhoWhisper-small -- see
/// OpusAudioDecoder for turning the captured Opus frames into the PCM it
/// expects), the assistant's reply TEXT comes from a real
/// <see cref="ILlmClient"/> call, and the reply AUDIO comes from a real
/// <see cref="ITextToSpeech"/> call (TTS sidecar, facebook/mms-tts-vie) --
/// re-encoded to Opus via OpusAudioDecoder.EncodeToOpusFrames() for the
/// device. If TTS fails or returns nothing (network hiccup, empty text),
/// falls back to echoing the user's own recording rather than going
/// completely silent. After each turn, the exchange is persisted as a
/// <see cref="DiscussionMessage"/> (best-effort -- skipped, not thrown, if
/// the device can't be resolved).
/// </summary>
public sealed class LlmVoicePipeline : IVoicePipeline
{
    // Matches the firmware's SynoraVoiceSession audio_params exactly (both
    // the Opus decode/encode here and the encode/decode there are fixed to
    // this).
    private const int SampleRateHz = 16000;
    private const int FrameDurationMs = 60;

    // Synora is a Vietnamese voice-AI companion (see DESIGN.md) -- always
    // reply in Vietnamese regardless of the input language, since ASR
    // failures/silence can still hand the LLM an empty or garbled transcript.
    // Public so Program.cs's admin chat-test endpoint can exercise the exact
    // same prompt rather than a hand-copied (and driftable) duplicate.
    public const string SystemPrompt =
        "You are Synora, a friendly plant-care voice assistant. Always reply in Vietnamese (Tiếng Việt), " +
        "regardless of what language the user's message is in. Keep replies short and conversational. " +
        "You only discuss the plant, its pot/device, and sensor readings/status (temperature, humidity, " +
        "soil moisture, water level, watering) -- use your tools for these whenever they'd give a more " +
        "accurate answer than guessing. If the user asks about anything else (general knowledge, other " +
        "people/topics, requests unrelated to this plant or device), politely decline in one short " +
        "sentence and steer back to the plant -- do not answer the off-topic question itself.";

    // Per-session capture buffer, same shape/reasoning as LoopbackPipeline.
    private readonly ConcurrentDictionary<string, List<byte[]>> _capture = new();
    private readonly ISpeechToText _speechToText;
    private readonly ILlmClient _llmClient;
    private readonly ITextToSpeech _textToSpeech;
    private readonly McpBridge _mcpBridge;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly ILogger<LlmVoicePipeline> _logger;
    private readonly string _recordingsStoragePath;

    public LlmVoicePipeline(
        ISpeechToText speechToText, ILlmClient llmClient, ITextToSpeech textToSpeech, McpBridge mcpBridge,
        IServiceScopeFactory scopeFactory, ILogger<LlmVoicePipeline> logger, IOptions<VoiceRecordingOptions> recordingOptions)
    {
        _speechToText = speechToText;
        _llmClient = llmClient;
        _textToSpeech = textToSpeech;
        _mcpBridge = mcpBridge;
        _scopeFactory = scopeFactory;
        _logger = logger;
        _recordingsStoragePath = recordingOptions.Value.StoragePath;
        Directory.CreateDirectory(_recordingsStoragePath);
    }

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

        var pcm = OpusAudioDecoder.DecodeToPcm16(frames, SampleRateHz, FrameDurationMs);
        var userText = await _speechToText.TranscribeAsync(pcm, SampleRateHz, ct);

        var tools = await _mcpBridge.GetToolsAsync(session.DeviceId, session, ct);
        var assistantText = await _llmClient.GetReplyAsync(
            SystemPrompt, userText, tools,
            (name, args, callCt) => _mcpBridge.CallToolAsync(session.DeviceId, session, name, args, callCt),
            ct);

        await session.SendJsonAsync(ServerMessages.Stt(userText, session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Llm("neutral", session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Tts("start", session.SessionId), ct);
        await session.SendJsonAsync(ServerMessages.Tts("sentence_start", session.SessionId, assistantText), ct);

        var (ttsPcm, ttsSampleRateHz) = await _textToSpeech.SynthesizeAsync(assistantText, ct);
        // Fall back to echoing the user's own recording if TTS failed or
        // produced nothing -- degraded (the reply audio won't match
        // assistantText), but not completely silent.
        var replyFrames = ttsPcm.Length > 0
            ? OpusAudioDecoder.EncodeToOpusFrames(ttsPcm, ttsSampleRateHz, FrameDurationMs)
            : frames;

        uint ts = 0;
        foreach (var frame in replyFrames)
        {
            await session.SendAudioAsync(frame, ts, ct);
            ts += (uint)session.FrameDuration;
        }

        await session.SendJsonAsync(ServerMessages.Tts("stop", session.SessionId), ct);
        frames.Clear();

        // Written from the same decoded `pcm` STT ran on, before it goes out
        // of scope -- so playback matches exactly what was transcribed,
        // letting the web console's Discussions view compare "what was
        // actually said" against "what STT parsed it as" to spot
        // transcription errors.
        var userAudioFileName = TryWriteUserAudio(session.SessionId, pcm);

        await PersistDiscussionAsync(session, userText, assistantText, userAudioFileName, ct);
    }

    // Best-effort, like PersistDiscussionAsync below: a failure to write the
    // recording (disk full, permissions) shouldn't take down the turn or
    // lose the transcript/reply. Null pcm (empty capture -- e.g. the user
    // never actually spoke before stop) just skips the file entirely rather
    // than writing an empty WAV.
    private string? TryWriteUserAudio(string sessionId, short[] pcm)
    {
        if (pcm.Length == 0)
        {
            return null;
        }

        try
        {
            var fileName = $"{sessionId}-{Guid.NewGuid():N}.wav";
            var fullPath = Path.Combine(_recordingsStoragePath, fileName);
            File.WriteAllBytes(fullPath, OpusAudioDecoder.BuildWav(pcm, SampleRateHz));
            return fileName;
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to write user audio recording for session {SessionId}", sessionId);
            return null;
        }
    }

    public Task OnAbortAsync(VoiceSession session, CancellationToken ct)
    {
        if (_capture.TryGetValue(session.SessionId, out var frames))
        {
            frames.Clear();
        }
        return Task.CompletedTask;
    }

    // LlmVoicePipeline is a singleton but SynoraDbContext is scoped, so a
    // fresh scope is created per turn to resolve it.
    private async Task PersistDiscussionAsync(
        VoiceSession session, string userText, string assistantText, string? userAudioFileName, CancellationToken ct)
    {
        // VoiceSession.DeviceId is the server-assigned Device.Id guid, taken
        // straight from the MQTT topic (synora/devices/{deviceId}/voice/...)
        // -- unlike the old WebSocket endpoint, which had to resolve a
        // MAC-address "Device-Id" header to a Device row, the device already
        // knows its own guid after activation. No matching Device row (e.g.
        // a stale/unknown id) just skips persistence rather than throwing.
        try
        {
            using var scope = _scopeFactory.CreateScope();
            var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();

            var deviceExists = await db.Devices.AnyAsync(d => d.Id == session.DeviceId, ct);
            if (!deviceExists)
            {
                return;
            }

            db.DiscussionMessages.Add(new DiscussionMessage
            {
                DeviceId = session.DeviceId,
                SessionId = session.SessionId,
                UserText = userText,
                AssistantText = assistantText,
                CreatedAt = DateTimeOffset.UtcNow,
                UserAudioPath = userAudioFileName,
            });
            await db.SaveChangesAsync(ct);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to persist discussion history for session {SessionId}", session.SessionId);
        }
    }
}
