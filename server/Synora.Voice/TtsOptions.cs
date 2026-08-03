namespace Synora.Voice;

/// <summary>
/// Config for the TTS sidecar (VieNeu-TTS v3 Turbo), bound from the "Tts"
/// configuration section. No API key -- it's our own sidecar on the compose
/// network, not a third-party API (same reasoning as AsrOptions).
/// </summary>
public sealed class TtsOptions
{
    public string BaseUrl { get; set; } = "http://localhost:8001";
}
