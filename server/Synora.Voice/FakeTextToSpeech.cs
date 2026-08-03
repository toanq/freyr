namespace Synora.Voice;

/// <summary>
/// Testing-environment stand-in for <see cref="MmsTtsClient"/> -- returns a
/// short burst of silent PCM without a network call, matching
/// FakeSpeechToText/FakeLlmClient's pattern.
/// </summary>
public sealed class FakeTextToSpeech : ITextToSpeech
{
    private const int SampleRateHz = 16000;

    public Task<(short[] Pcm, int SampleRateHz)> SynthesizeAsync(string text, CancellationToken ct) =>
        Task.FromResult((new short[SampleRateHz / 10], SampleRateHz)); // 100ms of silence
}
