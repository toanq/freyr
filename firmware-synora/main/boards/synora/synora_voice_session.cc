#include "synora_voice_session.h"

#include "boards/common/synora_mqtt.h"
#include "mcp_server.h"
#include "synora_display.h"
#include "voice_messages.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

static const char* TAG = "SynoraVoiceSession";

namespace {
// Fixed to match the server's audio_params (Synora.Voice's VoiceSession):
// 16kHz mono, 60ms frames, Opus. config.h sets both codec sample rates to
// 16000 for the same reason -- no resampler needed on either leg.
constexpr int kSampleRateHz = 16000;
constexpr int kFrameDurationMs = 60;

// See awaiting_reply_since_ms_'s doc comment -- generous (LLM+TTS can
// legitimately take several seconds) but not infinite.
constexpr int64_t kMaxAwaitingReplyMs = 20000;

// AUDIO_CODEC_DMA_DESC_NUM(6) * AUDIO_CODEC_DMA_FRAME_NUM(240) samples can be
// sitting in the I2S DMA queue after the last Write() call returns -- that
// call only blocks until the data is handed to DMA, not until it's actually
// been played out through the speaker (~90ms @16kHz). MaybeReturnToIdle()
// waits this long after the last audio/down chunk before letting the device
// go idle, so a fast re-press of the button can't start recording while the
// tail of the reply is still physically audible (confirmed live: doing so
// puts an audible echo of the reply's last word at the start of the next
// recording).
constexpr int kPlaybackDrainSettleMs = 120;

// After a reply finishes playing, stay in the kTalking display state (rather
// than snapping straight back to idle) for this long -- gives a beat for a
// natural follow-up before the eyes/subtitle reset, instead of feeling like
// the device stopped paying attention the instant it finished speaking.
constexpr int kPostReplyIdleDelayMs = 10000;

// Rough calibration, not derived from any spec -- normal speech into a
// phone/laptop mic typically RMSes in the low thousands out of int16's
// +-32768 range. Tune on real hardware if the eyes barely move or maxes out
// on every whisper. Only feeds a cosmetic animation, so it doesn't need to
// be more precise than "roughly tracks how loud this sounds".
constexpr float kAudioLevelReferenceRms = 6000.0f;

float ComputeAudioLevel(const int16_t* pcm, size_t samples) {
    if (samples == 0) {
        return 0.0f;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t s = pcm[i];
        sum_sq += s * s;
    }
    float rms = std::sqrt(static_cast<float>(sum_sq) / static_cast<float>(samples));
    return std::clamp(rms / kAudioLevelReferenceRms, 0.0f, 1.0f);
}

// Local silence-threshold auto-stop, replacing pure manual press-to-talk --
// originally calibrated (0.1) against the standalone vad_playback_test app
// (test_apps/vad_playback_test/main/main.cc) in a different, quieter room. A
// manual second press still works too (Toggle() -> StopListening()); this is
// a dual-path exactly like that test app, not a replacement for the button.
//
// Re-calibrated (2026-09-22) against this device's actual deployment room,
// which is a known noisy environment (fans/machine room -- see
// project_synora_board memory's mic-noise investigation): live level=
// readings while silent repeatedly spiked to 0.11-0.145, well above the old
// 0.1 threshold, resetting the silence timer roughly every 200-900ms and
// never leaving a clean kSilenceHangoverMs-long gap -- the recording only
// ever ended via kMaxListeningMs's 20s safety cap, or by chance after
// 10-15s once ambient noise happened to have one lucky quiet enough
// stretch. 0.25 sits with real margin above that observed ~0.145 ambient
// ceiling while still comfortably below real speech (peaks of 0.7-0.84 seen
// in the original test app's room). The value itself is
// SynoraVoiceSession::kSilenceLevelThreshold (header), shared with mictest.
constexpr int kSilenceHangoverMs = 3000;
// Safety cap in case the threshold above somehow never triggers (e.g.
// continuous loud background noise never drops below it) -- manual mode
// never needed one of these since a human decided when to stop, but an
// auto-stop-driven recording needs its own bound. Generous -- real
// utterances should end well before this.
constexpr int kMaxListeningMs = 20000;

// NOT porting vad_playback_test's flat 1.5x playback-gain fix here, on
// closer look: that was calibrated against the test app's default
// AudioCodec::output_volume_ (70/100). main.cc's app_main() already calls
// codec_if.SetOutputVolume(100) -- max, "for the speaker hardware
// debugging" -- so this path is already at unity gain (no headroom left in
// AudioCodec::Write()'s own volume_factor). Layering another 1.5x flat
// multiplier on top of an already-unity signal would clip real speech much
// more aggressively than it did at the test app's 70/100, distorting
// playback rather than fixing loudness. If the real device still sounds
// quiet, the remaining lever is the MAX98357A's GAIN pin (floating = 9dB
// per config.h's wiring notes) -- analog headroom software can't add once
// the digital path is already maxed.

}  // namespace

