using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// OpenAI-compatible chat-completions client. Defaults to DeepSeek's API, but
/// any OpenAI-compatible endpoint works via LlmOptions.BaseUrl/Model. The
/// real API key comes from the Llm__ApiKey environment variable (or
/// Llm:ApiKey in appsettings) -- never hardcoded here.
/// </summary>
public sealed class DeepSeekLlmClient : ILlmClient
{
    // Each round is one chat-completions round trip; a round with tool_calls
    // costs one or more additional device round trips (via callTool) before
    // the next round. Caps a model that keeps calling tools instead of
    // answering from looping forever.
    private const int MaxToolRounds = 4;

    private static readonly JsonSerializerOptions RequestJsonOptions = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private readonly IHttpClientFactory _httpClientFactory;
    private readonly LlmOptions _options;
    private readonly ILogger<DeepSeekLlmClient> _logger;

    public DeepSeekLlmClient(IHttpClientFactory httpClientFactory, IOptions<LlmOptions> options, ILogger<DeepSeekLlmClient> logger)
    {
        _httpClientFactory = httpClientFactory;
        _options = options.Value;
        _logger = logger;
    }

    public async Task<string> GetReplyAsync(
        string systemPrompt, string userText,
        IReadOnlyList<McpToolDefinition> tools,
        Func<string, JsonElement, CancellationToken, Task<JsonElement>> callTool,
        CancellationToken ct)
    {
        if (string.IsNullOrEmpty(_options.ApiKey))
        {
            _logger.LogWarning("Llm:ApiKey is not configured -- set Llm__ApiKey. Skipping the real LLM call.");
            return "(LLM not configured)";
        }

        var client = _httpClientFactory.CreateClient();
        // BaseAddress must end with "/" and the request path below must NOT
        // start with "/" -- otherwise .NET's URI-combining rules treat the
        // request path as root-relative and silently drop any path segment
        // already in BaseAddress (invisible with DeepSeek's bare-host
        // BaseUrl, but broke immediately against OpenRouter's
        // https://openrouter.ai/api/v1: requests landed on openrouter.ai's
        // homepage instead of /api/v1/chat/completions, returning an HTML
        // page that failed to parse as JSON).
        var baseUrl = _options.BaseUrl.EndsWith('/') ? _options.BaseUrl : _options.BaseUrl + "/";
        client.BaseAddress = new Uri(baseUrl);
        client.DefaultRequestHeaders.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", _options.ApiKey);

        var toolsWire = tools.Count == 0 ? null : tools.Select(ToWireTool).ToArray();
        var messages = new List<ChatMessage>
        {
            new("system", systemPrompt),
            new("user", userText),
        };

        // Tracks whether any tool actually ran this turn -- shapes which
        // fallback text below fits if the model then returns empty content,
        // which does happen occasionally (model-dependent) right after a
        // successful tool call. Confirmed live: a real self.do_watering call
        // ran fine, but the model's follow-up message came back blank, and
        // the literal "(no reply)" placeholder that used to fill that gap
        // would otherwise have been spoken/shown to the user verbatim.
        var usedTool = false;

        try
        {
            for (int round = 0; round < MaxToolRounds; round++)
            {
                var request = new ChatCompletionRequest(_options.Model, messages.ToArray(), Stream: false, Tools: toolsWire);
                using var response = await client.PostAsJsonAsync("chat/completions", request, RequestJsonOptions, ct);
                response.EnsureSuccessStatusCode();

                var body = await response.Content.ReadFromJsonAsync<ChatCompletionResponse>(cancellationToken: ct);
                var message = body?.Choices?.FirstOrDefault()?.Message;

                if (message is null || message.ToolCalls is not { Length: > 0 } toolCalls)
                {
                    var reply = message?.Content;
                    if (!string.IsNullOrWhiteSpace(reply))
                    {
                        return reply.Trim();
                    }
                    return usedTool
                        ? "Xong rồi nhé!"
                        : "Xin lỗi, mình chưa nghĩ ra câu trả lời, bạn hỏi lại được không?";
                }

                // Echo the assistant's tool-call request back into the
                // conversation (required by the chat-completions contract so
                // the follow-up "tool" messages below have something to
                // attach to), then execute each requested call and append its
                // result before looping back for the model's next turn.
                usedTool = true;
                messages.Add(message);
                foreach (var call in toolCalls)
                {
                    var toolResult = await ExecuteToolCallAsync(call, callTool, ct);
                    messages.Add(new ChatMessage("tool", toolResult, ToolCallId: call.Id));
                }
            }

            _logger.LogWarning("LLM tool-calling loop exceeded {MaxRounds} rounds without a final reply", MaxToolRounds);
            return "Xin lỗi, mình chưa hoàn tất được yêu cầu đó.";
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            _logger.LogWarning(ex, "LLM call failed.");
            return "Xin lỗi, mình chưa nghĩ ra câu trả lời lúc này.";
        }
    }

