namespace Synora.Core;

// One user<->assistant turn from the voice pipeline, persisted so the web
// console can show a device's conversation history. UserText is currently
// always the ASR placeholder ("[loopback]") until real STT lands -- see
// LlmVoicePipeline in Synora.Voice.
public class DiscussionMessage
{
    public Guid Id { get; set; }
    public Guid DeviceId { get; set; }
    public Device? Device { get; set; }
    public required string SessionId { get; set; }
    public required string UserText { get; set; }
    public required string AssistantText { get; set; }
    public DateTimeOffset CreatedAt { get; set; }

    // Filename (not a full path) of the WAV recording of what the user
    // actually said, stored under Recordings:StoragePath -- same
    // file-on-disk-plus-path-column convention as Device.AvatarPath/
    // User.AvatarPath. Null for turns captured before this field existed,
    // or when the capture buffer was empty (e.g. abort-timing edge cases).
    public string? UserAudioPath { get; set; }
}
