using System.Net.Http.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace Synora.Voice;

/// <summary>
/// Calls the ASR sidecar's POST /transcribe (a WAV file upload -> {"text": "..."}).
/// See asr/app.py -- faster-whisper + PhoWhisper-small, forced to Vietnamese.
/// </summary>
public sealed class PhoWhisperClient : ISpeechToText
{
    private readonly IHttpClientFactory _httpClientFactory;
    private readonly AsrOptions _options;
    private readonly ILogger<PhoWhisperClient> _logger;

    public PhoWhisperClient(IHttpClientFactory httpClientFactory, IOptions<AsrOptions> options, ILogger<PhoWhisperClient> logger)
    {
        _httpClientFactory = httpClientFactory;
        _options = options.Value;
        _logger = logger;
    }

    public async Task<string> TranscribeAsync(short[] pcmSamples, int sampleRateHz, CancellationToken ct)
    {
        if (pcmSamples.Length == 0)
        {
            return "";
        }

        var wavBytes = OpusAudioDecoder.BuildWav(pcmSamples, sampleRateHz);

        var client = _httpClientFactory.CreateClient();
        // The sidecar's model-conversion step can take several minutes on a
        // cold start (see asr/entrypoint.sh) -- generous timeout so the
        // first request after a fresh `docker compose up` doesn't just fail.
        client.Timeout = TimeSpan.FromMinutes(2);

        using var content = new MultipartFormDataContent();
        using var audioContent = new ByteArrayContent(wavBytes);
        audioContent.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("audio/wav");
        content.Add(audioContent, "file", "audio.wav");

        try
        {
            using var response = await client.PostAsync($"{_options.BaseUrl}/transcribe", content, ct);
            response.EnsureSuccessStatusCode();
            var body = await response.Content.ReadFromJsonAsync<TranscribeResponse>(cancellationToken: ct);
            return body?.Text?.Trim() ?? "";
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            _logger.LogWarning(ex, "ASR call failed.");
            return "";
        }
    }

    private sealed record TranscribeResponse([property: JsonPropertyName("text")] string? Text);
}
