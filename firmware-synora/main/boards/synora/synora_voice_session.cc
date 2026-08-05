#include "synora_voice_session.h"

#include "boards/common/synora_mqtt.h"
#include "mcp_server.h"
#include "synora_display.h"
#include "voice_messages.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

static const char* TAG = "SynoraVoiceSession";

namespace {
// Fixed to match the server's audio_params (Synora.Voice's VoiceSession):
// 16kHz mono, 60ms frames, Opus. config.h sets both codec sample rates to
// 16000 for the same reason -- no resampler needed on either leg.
constexpr int kSampleRateHz = 16000;
constexpr int kFrameDurationMs = 60;

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

// On the same 0.0-1.0 ComputeAudioLevel() scale -- comfortably above mic
// noise floor, low enough to catch normal speech onset promptly. Only used
// to detect "has the user said anything yet this turn", not for real VAD
// (end-of-turn is still the user's own button click, unchanged).
constexpr float kSpeechDetectedThreshold = 0.12f;
// How long a turn waits for any speech before FeedMic() gives up and
// cancels it locally -- separate constants (not one shared value) since a
// button-initiated turn (the user just pressed it, presumably to say
// something right away) and an auto-relistened turn (see OnControlDown(),
// no explicit user action prompted it) warrant different grace periods.
constexpr int64_t kListenSilenceTimeoutMs = 10000;
constexpr int64_t kRelistenSilenceTimeoutMs = 20000;
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

    ESP_LOGI(TAG, "Ready: encoder frame = %d samples, decoder out buf = %u bytes",
             encoder_frame_samples_, static_cast<unsigned>(decode_out_buf_.size()));
}

void SynoraVoiceSession::Toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (listening_) {
        StopListening();
    } else if (!SynoraMqtt::GetInstance().IsMuted()) {
        StartListening(/*is_relisten=*/false);
    }
}

// mutex_ already held by the caller (Toggle()/OnControlDown()).
void SynoraVoiceSession::StartListening(bool is_relisten) {
    pcm_accum_.clear();
    listening_ = true;
    listening_started_ms_ = esp_timer_get_time() / 1000;
    speech_detected_ = false;
    is_relisten_ = is_relisten;

    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildHelloMessage(kSampleRateHz, kFrameDurationMs), /*qos=*/1);
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/true), 1);
    SynoraDisplay::GetInstance().SetState(EyesState::kListening);
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f); // fresh session starts quiet, FeedMic() updates it immediately
    ESP_LOGI(TAG, "Listening");
}

// mutex_ already held by the caller (Toggle()).
void SynoraVoiceSession::StopListening() {
    listening_ = false;
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/false), 1);
    // The server's reply (stt/llm/tts + audio/down) is expected imminently;
    // OnControlDown() switches back to idle once tts:stop actually arrives.
    SynoraDisplay::GetInstance().SetState(EyesState::kTalking);
    // Reset rather than leave the last mic level showing -- there's a real
    // silence gap here until the first decoded reply chunk arrives and
    // starts driving the level itself.
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f);
    ESP_LOGI(TAG, "Stopped listening, awaiting reply");
}

void SynoraVoiceSession::FeedMic(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!listening_ || opus_encoder_ == nullptr) {
        return;
    }
    if (SynoraMqtt::GetInstance().IsMuted()) {
        // Muted mid-turn (server toggle raced the button click) -- cancel
        // right away rather than waiting out the silence timeout below;
        // dropped mic audio would hit that same timeout eventually, but
        // there's no reason to wait once we already know why it's silent.
        ESP_LOGI(TAG, "Muted mid-session -- cancelling locally");
        CancelListeningLocally();
        return;
    }
    float level = ComputeAudioLevel(pcm.data(), pcm.size());
    SynoraDisplay::GetInstance().SetAudioLevel(level);

    int64_t timeout_ms = is_relisten_ ? kRelistenSilenceTimeoutMs : kListenSilenceTimeoutMs;
    if (level >= kSpeechDetectedThreshold) {
        speech_detected_ = true;
    } else if (!speech_detected_ && (esp_timer_get_time() / 1000 - listening_started_ms_) >= timeout_ms) {
        // The user never said anything this turn -- cancel locally rather
        // than sending a near-empty capture to the server and waiting on a
        // reply nobody asked for. Not StopListening(): that sets kTalking
        // and expects a reply, which is exactly what this is trying to
        // avoid.
        ESP_LOGI(TAG, "No speech within %lld ms (relisten=%d), cancelling", static_cast<long long>(timeout_ms),
                 is_relisten_);
        CancelListeningLocally();
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
// and awaits a reply). Shared by the silence timeout and the mute-mid-turn
// case above.
void SynoraVoiceSession::CancelListeningLocally() {
    listening_ = false;
    SynoraMqtt::GetInstance().Publish("voice/control/up", BuildListenMessage(/*start=*/false), 1);
    SynoraDisplay::GetInstance().SetState(EyesState::kIdle);
    SynoraDisplay::GetInstance().SetAudioLevel(0.0f);
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
        // Auto-relisten rather than going idle -- lets a conversation
        // continue across multiple turns without another button press.
        // FeedMic()'s silence timeout (kRelistenSilenceTimeoutMs) is what
        // actually falls back to idle, if the user doesn't say anything
        // this time. Skipped while muted -- same reasoning as Toggle().
        std::lock_guard<std::mutex> lock(mutex_);
        if (!SynoraMqtt::GetInstance().IsMuted()) {
            StartListening(/*is_relisten=*/true);
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
    }
    // stt/llm/system/alert: nothing to show yet -- no text display/UI built.
    ESP_LOGI(TAG, "control/down: %s", payload.c_str());
}

void SynoraVoiceSession::OnAudioDown(const std::string& payload) {
    // Muted: drop the reply instead of writing it to the speaker. Checked
    // here rather than by disabling the codec's output channel -- toggling
    // i2s_channel_disable() while NoAudioCodec::Write() might already be
    // mid-flight on another chunk would trip its ESP_ERROR_CHECK; skipping
    // the write call entirely sidesteps that race.
    if (opus_decoder_ == nullptr || codec_ == nullptr || SynoraMqtt::GetInstance().IsMuted()) {
        return;
    }
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data()));
    raw.len = payload.size();
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
