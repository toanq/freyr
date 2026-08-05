using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// The device-side MCP (Model Context Protocol) bridge: discovers the tools
/// a device declares (JSON-RPC <c>initialize</c> + <c>tools/list</c>, with
/// cursor-based pagination) and invokes them (<c>tools/call</c>), all riding
/// the existing voice control channel (<see cref="VoiceSession.SendJsonAsync"/>,
/// wrapped as <see cref="ServerMessages.Mcp"/>) rather than a separate
/// transport. <see cref="VoiceMqttDispatcher"/> routes the device's <c>mcp</c>
/// responses here via <see cref="HandleResponse"/>; <see cref="LlmVoicePipeline"/>
/// calls <see cref="GetToolsAsync"/>/<see cref="CallToolAsync"/> to let the LLM
/// see and invoke those tools.
///
/// Tool discovery is cached per device for the lifetime of the server process
/// (the returned <see cref="Task{T}"/> itself is cached, coalescing concurrent
/// callers) rather than re-run on every session -- a device's <c>hello</c>
/// fires on every button press/relisten, and a firmware's tool set only
/// changes on a flash+reboot, so re-discovering every turn would add a device
/// round trip to every single conversation turn for no benefit.
/// </summary>
public sealed class McpBridge
{
    // initialize/tools/list are simple, near-instant device-side lookups;
    // tools/call may involve real hardware (e.g. self.do_watering), hence
    // the longer budget.
    private static readonly TimeSpan DiscoveryTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(8);

    // Hard cap on tools/list pagination rounds -- a misbehaving device
    // returning a non-empty nextCursor forever shouldn't be able to hang
    // discovery indefinitely.
    private const int MaxListPages = 20;

    private int _nextRequestId;
    private readonly ConcurrentDictionary<string, TaskCompletionSource<JsonElement>> _pending = new();
    private readonly ConcurrentDictionary<Guid, Task<IReadOnlyList<McpToolDefinition>>> _toolsCache = new();
    private readonly ILogger<McpBridge> _logger;

    public McpBridge(ILogger<McpBridge> logger)
    {
        _logger = logger;
    }

    /// <summary>
    /// Tools declared by this device, discovering (and caching) them on first
    /// use. Never throws -- a device that doesn't answer just yields an empty
    /// list, so a turn without MCP support still gets a normal LLM reply.
    /// </summary>
    public Task<IReadOnlyList<McpToolDefinition>> GetToolsAsync(Guid deviceId, VoiceSession session, CancellationToken ct) =>
        _toolsCache.GetOrAdd(deviceId, _ => DiscoverToolsAsync(deviceId, session, ct));

    /// <summary>
    /// Invoke one tool on the device and return its JSON-RPC response
    /// (<c>result</c> or <c>error</c>). Never throws for a timeout/no-answer
    /// case -- returns a synthetic <c>{"error":{"message":"..."}}</c> element
    /// instead, so callers (the LLM tool-calling loop) can tell the model the
    /// call failed rather than the whole turn blowing up.
    /// </summary>
    public Task<JsonElement> CallToolAsync(
        Guid deviceId, VoiceSession session, string name, JsonElement arguments, CancellationToken ct)
    {
        int id = Interlocked.Increment(ref _nextRequestId);
        var request = new
        {
            jsonrpc = "2.0",
            method = "tools/call",
            id,
            @params = new { name, arguments },
        };
        return SendAndWaitAsync(deviceId, session, id, request, CallTimeout, ct);
    }

    /// <summary>Called by <see cref="VoiceMqttDispatcher"/> for every device <c>mcp</c> message.</summary>
    public void HandleResponse(Guid deviceId, JsonElement payload)
    {
        if (!payload.TryGetProperty("id", out var idEl) || idEl.ValueKind != JsonValueKind.Number)
        {
            return;
        }
        if (_pending.TryRemove(PendingKey(deviceId, idEl.GetInt32()), out var tcs))
        {
            // Clone -- payload's backing JsonDocument is disposed the moment
            // VoiceMqttDispatcher.HandleControlAsync's `using (doc)` block
            // returns, but the TCS is awaited from a different async flow
            // (DiscoverToolsAsync, running as a background task) that
            // resumes later, after that disposal. Confirmed live: without
            // this, IsSuccess() on the awaited result threw
            // ObjectDisposedException("JsonDocument") every time, silently
            // aborting MCP tool discovery for every session.
            tcs.TrySetResult(payload.Clone());
        }
    }