SynoraVoiceSession& SynoraVoiceSession::GetInstance() {
    static SynoraVoiceSession instance;
    return instance;
}

void SynoraVoiceSession::Init(AudioCodec* codec) {
    codec_ = codec;

    esp_opus_enc_config_t enc_cfg = {};
    enc_cfg.sample_rate = kSampleRateHz;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc_cfg.complexity = 0;
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = true;
    enc_cfg.enable_vbr = true;
    auto err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed: %d", err);
        return;
    }
    esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_samples_, &encoder_out_buf_size_);
    encoder_frame_samples_ /= sizeof(int16_t); // API returns bytes; we buffer in samples.
    encode_out_buf_.resize(encoder_out_buf_size_);

    esp_opus_dec_cfg_t dec_cfg = {};
    dec_cfg.sample_rate = kSampleRateHz;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;
    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "esp_opus_dec_open failed: %d", err);
        return;
    }
    // One 60ms/16kHz mono frame, generous headroom for a slightly larger
    // decoded frame (encoder frame duration was negotiated at open time, so
    // this shouldn't vary in practice).
    decode_out_buf_.resize(kSampleRateHz / 1000 * kFrameDurationMs * sizeof(int16_t) * 2);

    // Holds pointers (heap-allocated std::string*, or nullptr as an
    // idle-check wake-up -- see PlaybackTaskLoop()), not the payload bytes
    // themselves.
    playback_queue_ = xQueueCreate(kMaxPlaybackQueueDepth, sizeof(std::string*));
    assert(playback_queue_ != nullptr);
    // PSRAM-backed stack: decode (esp_opus_dec) + the I2S write is the same
    // kind of stack cost that forced audio_input_task's stack into PSRAM in
    // main.cc -- same fix, applied up front rather than after a live crash.
    constexpr size_t kPlaybackTaskStackSize = 4096 * 4;
    playback_task_stack_ = static_cast<StackType_t*>(heap_caps_malloc(kPlaybackTaskStackSize, MALLOC_CAP_SPIRAM));
    playback_task_buffer_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    assert(playback_task_stack_ != nullptr && playback_task_buffer_ != nullptr);
    playback_task_ = xTaskCreateStatic(&SynoraVoiceSession::PlaybackTaskTrampoline, "voice_playback",
        kPlaybackTaskStackSize, this, 4, playback_task_stack_, playback_task_buffer_);
    assert(playback_task_ != nullptr);

    ESP_LOGI(TAG, "Ready: encoder frame = %d samples, decoder out buf = %u bytes",
             encoder_frame_samples_, static_cast<unsigned>(decode_out_buf_.size()));
}

// Press starts a turn; a second press OR FeedMic()'s own silence-threshold
// auto-stop ends it (see kSilenceLevelThreshold's comment) -- not xiaozhi's
// default "AutoStop" mode (server-side streaming ASR deciding end-of-turn on
// its own), which would need real server-side streaming/endpointing work our
// pipeline doesn't have yet (it's currently batch: STT only starts once
// "listen stop" arrives). The threshold logic is purely local/client-side,
// layered on top of the same manual start/stop plumbing rather than
// replacing it.
void SynoraVoiceSession::Toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (listening_) {
        StopListening();
    } else if (awaiting_reply_ &&
               (esp_timer_get_time() / 1000 - awaiting_reply_since_ms_) < kMaxAwaitingReplyMs) {
        // A reply is still streaming/playing (StopListening() already fired,
        // tts:stop hasn't arrived yet) -- ignore rather than starting a new
        // turn on top of it. Confirmed live this matters: a second press (or
        // a spurious one, e.g. button-line noise) landing in this window
        // otherwise starts a fresh "hello"/session while the previous
        // reply's audio is still mid-flight, which is exactly what made the
        // device seem to "keep talking after going back to the dashboard".
        ESP_LOGI(TAG, "Still awaiting/playing previous reply -- ignoring press");
    } else if (!SynoraMqtt::GetInstance().IsMuted()) {
        if (awaiting_reply_) {
            ESP_LOGW(TAG, "tts:stop never arrived within %lld ms -- giving up waiting",
                     static_cast<long long>(kMaxAwaitingReplyMs));
            awaiting_reply_ = false;
        }
        StartListening();
    }
}