    private async Task<string> ExecuteToolCallAsync(
        ToolCallRequest call, Func<string, JsonElement, CancellationToken, Task<JsonElement>> callTool, CancellationToken ct)
    {
        JsonElement arguments;
        try
        {
            arguments = string.IsNullOrWhiteSpace(call.Function.Arguments)
                ? JsonSerializer.SerializeToElement(new { })
                // .Clone() so the JsonElement stays valid after the transient
                // JsonDocument backing it is garbage-collected (not wrapped
                // in `using` since it doesn't need to outlive this line).
                : JsonDocument.Parse(call.Function.Arguments).RootElement.Clone();
        }
        catch (JsonException ex)
        {
            _logger.LogWarning(ex, "Malformed tool-call arguments for {ToolName}: {Arguments}", call.Function.Name, call.Function.Arguments);
            return JsonSerializer.Serialize(new { error = new { message = "malformed arguments" } });
        }

        var result = await callTool(call.Function.Name, arguments, ct);
        return result.GetRawText();
    }

    private static ToolDefinitionWire ToWireTool(McpToolDefinition tool) =>
        new("function", new ToolFunctionWire(tool.Name, tool.Description, tool.InputSchema));

    private sealed record ChatCompletionRequest(
        [property: JsonPropertyName("model")] string Model,
        [property: JsonPropertyName("messages")] ChatMessage[] Messages,
        [property: JsonPropertyName("stream")] bool Stream,
        [property: JsonPropertyName("tools")] ToolDefinitionWire[]? Tools = null);

    private sealed record ChatMessage(
        [property: JsonPropertyName("role")] string Role,
        [property: JsonPropertyName("content")] string? Content,
        [property: JsonPropertyName("tool_calls")] ToolCallRequest[]? ToolCalls = null,
        [property: JsonPropertyName("tool_call_id")] string? ToolCallId = null);

    private sealed record ToolCallRequest(
        [property: JsonPropertyName("id")] string Id,
        [property: JsonPropertyName("type")] string Type,
        [property: JsonPropertyName("function")] ToolCallFunction Function);

    private sealed record ToolCallFunction(
        [property: JsonPropertyName("name")] string Name,
        [property: JsonPropertyName("arguments")] string Arguments);

    private sealed record ToolDefinitionWire(
        [property: JsonPropertyName("type")] string Type,
        [property: JsonPropertyName("function")] ToolFunctionWire Function);

    private sealed record ToolFunctionWire(
        [property: JsonPropertyName("name")] string Name,
        [property: JsonPropertyName("description")] string Description,
        [property: JsonPropertyName("parameters")] JsonElement Parameters);

    private sealed record ChatCompletionResponse(
        [property: JsonPropertyName("choices")] ChatCompletionChoice[]? Choices);

    private sealed record ChatCompletionChoice(
        [property: JsonPropertyName("message")] ChatMessage? Message);
}
