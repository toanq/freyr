using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging.Abstractions;
using Synora.Voice;
using Xunit;

namespace Synora.Api.Tests;

// The voice channel moved from a WebSocket /v1/ endpoint to MQTT
// (synora/devices/{id}/voice/control|audio/up|down -- see MqttService in
// Synora.Api and VoiceMqttDispatcher in Synora.Voice). A full broker-backed
// integration test would need a real Mosquitto instance; instead, this
// exercises VoiceMqttDispatcher directly -- the exact seam MqttService calls
// with (deviceId, payload) once it has parsed a topic -- with a recording
// fake IVoicePublisher standing in for "publish to the down topics". This
// covers the same message-handling behavior the old WS test did (hello
// handshake, then a listen turn round-tripping stt/tts/audio) without
// needing a live broker.
public class VoiceEndpointTests
{
    [Fact]
    public async Task Hello_Then_Listen_Loopback_RoundTrips_Every_Message_Type()
    {
        using var dispatcher = new VoiceMqttDispatcher(
            new LoopbackPipeline(), new McpBridge(NullLogger<McpBridge>.Instance), NullLogger<VoiceMqttDispatcher>.Instance);
        var publisher = new RecordingVoicePublisher();
        var deviceId = Guid.NewGuid();
        var ct = CancellationToken.None;

        // 1. Device hello -> server hello with a session id, published on voice/control/down.
        await dispatcher.HandleControlAsync(
            deviceId,
            Utf8("""{"type":"hello","version":2,"transport":"websocket","features":{"mcp":true},"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}"""),
            publisher, ct);

        // Not necessarily the only message: the hello also kicks off MCP tool
        // discovery in the background (features.mcp=true above), which may
        // publish its own "mcp" initialize request on this same channel
        // before or after this assertion runs -- find the hello reply
        // specifically rather than assuming it's alone.
        var hello = Assert.Single(publisher.ControlMessages, msg => JsonDocument.Parse(msg).RootElement.GetProperty("type").GetString() == "hello");
        using (var doc = JsonDocument.Parse(hello))
        {
            Assert.Equal("hello", doc.RootElement.GetProperty("type").GetString());
            Assert.False(string.IsNullOrWhiteSpace(doc.RootElement.GetProperty("session_id").GetString()));
        }
        publisher.ControlMessages.Clear();

        // 2. A listen turn: start -> one Opus frame (on voice/audio/up) -> stop.
        await dispatcher.HandleControlAsync(deviceId, Utf8("""{"type":"listen","state":"start","mode":"manual"}"""), publisher, ct);

        var opus = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
        await dispatcher.HandleAudioAsync(deviceId, opus, ct);

        await dispatcher.HandleControlAsync(deviceId, Utf8("""{"type":"listen","state":"stop"}"""), publisher, ct);

        // 3. The loopback response should exercise stt, tts start, the echoed
        //    audio (on voice/audio/down), and tts stop.
        bool gotStt = false, gotTtsStart = false, gotTtsStop = false;
        foreach (var msg in publisher.ControlMessages)
        {
            using var doc = JsonDocument.Parse(msg);
            var type = doc.RootElement.GetProperty("type").GetString();
            if (type == "stt") gotStt = true;
            if (type == "tts")
            {
                var state = doc.RootElement.GetProperty("state").GetString();
                if (state == "start") gotTtsStart = true;
                if (state == "stop") gotTtsStop = true;
            }
        }

        Assert.True(gotStt, "expected an stt message");
        Assert.True(gotTtsStart, "expected a tts start message");
        Assert.True(gotTtsStop, "expected a tts stop message");

        var echoed = Assert.Single(publisher.AudioMessages);
        Assert.Equal(opus, echoed);
    }

    [Fact]
    public async Task Control_Or_Audio_Before_Hello_Is_Dropped_Silently()
    {
        using var dispatcher = new VoiceMqttDispatcher(
            new LoopbackPipeline(), new McpBridge(NullLogger<McpBridge>.Instance), NullLogger<VoiceMqttDispatcher>.Instance);
        var publisher = new RecordingVoicePublisher();
        var deviceId = Guid.NewGuid();

        // No hello yet for this device -- listen/abort/audio should all be
        // silently ignored (no session to attach them to), matching the old
        // handler's tolerance of out-of-order input.
        await dispatcher.HandleControlAsync(deviceId, Utf8("""{"type":"listen","state":"start"}"""), publisher, CancellationToken.None);
        await dispatcher.HandleAudioAsync(deviceId, new byte[] { 9, 9, 9 }, CancellationToken.None);

        Assert.Empty(publisher.ControlMessages);
        Assert.Empty(publisher.AudioMessages);
    }

    private static byte[] Utf8(string s) => Encoding.UTF8.GetBytes(s);

    private sealed class RecordingVoicePublisher : IVoicePublisher
    {
        public List<string> ControlMessages { get; } = new();
        public List<byte[]> AudioMessages { get; } = new();

        public Task PublishControlAsync(Guid deviceId, string json, CancellationToken ct)
        {
            ControlMessages.Add(json);
            return Task.CompletedTask;
        }

        public Task PublishAudioAsync(Guid deviceId, ReadOnlyMemory<byte> opus, CancellationToken ct)
        {
            AudioMessages.Add(opus.ToArray());
            return Task.CompletedTask;
        }
    }
}
