using System.Text.Json;
using Microsoft.Extensions.Logging.Abstractions;
using Synora.Voice;
using Xunit;

namespace Synora.Api.Tests;

// McpBridge is the JSON-RPC request/response correlation layer between the
// LLM tool-calling loop and a device's MCP tools (see LlmVoicePipeline +
// VoiceMqttDispatcher's "mcp" case). These exercise it directly, without a
// dispatcher/broker, the same way VoiceEndpointTests exercises
// VoiceMqttDispatcher directly with a recording fake IVoicePublisher.
public class McpBridgeTests
{
    [Fact]
    public async Task CallToolAsync_ResolvesOnceHandleResponseReceivesMatchingId()
    {
        var bridge = new McpBridge(NullLogger<McpBridge>.Instance);
        var publisher = new RecordingVoicePublisher();
        var deviceId = Guid.NewGuid();
        var session = new VoiceSession(deviceId, "session-1", protocolVersion: 2, sampleRate: 16000, frameDuration: 60, publisher);

        var callTask = bridge.CallToolAsync(
            deviceId, session, "self.get_sensor_readings", JsonDocument.Parse("{}").RootElement, CancellationToken.None);

        // The fake publisher completes synchronously, but poll briefly rather
        // than assume that -- guards against this becoming a flaky test if
        // VoiceSession's send path ever becomes genuinely async.
        for (int i = 0; i < 50 && publisher.ControlMessages.Count == 0; i++)
        {
            await Task.Delay(5);
        }

        var sent = Assert.Single(publisher.ControlMessages);
        using var sentDoc = JsonDocument.Parse(sent);
        var payload = sentDoc.RootElement.GetProperty("payload");
        Assert.Equal("tools/call", payload.GetProperty("method").GetString());
        var id = payload.GetProperty("id").GetInt32();

        var responseJson = "{\"jsonrpc\":\"2.0\",\"id\":" + id +
            ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"true\"}],\"isError\":false}}";
        using var response = JsonDocument.Parse(responseJson);
        bridge.HandleResponse(deviceId, response.RootElement);

        var result = await callTask;
        Assert.True(result.TryGetProperty("result", out _));
    }

    [Fact]
    public async Task CallToolAsync_ReturnsErrorElement_WhenNoResponseArrives()
    {
        var bridge = new McpBridge(NullLogger<McpBridge>.Instance);
        var publisher = new RecordingVoicePublisher();
        var deviceId = Guid.NewGuid();
        var session = new VoiceSession(deviceId, "session-1", protocolVersion: 2, sampleRate: 16000, frameDuration: 60, publisher);

        // Cancel almost immediately instead of waiting out the real ~8s
        // per-call timeout -- CallToolAsync treats external cancellation the
        // same way it treats its own timeout (resolves with a synthetic
        // error rather than throwing), so this exercises the same "device
        // never answers" code path without a slow test.
        using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(50));

        var result = await bridge.CallToolAsync(
            deviceId, session, "self.do_watering",
            JsonDocument.Parse("""{"duration_seconds":5}""").RootElement, cts.Token);

        Assert.True(result.TryGetProperty("error", out _));
    }

    private sealed class RecordingVoicePublisher : IVoicePublisher
    {
        public List<string> ControlMessages { get; } = new();

        public Task PublishControlAsync(Guid deviceId, string json, CancellationToken ct)
        {
            ControlMessages.Add(json);
            return Task.CompletedTask;
        }

        public Task PublishAudioAsync(Guid deviceId, ReadOnlyMemory<byte> opus, CancellationToken ct) => Task.CompletedTask;
    }
}
