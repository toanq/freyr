#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_codec.h"

// Click-to-talk voice session: BOOT-button click starts/stops capturing mic
// audio, Opus-encoded and published to the server over the MQTT voice
// topics (synora/devices/{deviceId}/voice/...), matching Synora.Voice's
// VoiceMqttDispatcher. Response audio (also Opus, also over MQTT) is decoded
// and played back through the same AudioCodec. Drives SynoraDisplay's eyes
// (idle/listening/talking) as a side effect of session state, but has no
// display dependency of its own beyond that.
class SynoraVoiceSession {
public:
    static SynoraVoiceSession& GetInstance();

    // codec must already be Start()ed with input+output enabled.
    void Init(AudioCodec* codec);

    // BOOT-button click: idle -> start listening, listening -> stop
    // (server then sends its reply). No-op if MQTT isn't connected yet
    // (unpaired/no device id). Starting is also a no-op while the device is
    // server-muted (SynoraMqtt::IsMuted) -- stopping an already-listening
    // session is still allowed regardless.
    void Toggle();

    // Called for every chunk the audio input task reads from the codec,
    // regardless of session state -- a no-op unless currently listening.
    void FeedMic(const std::vector<int16_t>& pcm);

    // Registered as MQTT subscription callbacks (see SynoraMqtt::EnsureStarted).
    void OnControlDown(const std::string& payload);
    void OnAudioDown(const std::string& payload);

    SynoraVoiceSession(const SynoraVoiceSession&) = delete;
    SynoraVoiceSession& operator=(const SynoraVoiceSession&) = delete;

private:
    SynoraVoiceSession() = default;
    ~SynoraVoiceSession() = default;

    void StartListening(bool is_relisten);
    void StopListening();
    void CancelListeningLocally();
    void EncodeAndPublish(const int16_t* pcm, size_t samples);

    AudioCodec* codec_ = nullptr;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    int encoder_frame_samples_ = 0;   // input frame size, in samples
    int encoder_out_buf_size_ = 0;    // recommended encoded-output buffer size
    std::vector<uint8_t> encode_out_buf_;
    std::vector<uint8_t> decode_out_buf_;

    std::mutex mutex_;
    bool listening_ = false;
    std::vector<int16_t> pcm_accum_; // buffers FeedMic() calls up to one encoder frame

    // Silence timeout (see kListenSilenceTimeoutMs/kRelistenSilenceTimeoutMs
    // in the .cc): if the user doesn't say anything within that turn's
    // timeout, FeedMic() cancels it locally rather than round-tripping the
    // server for silence. Only matters for a turn that hasn't heard any
    // speech yet -- once speech_detected_ flips true, the turn ends the
    // normal way (a button click), same as before this feature existed.
    int64_t listening_started_ms_ = 0;
    bool speech_detected_ = false;
    bool is_relisten_ = false; // which timeout FeedMic() applies -- set by StartListening()'s caller
};
