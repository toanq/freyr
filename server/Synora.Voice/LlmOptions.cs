namespace Synora.Voice;

/// <summary>
/// Config for the OpenAI-compatible chat-completions backend (DeepSeek by
/// default), bound from the "Llm" configuration section. ApiKey has no
/// default -- it must come from config/environment (Llm__ApiKey), never a
/// hardcoded value.
/// </summary>
public sealed class LlmOptions
{
    public string BaseUrl { get; set; } = "https://api.deepseek.com";
    public string Model { get; set; } = "deepseek-chat";
    public string? ApiKey { get; set; }
}
