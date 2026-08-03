namespace Synora.Voice;

/// <summary>
/// Config for the ASR sidecar (faster-whisper + PhoWhisper-small), bound
/// from the "Asr" configuration section. No API key -- it's our own sidecar
/// on the compose network, not a third-party API.
/// </summary>
public sealed class AsrOptions
{
    public string BaseUrl { get; set; } = "http://localhost:8000";
}
