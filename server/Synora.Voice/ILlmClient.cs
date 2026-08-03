using System.Text.Json;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// Talks to the chat-completions LLM backend for one turn. Implementations:
/// <see cref="DeepSeekLlmClient"/> for the real OpenAI-compatible API, and a
/// canned stand-in registered in the "Testing" environment so the voice
/// endpoint test doesn't hit the network.
/// </summary>
public interface ILlmClient
{
    /// <summary>
    /// Get a reply to <paramref name="userText"/>, letting the model call
    /// into <paramref name="tools"/> (declared by the device, see
    /// <see cref="McpBridge"/>) as many times as it needs via
    /// <paramref name="callTool"/> before producing its final text reply.
    /// <paramref name="callTool"/> takes (tool name, JSON arguments) and
    /// returns the tool's JSON-RPC response (result or error) -- it never
    /// throws (see <see cref="McpBridge.CallToolAsync"/>).
    /// </summary>
    Task<string> GetReplyAsync(
        string systemPrompt,
        string userText,
        IReadOnlyList<McpToolDefinition> tools,
        Func<string, JsonElement, CancellationToken, Task<JsonElement>> callTool,
        CancellationToken ct);
}