// mutex_ already held by the caller (Toggle()).
void SynoraVoiceSession::StartListening() {
    pcm_accum_.clear();
    listening_ = true;
    turn_id_++;
    had_speech_ = false;
    silence_started_us_ = 0;
    listening_started_us_ = esp_timer_get_time();

    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildHelloMessage(kSampleRateHz, kFrameDurationMs), /*qos=*/1);
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/true), 1);
    SynoraDisplay::GetInstance().SetState(EyesState::kListening);
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f); // fresh session starts quiet, FeedMic() updates it immediately
    SynoraDisplay::GetInstance().SetSubtitle("Đang nghe...");
    ESP_LOGI(TAG, "Listening");
}

// mutex_ already held by the caller (Toggle()/FeedMic()).
void SynoraVoiceSession::StopListening() {
    listening_ = false;
    awaiting_reply_ = true;
    awaiting_reply_since_ms_ = esp_timer_get_time() / 1000;
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/false), 1);
    // The server's reply (stt/llm/tts + audio/down) is expected imminently;
    // OnControlDown() switches back to idle once tts:stop actually arrives.
    SynoraDisplay::GetInstance().SetState(EyesState::kTalking);
    // Reset rather than leave the last mic level showing -- there's a real
    // silence gap here until the first decoded reply chunk arrives and
    // starts driving the level itself.
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f);
    // "Đang xử lý..." (processing) rather than blank -- there's a real gap
    // here until the server's reply text/audio starts arriving, and leaving
    // the subtitle empty in the meantime looked like the button press hadn't
    // registered. OnControlDown() overwrites this with the actual reply text
    // once the first tts:sentence_start arrives.
    SynoraDisplay::GetInstance().SetSubtitle("Đang xử lý...");
    ESP_LOGI(TAG, "Stopped listening, awaiting reply");
}

void SynoraVoiceSession::FeedMic(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    float level = ComputeAudioLevel(pcm.data(), pcm.size());

    if (!listening_) {
        // Barge-in was here and got reverted (2026-09-22): confirmed live
        // that at this device's actual playback volume (main.cc maxes it to
        // 100 for speaker-hardware debugging), the speaker's own output
        // leaks into the mic loud enough to cross kBargeInLevelThreshold on
        // its own -- "Barge-in: level 0.53 > 0.50" fired ~2.6s into a
        // reply's own playback, self-interrupting every reply almost
        // immediately. This check has no AEC applied to it (the AEC
        // reference channel only feeds the wake-word engine, not this level
        // check), so there's no threshold that's both safely above this
        // device's own leaked audio and still low enough to catch real
        // speech -- would need real echo cancellation on this path to do
        // properly, not just a bigger number.
        return;
    }
    if (opus_encoder_ == nullptr) {
        return;
    }
    if (SynoraMqtt::GetInstance().IsMuted()) {
        // Muted mid-turn (server toggle raced the button click) -- cancel
        // right away rather than sending audio nobody will hear a reply to.
        ESP_LOGI(TAG, "Muted mid-session -- cancelling locally");
        CancelListeningLocally();
        return;
    }
    SynoraDisplay::GetInstance().SetAudioLevel(level);

    // Local silence-threshold auto-stop -- see kSilenceLevelThreshold's
    // comment. A manual press (Toggle() -> StopListening()) still works too;
    // whichever fires first wins, same as vad_playback_test's dual-path.
    int64_t now_us = esp_timer_get_time();
    if (level >= kSilenceLevelThreshold) {
        had_speech_ = true;
        silence_started_us_ = 0;
    } else if (had_speech_) {
        if (silence_started_us_ == 0) {
            silence_started_us_ = now_us;
        } else if (now_us - silence_started_us_ >= static_cast<int64_t>(kSilenceHangoverMs) * 1000) {
            ESP_LOGI(TAG, "Silence threshold: %dms below %.2f -- stopping", kSilenceHangoverMs,
                     kSilenceLevelThreshold);
            StopListening();
            return;  // listening_ is now false -- nothing left to encode this call
        }
    }
    if (now_us - listening_started_us_ >= static_cast<int64_t>(kMaxListeningMs) * 1000) {
        ESP_LOGW(TAG, "Safety timeout (%dms) -- no silence detected, stopping anyway", kMaxListeningMs);
        StopListening();
        return;
    }

    pcm_accum_.insert(pcm_accum_.end(), pcm.begin(), pcm.end());
    while (static_cast<int>(pcm_accum_.size()) >= encoder_frame_samples_) {
        EncodeAndPublish(pcm_accum_.data(), encoder_frame_samples_);
        pcm_accum_.erase(pcm_accum_.begin(), pcm_accum_.begin() + encoder_frame_samples_);
    }
}

