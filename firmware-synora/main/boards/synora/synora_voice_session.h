#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "audio/audio_codec.h"

// Hybrid press-to-talk / local-auto-stop voice session: BOOT-button click
// starts capturing mic audio, Opus-encoded and published to the server over
// the MQTT voice topics (synora/devices/{deviceId}/voice/...), matching
// Synora.Voice's VoiceMqttDispatcher. Response audio (also Opus, also over
// MQTT) is decoded and played back through the same AudioCodec.
//
// A recording ends either on a second button press OR on its own once
// FeedMic() sees a sustained silence (kSilenceLevelThreshold/
// kSilenceHangoverMs in the .cc) -- this replaced a purely-manual design
// after a standalone test app (test_apps/vad_playback_test/) validated that
// a level-threshold fallback is reliable on this hardware, where esp-sr's
// own AFE VAD alone was not (see that app's main.cc for the investigation).
//
// A mic-level barge-in during a reply (interrupting playback to start a new
// turn) was also ported from that test app, then reverted the same day:
// confirmed live that at this device's actual playback volume (maxed to 100
// in main.cc for speaker-hardware debugging), the speaker's own output
// leaks into the mic loud enough to cross any workable threshold on its
// own, self-interrupting every reply almost immediately. Would need real
// echo cancellation on this specific path (the AEC reference channel only
// feeds the wake-word engine today) to do properly -- not attempted yet.
// Toggle() also deliberately does NOT let a manual button press interrupt a
// reply either -- confirmed live before that a spurious press there (line
// noise, or a genuine double-press) starting a fresh session while the
// previous reply's audio was still mid-flight caused the device to seem to
// "keep talking after going back to the dashboard"; see Toggle()'s comment.
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
    // While listening, encodes+publishes it and runs the local
    // silence-threshold auto-stop.
    void FeedMic(const std::vector<int16_t>& pcm);

    // Registered as MQTT subscription callbacks (see SynoraMqtt::EnsureStarted).
    void OnControlDown(const std::string& payload);
    void OnAudioDown(const std::string& payload);

    SynoraVoiceSession(const SynoraVoiceSession&) = delete;
    SynoraVoiceSession& operator=(const SynoraVoiceSession&) = delete;

    // Mic level (0.0-1.0, see ComputeAudioLevel in the .cc) at or above which
    // a chunk counts as speech for the local auto-stop -- calibration notes
    // live next to its uses in the .cc. Public so test_console's mictest
    // reports against the same number.
    static constexpr float kSilenceLevelThreshold = 0.25f;

private:
    SynoraVoiceSession() = default;
    ~SynoraVoiceSession() = default;

    void StartListening();
    void StopListening();
    void CancelListeningLocally();
    void EncodeAndPublish(const int16_t* pcm, size_t samples);

    static void PlaybackTaskTrampoline(void* arg);
    void PlaybackTaskLoop();
    void DecodeAndPlay(const std::string& opus_payload);
    void MaybeReturnToIdle();
    void ReturnToIdleIfStillDue();

    AudioCodec* codec_ = nullptr;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    int encoder_frame_samples_ = 0;   // input frame size, in samples
    int encoder_out_buf_size_ = 0;    // recommended encoded-output buffer size
    std::vector<uint8_t> encode_out_buf_;
    std::vector<uint8_t> decode_out_buf_;

    // audio/down handling runs on esp-mqtt's own mqtt_task (see OnAudioDown's
    // doc comment) -- decode + the blocking I2S write happen instead on this
    // dedicated task, fed through a queue of heap-allocated opus payloads
    // (owned by whichever side currently holds the pointer: OnAudioDown until
    // it's queued, PlaybackTaskLoop from there on). A nullptr entry is a
    // "check for idle transition" wake-up rather than a real payload (see
    // MaybeReturnToIdle()).
    QueueHandle_t playback_queue_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    StackType_t* playback_task_stack_ = nullptr;
    StaticTask_t* playback_task_buffer_ = nullptr;
    // Confirmed live (2026-09-22): the server delivers a whole reply's Opus
    // chunks in a fast MQTT burst rather than pacing them to real-time, so
    // the old 8-deep queue (~480ms of slack) filled up almost instantly --
    // only the first ~8 chunks of a reply ever got queued/played, the rest
    // silently dropped as "Playback queue full", making replies sound cut
    // off after a fraction of a second regardless of how long the actual
    // reply was. 400 chunks is ~24s of audio -- comfortably more than any
    // reply should run, at a trivial memory cost (the queue itself just
    // holds pointers; the real cost is however many heap-allocated Opus
    // payloads are actually in flight at once, unchanged by this bump).
    static constexpr size_t kMaxPlaybackQueueDepth = 400;
    std::atomic<int> pending_playback_chunks_{0};
    std::atomic<bool> tts_stop_received_{false};

    // Post-reply idle transition (kPostReplyIdleDelayMs), driven by
    // PlaybackTaskLoop()'s timed xQueueReceive instead of a blocking sleep so
    // a new turn's audio isn't held up behind it. Playback-task-only state.
    int64_t idle_due_us_ = 0;      // esp_timer_get_time() deadline, 0 = none pending
    uint32_t idle_due_turn_ = 0;   // turn_id_ when scheduled; skipped if a new turn started since

    std::mutex mutex_;
    bool listening_ = false;
    uint32_t turn_id_ = 0; // bumped by StartListening()
    std::vector<int16_t> pcm_accum_; // buffers FeedMic() calls up to one encoder frame

    // Local silence-threshold auto-stop state, reset by StartListening() at
    // the top of every turn -- had_speech_ gates the hangover exactly like
    // vad_playback_test's g_had_speech does, so a turn that's just quiet from
    // the start (user hasn't started talking yet) never auto-stops itself.
    bool had_speech_ = false;
    int64_t silence_started_us_ = 0;   // 0 = not currently in a below-threshold run
    int64_t listening_started_us_ = 0; // for the safety-timeout cap (kMaxListeningMs)

    // True from StopListening() until the reply's tts:stop actually arrives
    // (OnControlDown()) -- Toggle() refuses to start a new turn while this is
    // true, so a second press (or a spurious/phantom one) can't start
    // listening on top of a reply that's still streaming/playing.
    bool awaiting_reply_ = false;
    // When awaiting_reply_ was set -- if tts:stop never arrives (server
    // crash/timeout/lost connection), Toggle() gives up waiting after
    // kMaxAwaitingReplyMs (see .cc) rather than leaving the button
    // permanently unresponsive.
    int64_t awaiting_reply_since_ms_ = 0;
};
