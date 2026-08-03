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
}
