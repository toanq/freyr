using System.Net.Http.Json;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Synora.Infrastructure;

namespace Synora.Voice;

/// <summary>
/// Calls the TTS sidecar's POST /synthesize ({"text": "...", "voice": ...}
/// -> WAV bytes). See tts/app.py -- VieNeu-TTS v3 Turbo (ONNX/CPU), 14
/// built-in preset voices. Reuses OpusAudioDecoder.ParseWav16Mono (built for
/// the admin STT test's upload parsing) to turn the WAV response back into
/// PCM16 samples. Replaced MmsTtsClient (facebook/mms-tts-vie) after the
/// user asked for a more natural, female-voice option -- that backend was
/// single-speaker, so had no voice to choose.
/// </summary>
public sealed class VieNeuTtsClient : ITextToSpeech
{
    // The sidecar resamples VieNeu-TTS's native 48kHz output down to this
    // before returning (see tts/app.py's OUTPUT_SAMPLE_RATE_HZ) specifically
    // so the rest of the pipeline (Opus re-encode here, the firmware's
    // fixed-rate Opus decoder) doesn't have to change just because the TTS
    // backend did.
    private const int SampleRateHz = 16000;

    private readonly IHttpClientFactory _httpClientFactory;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly TtsOptions _options;
    private readonly ILogger<VieNeuTtsClient> _logger;

    public VieNeuTtsClient(
        IHttpClientFactory httpClientFactory, IServiceScopeFactory scopeFactory, IOptions<TtsOptions> options,
        ILogger<VieNeuTtsClient> logger)
    {
        _httpClientFactory = httpClientFactory;
        _scopeFactory = scopeFactory;
        _options = options.Value;
        _logger = logger;
    }

    public async Task<(short[] Pcm, int SampleRateHz)> SynthesizeAsync(string text, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return (Array.Empty<short>(), SampleRateHz);
        }

        var voice = await GetVoiceAsync(ct);

        var client = _httpClientFactory.CreateClient();
        // Cold CPU inference for a longer reply can take a few seconds;
        // generous but not unbounded.
        client.Timeout = TimeSpan.FromSeconds(30);

        try
        {
            using var response = await client.PostAsJsonAsync($"{_options.BaseUrl}/synthesize", new { text, voice }, ct);
            response.EnsureSuccessStatusCode();
            var wavBytes = await response.Content.ReadAsByteArrayAsync(ct);
            var pcm = OpusAudioDecoder.ParseWav16Mono(wavBytes, SampleRateHz);
            return (pcm, SampleRateHz);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or InvalidDataException)
        {
            _logger.LogWarning(ex, "TTS call failed.");
            return (Array.Empty<short>(), SampleRateHz);
        }
    }

    // Read fresh on every call (not cached) -- VieNeuTtsClient is a
    // singleton but this value is admin-editable at runtime (see Program.cs's
    // GET/PUT /admin/system-settings), and a single extra indexed-by-PK read
    // is negligible next to the TTS call itself.
    private async Task<string?> GetVoiceAsync(CancellationToken ct)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();
        var settings = await db.SystemSettings.FindAsync([1], ct);
        return settings?.TtsVoice;
    }
}
