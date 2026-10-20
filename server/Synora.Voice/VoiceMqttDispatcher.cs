using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Synora.Protocol;

namespace Synora.Voice;

/// <summary>
/// Drives voice sessions over MQTT. Replaces the old WebSocket-based
/// <c>VoiceConnectionHandler</c>: there's no TCP-connection-open/close event
/// with MQTT like there was with a WebSocket accept/close, so a <c>hello</c>
/// message arriving on <c>voice/control/up</c> for a given device starts a
/// session, and sessions are kept in memory (<see cref="_sessions"/>) and
/// expired after <see cref="IdleTimeout"/> of inactivity (no control or audio
/// message) -- a deliberate, disclosed scope reduction versus proper MQTT
/// presence tracking (Last Will and Testament), which is future hardening
/// work.
///
/// The MQTT transport (subscribing to the <c>voice/control/up</c>/
/// <c>voice/audio/up</c> wildcard topics and routing each message here) lives
/// in Synora.Api's MQTT hosted service; this class knows the xiaozhi JSON
/// framing but nothing about MQTTnet.
/// </summary>
public sealed class VoiceMqttDispatcher : IDisposable
{
    private const int ServerSampleRate = 16000;
    private const int ServerFrameDurationMs = 60;

    // No control or audio message for this long -> the session is considered
    // abandoned and dropped. Picked as a reasonable MVP idle timeout; there's
    // no ack/keepalive channel to rely on instead.
    private static readonly TimeSpan IdleTimeout = TimeSpan.FromMinutes(3);
    private static readonly TimeSpan SweepInterval = TimeSpan.FromMinutes(1);

    private readonly IVoicePipeline _pipeline;
    private readonly McpBridge _mcpBridge;
    private readonly ILogger<VoiceMqttDispatcher> _logger;
    private readonly ConcurrentDictionary<Guid, SessionEntry> _sessions = new();
    private readonly Timer _idleSweepTimer;

    public VoiceMqttDispatcher(IVoicePipeline pipeline, McpBridge mcpBridge, ILogger<VoiceMqttDispatcher> logger)
    {
        _pipeline = pipeline;
        _mcpBridge = mcpBridge;
        _logger = logger;
        _idleSweepTimer = new Timer(_ => SweepIdleSessions(), null, SweepInterval, SweepInterval);
    }

