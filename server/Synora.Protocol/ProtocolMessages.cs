using System.Text.Json;
using System.Text.Json.Serialization;

namespace Synora.Protocol;

/// <summary>
/// JSON control messages for the xiaozhi WebSocket protocol. The device sends
/// <c>hello</c>/<c>listen</c>/<c>abort</c>/<c>mcp</c>; the server replies with
/// <c>hello</c> then streams <c>tts</c>/<c>stt</c>/<c>llm</c>/<c>mcp</c> control
/// alongside binary Opus. Keys are snake_case to match the firmware's cJSON
/// field names exactly.
/// </summary>
public static class ServerMessages
{
    private static readonly JsonSerializerOptions Options = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    /// <summary>Server hello — answers the device hello and opens the audio channel.</summary>
    public static string Hello(string sessionId, int sampleRate, int frameDuration) =>
        JsonSerializer.Serialize(new
        {
            type = "hello",
            transport = "websocket",
            session_id = sessionId,
            audio_params = new { sample_rate = sampleRate, frame_duration = frameDuration },
        }, Options);

    /// <summary>TTS lifecycle: state = "start" | "stop" | "sentence_start" (text set for sentence_start).</summary>
    public static string Tts(string state, string? sessionId = null, string? text = null) =>
        JsonSerializer.Serialize(new { type = "tts", session_id = sessionId, state, text }, Options);

    /// <summary>Recognized user speech shown on the device as the "user" chat line.</summary>
    public static string Stt(string text, string? sessionId = null) =>
        JsonSerializer.Serialize(new { type = "stt", session_id = sessionId, text }, Options);

    /// <summary>LLM-driven expression/emotion for the device face.</summary>
    public static string Llm(string emotion, string? sessionId = null) =>
        JsonSerializer.Serialize(new { type = "llm", session_id = sessionId, emotion }, Options);

    /// <summary>MCP tool-call passthrough (payload is a raw JSON object).</summary>
    public static string Mcp(JsonElement payload, string? sessionId = null) =>
        JsonSerializer.Serialize(new { type = "mcp", session_id = sessionId, payload }, Options);

    /// <summary>System command (currently the device only acts on "reboot").</summary>
    public static string System(string command) =>
        JsonSerializer.Serialize(new { type = "system", command }, Options);

    /// <summary>Alert banner + sound on the device (all three fields required by firmware).</summary>
    public static string Alert(string status, string message, string emotion) =>
        JsonSerializer.Serialize(new { type = "alert", status, message, emotion }, Options);
}

/// <summary>Audio parameters exchanged in the hello handshake.</summary>
public sealed record AudioParams(string Format, int SampleRate, int Channels, int FrameDuration);

/// <summary>Parsed device <c>hello</c> message.</summary>
public sealed record ClientHello(int Version, AudioParams? Audio, bool WantsMcp, bool WantsAec);

/// <summary>
/// One MCP tool as declared by the device's <c>tools/list</c> response --
/// <see cref="InputSchema"/> is the raw JSON Schema object the device sent,
/// passed straight through to the LLM's function-calling "parameters" field
/// (see McpBridge/DeepSeekLlmClient in Synora.Voice) without reinterpreting it
/// server-side.
/// </summary>
public sealed record McpToolDefinition(string Name, string Description, JsonElement InputSchema);

/// <summary>Parsing helpers for device → server messages (best-effort, null-tolerant).</summary>
public static class ClientMessages
{
    /// <summary>Read the "type" discriminator, or null if absent.</summary>
    public static string? GetType(JsonElement root) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty("type", out var t) && t.ValueKind == JsonValueKind.String
            ? t.GetString()
            : null;

    /// <summary>Read a string property, or null.</summary>
    public static string? GetString(JsonElement root, string name) =>
        root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    /// <summary>
    /// Extract the inner JSON-RPC object from a device <c>{"type":"mcp","payload":{...}}</c>
    /// message, or null if this isn't a well-formed mcp message.
    /// </summary>
    public static JsonElement? GetMcpPayload(JsonElement root) =>
        GetType(root) == "mcp" && root.TryGetProperty("payload", out var p) && p.ValueKind == JsonValueKind.Object
            ? p
            : null;

    /// <summary>Parse a device <c>hello</c>. Returns null if it isn't a well-formed hello.</summary>
    public static ClientHello? ParseHello(JsonElement root)
    {
        if (GetType(root) != "hello") return null;

        int version = root.TryGetProperty("version", out var v) && v.ValueKind == JsonValueKind.Number ? v.GetInt32() : 1;

        AudioParams? audio = null;
        if (root.TryGetProperty("audio_params", out var ap) && ap.ValueKind == JsonValueKind.Object)
        {
            audio = new AudioParams(
                Format: GetString(ap, "format") ?? "opus",
                SampleRate: ap.TryGetProperty("sample_rate", out var sr) && sr.ValueKind == JsonValueKind.Number ? sr.GetInt32() : 16000,
                Channels: ap.TryGetProperty("channels", out var ch) && ch.ValueKind == JsonValueKind.Number ? ch.GetInt32() : 1,
                FrameDuration: ap.TryGetProperty("frame_duration", out var fd) && fd.ValueKind == JsonValueKind.Number ? fd.GetInt32() : 60);
        }

        bool mcp = false, aec = false;
        if (root.TryGetProperty("features", out var f) && f.ValueKind == JsonValueKind.Object)
        {
            mcp = f.TryGetProperty("mcp", out var m) && m.ValueKind == JsonValueKind.True;
            aec = f.TryGetProperty("aec", out var a) && a.ValueKind == JsonValueKind.True;
        }

        return new ClientHello(version, audio, mcp, aec);
    }
}
