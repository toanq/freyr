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

    // Regression test for a live bug: GetToolsAsync used to cache the
    // *Task* returned by DiscoverToolsAsync forever (GetOrAdd), including
    // when discovery failed/timed out (e.g. a transient MQTT hiccup during
    // the hello-triggered background prefetch in VoiceMqttDispatcher). Once
    // that empty-list result was cached, every later real conversation turn
    // for that device got an empty tools list handed to the LLM forever --
    // confirmed live in this project as the reason "water the plant" never
    // actually ran the pump, even though the device was clearly responsive
    // again by the time the user spoke. GetToolsAsync must retry discovery
    // on the next call after a failed/empty one, rather than sticking with
    // the poisoned empty result for the rest of the process's life.
    [Fact]
    public async Task GetToolsAsync_RetriesAfterFailedDiscovery()
    {
        var bridge = new McpBridge(NullLogger<McpBridge>.Instance);
        var publisher = new RecordingVoicePublisher();
        var deviceId = Guid.NewGuid();
        var session = new VoiceSession(deviceId, "session-1", protocolVersion: 2, sampleRate: 16000, frameDuration: 60, publisher);

        // First attempt: nobody ever answers "initialize" -- cancel almost
        // immediately instead of waiting out the real ~5s discovery timeout,
        // same trick as CallToolAsync_ReturnsErrorElement_WhenNoResponseArrives.
        using (var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(50)))
        {
            var firstAttempt = await bridge.GetToolsAsync(deviceId, session, cts.Token);
            Assert.Empty(firstAttempt);
        }

        // Second attempt: the device is responsive now. If the empty result
        // above got permanently cached, this call would short-circuit to the
        // same empty list without sending anything -- so assert a fresh
        // "initialize" request actually goes out, then answer it (and the
        // "tools/list" that follows) for real.
        var secondAttemptTask = bridge.GetToolsAsync(deviceId, session, CancellationToken.None);

        // The first (abandoned) attempt already sent one "initialize" before
        // its ct fired, so the second attempt's own "initialize" is the
        // *second* control message overall, not the first.
        var initSent = await WaitForNthControlMessageAsync(publisher, index: 2);
        using (var initDoc = JsonDocument.Parse(initSent))
        {
            var payload = initDoc.RootElement.GetProperty("payload");
            Assert.Equal("initialize", payload.GetProperty("method").GetString());
            var initId = payload.GetProperty("id").GetInt32();
            using var initResponse = JsonDocument.Parse(
                "{\"jsonrpc\":\"2.0\",\"id\":" + initId + ",\"result\":{\"capabilities\":{\"tools\":{}}}}");
            bridge.HandleResponse(deviceId, initResponse.RootElement);
        }

        var listSent = await WaitForNthControlMessageAsync(publisher, index: 3);
        using (var listDoc = JsonDocument.Parse(listSent))
        {
            var payload = listDoc.RootElement.GetProperty("payload");
            Assert.Equal("tools/list", payload.GetProperty("method").GetString());
            var listId = payload.GetProperty("id").GetInt32();
            using var listResponse = JsonDocument.Parse(
                "{\"jsonrpc\":\"2.0\",\"id\":" + listId +
                ",\"result\":{\"tools\":[{\"name\":\"self.do_watering\",\"description\":\"Water the plant\"}]}}");
            bridge.HandleResponse(deviceId, listResponse.RootElement);
        }

        var secondAttempt = await secondAttemptTask;
        var tool = Assert.Single(secondAttempt);
        Assert.Equal("self.do_watering", tool.Name);
    }

    private static async Task<string> WaitForNthControlMessageAsync(RecordingVoicePublisher publisher, int index)
    {
        for (int i = 0; i < 200 && publisher.ControlMessages.Count < index; i++)
        {
            await Task.Delay(5);
        }
        return publisher.ControlMessages[index - 1];
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
