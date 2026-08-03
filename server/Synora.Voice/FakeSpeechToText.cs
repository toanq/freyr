namespace Synora.Voice;

/// <summary>
/// Testing-environment stand-in for <see cref="PhoWhisperClient"/> -- returns
/// a canned transcript without a network call, matching FakeLlmClient's
/// pattern for ILlmClient.
/// </summary>
public sealed class FakeSpeechToText : ISpeechToText
{
    public const string CannedTranscript = "[fake transcript]";

    public Task<string> TranscribeAsync(short[] pcmSamples, int sampleRateHz, CancellationToken ct) =>
        Task.FromResult(CannedTranscript);
}
