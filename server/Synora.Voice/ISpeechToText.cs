namespace Synora.Voice;

/// <summary>
/// Speech-to-text over already-decoded PCM audio (see <see cref="OpusAudioDecoder"/>
/// for turning the device's captured Opus frames into this shape).
/// </summary>
public interface ISpeechToText
{
    Task<string> TranscribeAsync(short[] pcmSamples, int sampleRateHz, CancellationToken ct);
}