// mutex_ already held by the caller (FeedMic()) -- ends a turn with no
// server round-trip expected (unlike StopListening(), which sets kTalking
// and awaits a reply). Only the mid-turn-mute case reaches this now.
void SynoraVoiceSession::CancelListeningLocally() {
    listening_ = false;
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/false), 1);
    SynoraDisplay::GetInstance().SetState(EyesState::kIdle);
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f);
    SynoraDisplay::GetInstance().SetSubtitle("");
}

// mutex_ already held by the caller (FeedMic()).
void SynoraVoiceSession::EncodeAndPublish(const int16_t* pcm, size_t samples) {
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
    in.len = samples * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = encode_out_buf_.data();
    out.len = encode_out_buf_.size();

    auto err = esp_opus_enc_process(opus_encoder_, &in, &out);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "Opus encode failed: %d", err);
        return;
    }
    SynoraMqtt::GetInstance().Publish(
        "voice/audio/up", std::string(reinterpret_cast<char*>(out.buffer), out.encoded_bytes), /*qos=*/0);
}

void SynoraVoiceSession::OnControlDown(const std::string& payload) {
    if (IsTtsStopMessage(payload)) {
        // Don't flip to idle here -- MaybeReturnToIdle() (run from the
        // playback task) does that once every queued audio/down chunk has
        // actually finished playing, so the next StartListening() can't
        // start recording over the tail of this reply. tts:stop is
        // guaranteed to reach OnControlDown() after every preceding
        // audio/down's OnAudioDown() call (same mqtt_task, in order), so
        // pending_playback_chunks_ already reflects this whole reply.
        tts_stop_received_.store(true);
        if (playback_queue_ != nullptr) {
            std::string* wake_up = nullptr;  // not a payload -- see PlaybackTaskLoop()
            xQueueSend(playback_queue_, &wake_up, 0);
        }
    } else if (IsMcpMessage(payload)) {
        // The server's MCP bridge (initialize/tools/list/tools/call) --
        // unwrap the outer {"type":"mcp","payload":{...}} envelope and hand
        // the inner JSON-RPC object to McpServer, which replies on the same
        // voice/control/up topic via SynoraMqtt directly.
        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (root != nullptr) {
            cJSON* mcp_payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(mcp_payload)) {
                McpServer::GetInstance().ParseMessage(mcp_payload);
            }
            cJSON_Delete(root);
        }
    } else {
        std::string sentence;
        if (TryGetTtsSentenceText(payload, sentence)) {
            SynoraDisplay::GetInstance().SetSubtitle(sentence);
        }
    }
    // stt/llm/system/alert: nothing else to show yet -- no text display/UI built.
    ESP_LOGI(TAG, "control/down: %s", payload.c_str());
}