    private async Task<IReadOnlyList<McpToolDefinition>> DiscoverToolsAsync(Guid deviceId, VoiceSession session, CancellationToken ct)
    {
        int initId = Interlocked.Increment(ref _nextRequestId);
        var initRequest = new { jsonrpc = "2.0", method = "initialize", id = initId, @params = new { capabilities = new { } } };
        var initResponse = await SendAndWaitAsync(deviceId, session, initId, initRequest, DiscoveryTimeout, ct);
        if (!IsSuccess(initResponse))
        {
            _logger.LogWarning("Device {DeviceId} did not respond to MCP initialize -- no tools available this session", deviceId);
            return Array.Empty<McpToolDefinition>();
        }

        var tools = new List<McpToolDefinition>();
        string cursor = "";
        for (int page = 0; page < MaxListPages; page++)
        {
            int listId = Interlocked.Increment(ref _nextRequestId);
            var listRequest = new { jsonrpc = "2.0", method = "tools/list", id = listId, @params = new { cursor } };
            var listResponse = await SendAndWaitAsync(deviceId, session, listId, listRequest, DiscoveryTimeout, ct);
            if (!IsSuccess(listResponse))
            {
                _logger.LogWarning(
                    "Device {DeviceId} tools/list failed/timed out after {Count} tool(s) discovered", deviceId, tools.Count);
                break;
            }

            var result = listResponse.GetProperty("result");
            if (result.TryGetProperty("tools", out var toolsEl) && toolsEl.ValueKind == JsonValueKind.Array)
            {
                foreach (var t in toolsEl.EnumerateArray())
                {
                    var toolName = t.TryGetProperty("name", out var n) ? n.GetString() : null;
                    if (toolName is null)
                    {
                        continue;
                    }
                    var description = t.TryGetProperty("description", out var d) ? d.GetString() ?? "" : "";
                    var inputSchema = t.TryGetProperty("inputSchema", out var s)
                        ? s
                        : JsonSerializer.SerializeToElement(new { type = "object", properties = new { } });
                    tools.Add(new McpToolDefinition(toolName, description, inputSchema));
                }
            }

            cursor = result.TryGetProperty("nextCursor", out var nc) && nc.ValueKind == JsonValueKind.String
                ? nc.GetString() ?? ""
                : "";
            if (cursor.Length == 0)
            {
                break;
            }
        }

        _logger.LogInformation("Discovered {Count} MCP tool(s) for device {DeviceId}", tools.Count, deviceId);
        return tools;
    }

    private async Task<JsonElement> SendAndWaitAsync(
        Guid deviceId, VoiceSession session, int id, object request, TimeSpan timeout, CancellationToken ct)
    {
        var key = PendingKey(deviceId, id);
        var tcs = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[key] = tcs;
        try
        {
            var payload = JsonSerializer.SerializeToElement(request);
            await session.SendJsonAsync(ServerMessages.Mcp(payload, session.SessionId), ct);

            // Cancellation (caller's ct, or this timeout) resolves the same
            // TCS with a synthetic error rather than throwing -- callers
            // (the LLM tool-calling loop) only ever need to branch on
            // "did the tool call succeed", not catch OperationCanceledException.
            using var timeoutCts = new CancellationTokenSource(timeout);
            using var reg = timeoutCts.Token.Register(() => tcs.TrySetResult(TimeoutErrorElement()));
            using var ctReg = ct.Register(() => tcs.TrySetResult(TimeoutErrorElement()));
            return await tcs.Task;
        }
        finally
        {
            _pending.TryRemove(key, out _);
        }
    }

    private static bool IsSuccess(JsonElement response) =>
        response.ValueKind == JsonValueKind.Object && response.TryGetProperty("result", out _);

    private static JsonElement TimeoutErrorElement() =>
        JsonSerializer.SerializeToElement(new { error = new { message = "tool call timed out" } });

    private static string PendingKey(Guid deviceId, int id) => $"{deviceId:N}:{id}";
}
