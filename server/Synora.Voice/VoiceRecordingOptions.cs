namespace Synora.Voice;

/// <summary>
/// Config for where LlmVoicePipeline writes the user's raw captured audio
/// (as WAV) so it can be played back next to the transcribed text, bound
/// from the "Recordings" configuration section. Same file-on-disk-plus-path-
/// column convention as Avatar:StoragePath in Synora.Api/Program.cs, kept as
/// its own section/directory since these are recordings, not images.
/// </summary>
public sealed class VoiceRecordingOptions
{
    public string StoragePath { get; set; } = "/data/recordings";
}
