namespace Synora.Voice;

/// <summary>
/// Text-to-speech, returning raw PCM16 mono samples plus the sample rate
/// they were produced at (the backend's own native rate -- callers
/// shouldn't assume a fixed value, even though the current MMS-TTS backend
/// happens to match the pipeline's fixed 16kHz already).
/// </summary>
public interface ITextToSpeech
{
    Task<(short[] Pcm, int SampleRateHz)> SynthesizeAsync(string text, CancellationToken ct);
}
