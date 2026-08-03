using System.Text.Json;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// Canned <see cref="ILlmClient"/> for the "Testing" environment so
/// <see cref="LlmVoicePipeline"/> can be exercised end-to-end (including the
/// discussion-history persistence) without ever hitting a real LLM API.
/// Ignores <c>tools</c>/<c>callTool</c> -- tool-calling itself is exercised
/// against <see cref="McpBridge"/> directly, not through this fake.
/// </summary>
public sealed class FakeLlmClient : ILlmClient
{
    public const string CannedReply = "Your plant looks happy today!";

    public Task<string> GetReplyAsync(
        string systemPrompt, string userText,
        IReadOnlyList<McpToolDefinition> tools,
        Func<string, JsonElement, CancellationToken, Task<JsonElement>> callTool,
        CancellationToken ct) =>
        Task.FromResult(CannedReply);
}