// Runs on esp-mqtt's own mqtt_task (see synora_mqtt.cc's EventHandler) --
// keep this fast. Opus decode and the blocking I2S write used to happen
// right here, which meant a burst of audio/down chunks (or one slow-ish
// decode+write) could stall MQTT's processing of everything else on that
// same task -- control messages (including tts:stop), TLS/WSS record
// handling, the lot. That's the actual cause of choppy-sounding replies:
// playback timing was at the mercy of whatever else that task was doing.
// Decode + write now happen on a dedicated playback task instead, fed
// through this queue, which also absorbs per-chunk network jitter.
void SynoraVoiceSession::OnAudioDown(const std::string& payload) {
    if (opus_decoder_ == nullptr || codec_ == nullptr || playback_queue_ == nullptr) {
        return;
    }
    auto* item = new std::string(payload);
    if (xQueueSend(playback_queue_, &item, 0) != pdTRUE) {
        // Playback task fell behind (shouldn't happen in steady state) --
        // drop this chunk rather than block the MQTT task or grow the queue
        // without bound.
        ESP_LOGW(TAG, "Playback queue full -- dropping an audio/down chunk");
        delete item;
        return;
    }
    pending_playback_chunks_.fetch_add(1, std::memory_order_relaxed);
}

void SynoraVoiceSession::PlaybackTaskTrampoline(void* arg) {
    static_cast<SynoraVoiceSession*>(arg)->PlaybackTaskLoop();
}

void SynoraVoiceSession::PlaybackTaskLoop() {
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (idle_due_us_ != 0) {
            int64_t remaining_us = idle_due_us_ - esp_timer_get_time();
            wait = remaining_us > 0 ? pdMS_TO_TICKS(remaining_us / 1000) + 1 : 0;
        }
        std::string* item = nullptr;
        if (xQueueReceive(playback_queue_, &item, wait) != pdTRUE) {
            if (idle_due_us_ != 0 && esp_timer_get_time() >= idle_due_us_) {
                ReturnToIdleIfStillDue();
            }
            continue;
        }
        if (item != nullptr) {
            DecodeAndPlay(*item);
            delete item;
            pending_playback_chunks_.fetch_sub(1, std::memory_order_relaxed);
        }
        // Either a real chunk just finished (pending_ may have hit 0) or
        // this was OnControlDown()'s idle-check wake-up -- either way, see
        // if it's time to go idle.
        MaybeReturnToIdle();
    }
}

// Muted: drop the reply instead of writing it to the speaker. Checked here
// rather than by disabling the codec's output channel -- toggling
// i2s_channel_disable() while NoAudioCodec::Write() might already be
// mid-flight on another chunk would trip its ESP_ERROR_CHECK; skipping the
// write call entirely sidesteps that race.
void SynoraVoiceSession::DecodeAndPlay(const std::string& opus_payload) {
    if (SynoraMqtt::GetInstance().IsMuted()) {
        return;
    }
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(opus_payload.data()));
    raw.len = opus_payload.size();
    esp_audio_dec_out_frame_t out = {};
    out.buffer = decode_out_buf_.data();
    out.len = decode_out_buf_.size();
    esp_audio_dec_info_t info = {};

    auto err = esp_opus_dec_decode(opus_decoder_, &raw, &out, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "Opus decode failed: %d", err);
        return;
    }
    std::vector<int16_t> pcm(out.decoded_size / sizeof(int16_t));
    std::memcpy(pcm.data(), out.buffer, out.decoded_size);
    SynoraDisplay::GetInstance().SetAudioLevel(ComputeAudioLevel(pcm.data(), pcm.size()));
    codec_->OutputData(pcm);
}

// Runs on the playback task, never on mqtt_task -- the drain-settle delay
// below would otherwise stall MQTT message processing.
void SynoraVoiceSession::MaybeReturnToIdle() {
    if (!tts_stop_received_.load() || pending_playback_chunks_.load() != 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(kPlaybackDrainSettleMs));
    tts_stop_received_.store(false);

    // Reply fully played -- a follow-up press may start a new turn now; the
    // display stays in kTalking until the idle deadline below.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!listening_) {
        awaiting_reply_ = false;
    }
    idle_due_turn_ = turn_id_;
    idle_due_us_ = esp_timer_get_time() + static_cast<int64_t>(kPostReplyIdleDelayMs) * 1000;
}

// Playback task, once idle_due_us_ passes with no new chunk in between.
void SynoraVoiceSession::ReturnToIdleIfStillDue() {
    idle_due_us_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (turn_id_ != idle_due_turn_ || listening_ || awaiting_reply_) {
        return;  // a new turn started since the reply finished -- leave its display state alone
    }
    SynoraDisplay::GetInstance().SetState(EyesState::kIdle);
    SynoraDisplay::GetInstance().SetSubtitle("");
}
