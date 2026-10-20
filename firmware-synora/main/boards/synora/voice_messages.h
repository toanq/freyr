#pragma once

#include <string>

// Pure JSON message building/parsing for the MQTT voice control topics --
// no ESP-IDF/hardware dependency (cJSON itself is portable C), kept separate
// from synora_voice_session.cc specifically so it can be exercised by a
// plain host unit test (see firmware-synora/tests/). Message shapes match
// the server's Synora.Protocol/ProtocolMessages.cs exactly.

// {"type":"hello","version":2,"transport":"mqtt","features":{"mcp":true},"audio_params":{"format":"opus","sample_rate":...,"channels":1,"frame_duration":...}}
std::string BuildHelloMessage(int sample_rate_hz, int frame_duration_ms);

// {"type":"listen","state":"start","mode":"manual"} or {"type":"listen","state":"stop"}
std::string BuildListenMessage(bool start);

// True if payload is a {"type":"tts","state":"stop",...} control message --
// the signal SynoraVoiceSession uses to flip the eyes back to idle.
bool IsTtsStopMessage(const std::string& payload);

// True if payload is a {"type":"mcp","payload":{...}} control message --
// the signal SynoraVoiceSession uses to forward the inner JSON-RPC payload
// to McpServer::ParseMessage.
bool IsMcpMessage(const std::string& payload);

// If payload is a {"type":"tts","state":"sentence_start","text":"..."}
// control message, fills out_text with its "text" field and returns true --
// SynoraVoiceSession shows this under the eyes as a subtitle for the reply.
bool TryGetTtsSentenceText(const std::string& payload, std::string& out_text);