    /// <summary>
    /// Handle one message published on <c>synora/devices/{deviceId}/voice/control/up</c>.
    /// A <c>hello</c> (re)starts the session for this device (constructing it
    /// with <paramref name="publisher"/> -- passed in by the caller rather than
    /// constructor-injected here, since this class's only implementation of
    /// <see cref="IVoicePublisher"/> is the very MQTT service that also has to
    /// route incoming messages to this dispatcher, which would otherwise be a
    /// circular DI dependency). Any other message requires an existing session
    /// and is dropped silently if there isn't one (matches the old handler's
    /// tolerance of malformed/out-of-order input).
    /// </summary>
    public async Task HandleControlAsync(Guid deviceId, ReadOnlyMemory<byte> payload, IVoicePublisher publisher, CancellationToken ct)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(payload);
        }
        catch (JsonException)
        {
            return;
        }

        using (doc)
        {
            var root = doc.RootElement;
            var type = ClientMessages.GetType(root);

            if (type == "hello")
            {
                var hello = ClientMessages.ParseHello(root);
                if (hello is null)
                {
                    return;
                }

                var sessionId = Guid.NewGuid().ToString("N");
                var session = new VoiceSession(
                    deviceId, sessionId, hello.Version, ServerSampleRate, ServerFrameDurationMs, publisher);
                _sessions[deviceId] = new SessionEntry(session);

                await session.SendJsonAsync(
                    ServerMessages.Hello(sessionId, ServerSampleRate, ServerFrameDurationMs), ct);
                await _pipeline.OnSessionStartAsync(session, ct);

                // Fire-and-forget: populates McpBridge's per-device cache in
                // the background so it's ready by the time this turn's
                // OnListenStopAsync needs it, without making the hello reply
                // wait on a device round trip. Only attempted if the device
                // actually advertised MCP support.
                if (hello.WantsMcp)
                {
                    _ = DiscoverToolsInBackgroundAsync(deviceId, session, ct);
                }
                return;
            }

            if (!_sessions.TryGetValue(deviceId, out var entry))
            {
                return;
            }
            entry.Touch();
            var session2 = entry.Session;

            switch (type)
            {
                case "listen":
                    switch (ClientMessages.GetString(root, "state"))
                    {
                        case "start":
                            await _pipeline.OnListenStartAsync(
                                session2, ClientMessages.GetString(root, "mode") ?? "auto", ct);
                            break;
                        case "stop":
                            await _pipeline.OnListenStopAsync(session2, ct);
                            break;
                        // "detect" (wake word) is followed by "start"; nothing to do here yet.
                    }
                    break;
                case "abort":
                    await _pipeline.OnAbortAsync(session2, ct);
                    break;
                case "mcp":
                    var mcpPayload = ClientMessages.GetMcpPayload(root);
                    if (mcpPayload is not null)
                    {
                        _mcpBridge.HandleResponse(deviceId, mcpPayload.Value);
                    }
                    break;
            }
        }
    }

    /// <summary>
    /// Handle one message published on <c>synora/devices/{deviceId}/voice/audio/up</c>
    /// (raw Opus, no framing). Dropped silently if there's no active session for
    /// this device (e.g. audio arriving before hello, or after the session idled out).
    /// </summary>
    public async Task HandleAudioAsync(Guid deviceId, ReadOnlyMemory<byte> opus, CancellationToken ct)
    {
        if (!_sessions.TryGetValue(deviceId, out var entry))
        {
            return;
        }
        entry.Touch();
        await _pipeline.OnAudioAsync(entry.Session, opus, ct);
    }

    /// <summary>
    /// Returns the currently live session for this device if one exists
    /// (reused as-is -- e.g. an in-progress real voice turn), or creates and
    /// registers a fresh ad-hoc one. For callers that need to reach a
    /// device's MCP tools outside of a real device-initiated "hello" turn
    /// (see the admin chat-test endpoint in Synora.Api's Program.cs).
    ///
    /// Registering the session in <see cref="_sessions"/> here (rather than
    /// just handing back a bare <c>new VoiceSession(...)</c>) isn't cosmetic:
    /// <see cref="HandleControlAsync"/>'s non-"hello" branches -- including
    /// the "mcp" case <see cref="McpBridge"/> depends on for tool
    /// discovery/call responses -- all gate on <c>_sessions.TryGetValue</c>
    /// first and silently drop the message otherwise. An unregistered
    /// session's outgoing MCP request reaches the device fine, but the
    /// device's reply then has nowhere to land and the call times out.
    /// Confirmed live: this exact bug, via the admin chat's ad-hoc tool
    /// calls, before this method existed.
    /// </summary>
    public VoiceSession GetOrCreateSession(Guid deviceId, IVoicePublisher publisher)
    {
        var entry = _sessions.GetOrAdd(deviceId, static (id, pub) => new SessionEntry(new VoiceSession(
            id, Guid.NewGuid().ToString("N"), protocolVersion: 2, ServerSampleRate, ServerFrameDurationMs, pub)), publisher);
        entry.Touch();
        return entry.Session;
    }

    private async Task DiscoverToolsInBackgroundAsync(Guid deviceId, VoiceSession session, CancellationToken ct)
    {
        try
        {
            await _mcpBridge.GetToolsAsync(deviceId, session, ct);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "MCP tool discovery failed for device {DeviceId}", deviceId);
        }
    }

    private void SweepIdleSessions()
    {
        var cutoff = DateTimeOffset.UtcNow - IdleTimeout;
        foreach (var (deviceId, entry) in _sessions)
        {
            if (entry.LastActivity < cutoff && _sessions.TryRemove(deviceId, out _))
            {
                _logger.LogInformation(
                    "Voice session for device {DeviceId} expired after {Timeout} idle", deviceId, IdleTimeout);
            }
        }
    }

    public void Dispose() => _idleSweepTimer.Dispose();

    private sealed class SessionEntry(VoiceSession session)
    {
        public VoiceSession Session { get; } = session;
        public DateTimeOffset LastActivity { get; private set; } = DateTimeOffset.UtcNow;
        public void Touch() => LastActivity = DateTimeOffset.UtcNow;
    }
}
