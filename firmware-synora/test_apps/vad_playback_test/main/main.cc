// Standalone test app: press the BOOT button to start recording, esp-sr's
// AFE VAD auto-stops the recording once the user goes quiet, then the
// recording plays back through the speaker. See the top-level CMakeLists.txt
// for why this exists as its own binary rather than a mode in the real app.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <cstdio>
#include <cstring>

#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "audio/codecs/no_audio_codec.h"
#include "audio/engines/afe_audio_engine.h"
#include "boards/common/button.h"
#include "boards/synora/config.h"
#include "boards/synora/font_noto_sans_vi.h"
#include "boards/synora/text_renderer.h"

namespace {

constexpr const char* TAG = "vad_playback_test";
constexpr int kChunkSamples = 160;  // 10ms @ 16kHz, same chunking as the real firmware's AudioInputTask

// Same 0.0-1.0 normalization as synora_voice_session.cc's (file-local there
// too) ComputeAudioLevel() -- kept numerically identical so readings are
// comparable against that code, in case the VAD model turns out too twitchy
// or too sluggish and a level-threshold fallback needs tuning against real
// numbers instead of guessing.
constexpr float kAudioLevelReferenceRms = 6000.0f;
constexpr int kLevelLogIntervalMs = 200;  // throttled -- logging every 10ms chunk would flood the console

// Level-threshold fallback for ending a recording -- added after esp-sr's
// own AFE VAD was observed getting stuck reporting "speaking" indefinitely
// on real hardware (ambient room noise apparently confusing the WebRTC VAD
// model into never confidently declaring silence), leaving StopAndPlayback()
// never called and the app stuck in State::kRecording. This runs alongside
// the VAD callback, not instead of it -- whichever fires first wins (both
// route through the same StopAndPlayback(), which no-ops on a second call).
// 0.1 sits comfortably above the observed ambient noise floor (typically
// 0.02-0.06, occasional bursts to ~0.14) and below real speech (spikes seen
// up to 0.84), from a live capture during this test session.
constexpr float kSilenceLevelThreshold = 0.1f;
// Consecutive time below threshold required before declaring real silence --
// not zero, so a brief dip between words/syllables doesn't cut off a
// recording mid-sentence. Tunable if this proves too eager/too sluggish.
constexpr int kSilenceHangoverMs = 3000;

// The AFE's VAD output is meaningless until it has actually been fed some
// audio: with vad_delay_ms (128) plus the default vad_min_speech_ms (128)
// and the vad_min_noise_ms (100) set in afe_audio_engine.cc, a
// freshly-started AFE emits one spurious "speech started" -> "speech
// stopped" pair inside the first ~350ms. That pair used to set
// g_had_speech and then immediately satisfy the stop-on-silence condition,
// which is why the first recording after boot ended by itself after ~0.2s.
// VAD transitions inside this window are ignored; speech that really does
// start this early is still caught by kSilenceLevelThreshold above, which
// doesn't consult the VAD at all.
constexpr int kVadSettleMs = 500;

// Barge-in: if the mic picks up real speech while a recording is being
// played back, stop the playback and start a new recording immediately,
// same as a real voice assistant letting you interrupt it. This app has no
// AEC reference path (unlike the real firmware's voice session), so the mic
// hears the speaker directly -- confirmed on real hardware that holding the
// mic near the speaker fed the played-back audio right back in as a
// "barge-in", starting a new recording that mostly captured its own echo,
// which played back and triggered again (a real feedback loop, not just a
// false positive). Raised well above kSilenceLevelThreshold and above the
// ~0.14 ambient/coupling levels seen in earlier captures, while staying
// reachable by genuine loud/close speech (peaks up to ~0.84 measured) --
// tune further if normal speech stops triggering it, or if feedback loops
// still happen at your mic/speaker distance and playback volume.
constexpr float kBargeInLevelThreshold = 0.5f;
// Playback is chunked into this many samples (100ms @ 16kHz) purely so there
// are checkpoints to test kBargeInLevelThreshold between -- the codec's
// underlying I2S write is a single blocking call otherwise, with no way to
// interrupt it mid-flight.
constexpr int kPlaybackChunkSamples = 1600;
// After playback finishes normally (no mid-playback barge-in), stay in
// State::kListening and keep watching for kBargeInLevelThreshold for this
// long before actually returning to kIdle -- lets you start talking right
// after the device finishes speaking without needing to press the button
// again, same as a real back-and-forth conversation.
constexpr int kPostPlaybackListenMs = 3000;
// How often the post-playback listening window polls the live mic level.
constexpr int kListenPollIntervalMs = 50;

// Speaker was reported quiet on real hardware. Applied here (test-app-local,
// on the already-recorded samples right before each chunk's OutputData()
// call) rather than via AudioCodec::SetOutputVolume() -- that persists to
// NVS under the "audio" namespace, which the real firmware reads too, and a
// debug-session volume bump has no business silently carrying over once this
// test app is replaced by the real one. If 1.5x still isn't loud enough,
// the next thing to try is the MAX98357A's GAIN pin (currently left
// floating = 9dB per config.h's wiring notes; wiring it for 12-15dB gets
// real analog headroom software gain can't, without the clipping risk of
// pushing this multiplier much higher).
constexpr float kPlaybackGain = 1.5f;

int16_t ApplyPlaybackGain(int16_t sample) {
    int32_t boosted = static_cast<int32_t>(sample * kPlaybackGain);
    return static_cast<int16_t>(std::clamp(boosted, static_cast<int32_t>(-32768), static_cast<int32_t>(32767)));
}

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

// Raw (unclamped) diagnostics -- ComputeAudioLevel()'s 0.0-1.0 clamp can't
// tell "genuinely loud" apart from "pegged at full-scale" (clipping/DC-bias/
// wiring fault) once RMS crosses kAudioLevelReferenceRms. min/max separate
// "loud but varying" (real audio) from "stuck at a constant rail value"
// (hardware fault) even without a speaker to listen with.
struct AudioStats {
    float rms = 0.0f;
    int16_t min_sample = 0;
    int16_t max_sample = 0;
};

AudioStats ComputeAudioStats(const int16_t* pcm, size_t samples) {
    AudioStats stats;
    if (samples == 0) {
        return stats;
    }
    int64_t sum_sq = 0;
    stats.min_sample = pcm[0];
    stats.max_sample = pcm[0];
    for (size_t i = 0; i < samples; i++) {
        int32_t s = pcm[i];
        sum_sq += s * s;
        stats.min_sample = std::min(stats.min_sample, pcm[i]);
        stats.max_sample = std::max(stats.max_sample, pcm[i]);
    }
    stats.rms = std::sqrt(static_cast<float>(sum_sq) / static_cast<float>(samples));
    return stats;
}

// Safety cap: if VAD never reports silence (continuous background noise, or
// the button was pressed by mistake), stop and play back whatever was
// captured rather than recording forever. Same spirit as
// synora_voice_session.cc's kMaxAwaitingReplyMs.
constexpr int kMaxRecordSeconds = 15;

enum class State { kIdle, kRecording, kPlaying, kListening };
enum class VadState { kUnknown, kSpeaking, kSilent };

std::atomic<State> g_state{State::kIdle};
// True once VAD has reported speech at least once during the current
// recording -- StopAndPlayback() only fires on the *next* silence after
// speech was actually seen, not on the ambient silence before the user
// starts talking.
std::atomic<bool> g_had_speech{false};
// Set by OnButtonPress() when pressed during State::kListening -- PlaybackTask
// (sole owner of that state's lifetime) checks this and treats it exactly
// like a mic barge-in, so a button press can end the listening window early
// too, not just speaking into the mic.
std::atomic<bool> g_skip_listening_requested{false};
std::atomic<int64_t> g_record_started_us{0};
// 0 = not currently in a below-threshold run; otherwise the esp_timer_get_time()
// timestamp when the level first dropped below kSilenceLevelThreshold. See
// kSilenceLevelThreshold's comment for why this fallback exists.
std::atomic<int64_t> g_silence_started_us{0};
std::mutex g_recording_mutex;
std::vector<int16_t> g_recording;
AudioCodec* g_codec = nullptr;
AfeAudioEngine* g_engine = nullptr;

// Hands a finished recording off to PlaybackTask rather than playing it back
// on whichever thread called StopAndPlayback() -- that thread is sometimes
// AudioInputTask itself (the silence-threshold/safety-timeout paths run
// there), and the codec's Write() is one big blocking I2S call with no
// interruption point, so playing back inline would leave nothing free to
// keep reading the mic for kBargeInLevelThreshold. Non-empty means
// PlaybackTask should wake up and play it.
std::mutex g_playback_queue_mutex;
std::condition_variable g_playback_queue_cv;
std::vector<int16_t> g_playback_queue;  // guarded by g_playback_queue_mutex

// TFT status screen -- there's no way to watch UART output while also
// holding the physical button and talking into the mic, so the same
// rms/min/max/level numbers AudioInputTask already logs get mirrored onto
// the display, plus state/VAD/button-press feedback that's otherwise only
// visible by scrolling back through the log after the fact. Same panel/pins
// as the real firmware's synora_display.cc (config.h's DISPLAY_* macros),
// but a much smaller purpose-built screen -- not worth pulling in the whole
// SynoraDisplay class (eyes/dashboard/pairing UI, SynoraBoard dependency)
// for one debug view.
esp_lcd_panel_io_handle_t g_panel_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
uint16_t* g_framebuffer = nullptr;
std::atomic<int> g_button_press_count{0};
std::atomic<VadState> g_vad_state{VadState::kUnknown};
std::mutex g_display_mutex;
AudioStats g_last_stats;    // guarded by g_display_mutex
float g_last_level = 0.0f;  // guarded by g_display_mutex
float g_last_recording_seconds = -1.0f;  // guarded by g_display_mutex; -1 = no playback yet

// Separate from g_display_mutex (which only ever guards tiny snapshot
// copies): this one serializes actual access to g_framebuffer/g_panel, which
// DrawStatusScreen()/PushFramebuffer() touch. Without it, AudioInputTask's
// periodic redraw, OnButtonPress()'s immediate redraw (button-driver task),
// and the VAD callback's redraw (esp-sr's own task) can all call
// DrawStatusScreen() concurrently -- racing on the shared framebuffer and
// issuing overlapping esp_lcd/SPI transactions on the same panel handle,
// which wedged the SPI driver and froze the display after a second button
// press during testing (confirmed real-hardware symptom, not theoretical).
std::mutex g_display_draw_mutex;

constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorRed = 0xF800;
constexpr uint16_t kColorGreen = 0x07E0;
constexpr uint16_t kColorGray = 0x8410;
constexpr uint16_t kColorAmber = 0xFD20;

void InitDisplay() {
    g_framebuffer = static_cast<uint16_t*>(
        heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    if (g_framebuffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate display framebuffer -- continuing without a TFT status screen");
        return;
    }

    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = DISPLAY_SCK_GPIO;
    bus_config.mosi_io_num = DISPLAY_MOSI_GPIO;
    bus_config.miso_io_num = -1;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_GPIO;
    io_config.dc_gpio_num = DISPLAY_DC_GPIO;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 20 * 1000 * 1000;  // same as synora_display.cc, same panel
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(DISPLAY_SPI_HOST), &io_config, &g_panel_io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_GPIO;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(g_panel_io, &panel_config, &g_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    // Same orientation fix as synora_display.cc, confirmed on the same
    // physical panel -- see that file's InitializePanel() comment.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(g_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(g_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));
}

void PushFramebuffer() {
    // Same PSRAM-DMA-bounce-buffer reasoning as synora_display.cc's
    // PushFramebuffer() -- band the transfer rather than sending the whole
    // frame in one shot.
    constexpr int kBandHeight = 40;
    for (int y = 0; y < DISPLAY_HEIGHT; y += kBandHeight) {
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, DISPLAY_WIDTH, y + kBandHeight, g_framebuffer + y * DISPLAY_WIDTH);
    }
}

void FillRect(int x, int y, int width, int height, uint16_t color) {
    for (int row = 0; row < height; row++) {
        int py = y + row;
        if (py < 0 || py >= DISPLAY_HEIGHT) {
            continue;
        }
        for (int col = 0; col < width; col++) {
            int px = x + col;
            if (px < 0 || px >= DISPLAY_WIDTH) {
                continue;
            }
            g_framebuffer[py * DISPLAY_WIDTH + px] = color;
        }
    }
}

const char* StateLabel(State state) {
    switch (state) {
        case State::kRecording:
            return "RECORDING";
        case State::kPlaying:
            return "PLAYING";
        case State::kListening:
            return "LISTENING";
        default:
            return "IDLE";
    }
}

uint16_t StateColor(State state) {
    switch (state) {
        case State::kRecording:
            return kColorRed;
        case State::kPlaying:
            return kColorGreen;
        case State::kListening:
            return kColorAmber;
        default:
            return kColorWhite;
    }
}

// Redraws the whole status screen from the given snapshot -- called from
// AudioInputTask's own periodic tick (throttled the same as its log line) and
// from every state-transition point (button press, VAD event, playback
// start/end) so the screen never lags more than one audio chunk behind
// what's actually happening, without needing a separate polling task.
void DrawStatusScreen(State state, const AudioStats& stats, float level, int64_t recording_elapsed_us) {
    if (g_framebuffer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> draw_lock(g_display_draw_mutex);
    std::memset(g_framebuffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    char line[64];
    std::snprintf(line, sizeof(line), "%s", StateLabel(state));
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 30, line, StateColor(state), kFontGlyphs24,
             kFontGlyphCount24, kFontLineHeight24);
    if (state == State::kRecording) {
        std::snprintf(line, sizeof(line), "%.1fs / %ds max", recording_elapsed_us / 1000000.0f, kMaxRecordSeconds);
        DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 52, line, kColorGray, kFontGlyphs14,
                 kFontGlyphCount14, kFontLineHeight14);
    }
    FillRect(0, 62, DISPLAY_WIDTH, 1, kColorGray);

    std::snprintf(line, sizeof(line), "RMS: %.0f", stats.rms);
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 92, line, kColorWhite, kFontGlyphs18,
             kFontGlyphCount18, kFontLineHeight18);
    std::snprintf(line, sizeof(line), "MIN: %d  MAX: %d", stats.min_sample, stats.max_sample);
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 116, line, kColorWhite, kFontGlyphs18,
             kFontGlyphCount18, kFontLineHeight18);
    // int16 range is +/-32767 -- pinned at (or within 100 of) either rail on
    // a real mic signal means clipping, not just "loud"; flagged in amber so
    // it's obvious without doing the math by eye every time.
    bool clipping = stats.min_sample <= -32660 || stats.max_sample >= 32660;
    std::snprintf(line, sizeof(line), "LEVEL: %.2f%s", level, clipping ? "  CLIPPING" : "");
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 140, line, clipping ? kColorAmber : kColorWhite,
             kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);

    constexpr int kBarX = 10;
    constexpr int kBarY = 150;
    constexpr int kBarWidth = 220;
    constexpr int kBarHeight = 16;
    FillRect(kBarX, kBarY, kBarWidth, kBarHeight, 0x2104);  // dim track
    int fill_width = static_cast<int>(kBarWidth * std::clamp(level, 0.0f, 1.0f));
    FillRect(kBarX, kBarY, fill_width, kBarHeight, clipping ? kColorAmber : kColorGreen);

    const char* vad_text = "VAD: unknown";
    switch (g_vad_state.load()) {
        case VadState::kSpeaking:
            vad_text = "VAD: speaking";
            break;
        case VadState::kSilent:
            vad_text = "VAD: silent";
            break;
        default:
            break;
    }
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 195, vad_text, kColorWhite, kFontGlyphs18,
             kFontGlyphCount18, kFontLineHeight18);

    std::snprintf(line, sizeof(line), "Button presses: %d", g_button_press_count.load());
    DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 220, line, kColorWhite, kFontGlyphs18,
             kFontGlyphCount18, kFontLineHeight18);

    float last_recording_seconds;
    {
        std::lock_guard<std::mutex> lock(g_display_mutex);
        last_recording_seconds = g_last_recording_seconds;
    }
    if (last_recording_seconds >= 0.0f) {
        std::snprintf(line, sizeof(line), "Last playback: %.1fs", last_recording_seconds);
        DrawText(g_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 10, 245, line, kColorWhite, kFontGlyphs18,
                 kFontGlyphCount18, kFontLineHeight18);
    }

    PushFramebuffer();
}

// Convenience wrapper for callsites that only have a state transition to
// report, not a fresh AudioStats reading -- reuses whatever AudioInputTask's
// periodic tick last measured rather than blocking on a new one.
void RedrawWithLastStats(State state) {
    AudioStats stats;
    float level;
    {
        std::lock_guard<std::mutex> lock(g_display_mutex);
        stats = g_last_stats;
        level = g_last_level;
    }
    int64_t elapsed_us = state == State::kRecording ? esp_timer_get_time() - g_record_started_us.load() : 0;
    DrawStatusScreen(state, stats, level, elapsed_us);
}

void StopAndPlayback() {
    // Guards against a double-fire -- e.g. the VAD-silence callback and the
    // safety timeout both landing at nearly the same moment.
    State expected = State::kRecording;
    if (!g_state.compare_exchange_strong(expected, State::kPlaying)) {
        return;
    }
    RedrawWithLastStats(State::kPlaying);
    g_engine->EnableVoiceProcessing(false);

    std::vector<int16_t> recording;
    {
        std::lock_guard<std::mutex> lock(g_recording_mutex);
        recording.swap(g_recording);
    }
    ESP_LOGI(TAG, "Recorded %u samples (%.1fs) -- playing back", static_cast<unsigned>(recording.size()),
             recording.size() / static_cast<float>(AUDIO_INPUT_SAMPLE_RATE));

    // Hands off to PlaybackTask and returns immediately -- see
    // g_playback_queue's comment for why this doesn't play back inline.
    {
        std::lock_guard<std::mutex> lock(g_playback_queue_mutex);
        g_playback_queue = std::move(recording);
    }
    g_playback_queue_cv.notify_one();
}

void StartRecording() {
    State expected = State::kIdle;
    if (!g_state.compare_exchange_strong(expected, State::kRecording)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_recording_mutex);
        g_recording.clear();
    }
    g_had_speech.store(false);
    g_record_started_us.store(esp_timer_get_time());
    g_silence_started_us.store(0);
    g_engine->EnableVoiceProcessing(true);
    ESP_LOGI(TAG, "Recording -- speak now, playback starts once you go quiet (or after %ds as a safety cap)",
             kMaxRecordSeconds);
    RedrawWithLastStats(State::kRecording);
}

// Runs the whole time the app is up, blocked waiting for StopAndPlayback()
// to hand off a finished recording. Plays it back in kPlaybackChunkSamples
// pieces (rather than one blocking codec call) specifically so it can check
// the live mic level between chunks -- AudioInputTask keeps updating
// g_last_level throughout, since this task (not AudioInputTask) is the one
// blocked in the codec's I2S write now. Speaking loud enough during playback
// (kBargeInLevelThreshold) cuts it short and starts a new recording
// immediately, instead of waiting for playback to finish first.
void PlaybackTask(void*) {
    while (true) {
        std::vector<int16_t> recording;
        {
            std::unique_lock<std::mutex> lock(g_playback_queue_mutex);
            g_playback_queue_cv.wait(lock, [] { return !g_playback_queue.empty(); });
            recording.swap(g_playback_queue);
        }

        bool barge_in = false;
        for (size_t offset = 0; offset < recording.size(); offset += kPlaybackChunkSamples) {
            size_t chunk_len = std::min(static_cast<size_t>(kPlaybackChunkSamples), recording.size() - offset);
            std::vector<int16_t> chunk(recording.begin() + offset, recording.begin() + offset + chunk_len);
            for (int16_t& sample : chunk) {
                sample = ApplyPlaybackGain(sample);
            }
            g_codec->OutputData(chunk);

            float level;
            {
                std::lock_guard<std::mutex> lock(g_display_mutex);
                level = g_last_level;
            }
            if (level > kBargeInLevelThreshold) {
                ESP_LOGI(TAG, "Barge-in: level %.2f > %.2f mid-playback -- stopping and starting a new recording",
                         level, kBargeInLevelThreshold);
                barge_in = true;
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_display_mutex);
            g_last_recording_seconds = recording.size() / static_cast<float>(AUDIO_INPUT_SAMPLE_RATE);
        }

        // Post-playback listening window -- only entered if playback finished
        // on its own (a mid-playback barge-in above already knows it wants a
        // new recording, no need to wait around first).
        if (!barge_in) {
            g_skip_listening_requested.store(false);
            g_state.store(State::kListening);
            RedrawWithLastStats(State::kListening);
            int64_t listen_start = esp_timer_get_time();
            while (esp_timer_get_time() - listen_start < static_cast<int64_t>(kPostPlaybackListenMs) * 1000) {
                if (g_skip_listening_requested.load()) {
                    ESP_LOGI(TAG, "Button pressed while listening -- starting a new recording now");
                    barge_in = true;
                    break;
                }
                float level;
                {
                    std::lock_guard<std::mutex> lock(g_display_mutex);
                    level = g_last_level;
                }
                if (level > kBargeInLevelThreshold) {
                    ESP_LOGI(TAG, "Barge-in: level %.2f > %.2f while listening -- starting a new recording", level,
                             kBargeInLevelThreshold);
                    barge_in = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(kListenPollIntervalMs));
            }
        }

        g_state.store(State::kIdle);  // lets StartRecording()'s CAS below succeed on a barge-in
        if (barge_in) {
            StartRecording();
        } else {
            ESP_LOGI(TAG, "Idle -- press the button to record again");
            RedrawWithLastStats(State::kIdle);
        }
    }
}

void OnButtonPress() {
    g_button_press_count.fetch_add(1);
    State state = g_state.load();
    if (state == State::kIdle) {
        StartRecording();
    } else if (state == State::kRecording) {
        ESP_LOGI(TAG, "Button pressed mid-recording -- stopping early");
        StopAndPlayback();
    } else if (state == State::kListening) {
        // PlaybackTask (sole owner of kListening's lifetime) picks this up
        // and starts the new recording itself -- see its comment.
        g_skip_listening_requested.store(true);
    }
    // kPlaying: no button action defined here -- mic barge-in already covers
    // "I want to interrupt", and StopAndPlayback() only CAS-transitions out
    // of kRecording, so a press mid-kPlaying is a silent no-op today.
}

void AudioInputTask(void*) {
    int64_t last_level_log_us = 0;
    while (true) {
        std::vector<int16_t> data(kChunkSamples);
        if (g_codec->InputData(data)) {
            // Logged always (not just while recording) -- lets mic response
            // be checked (quiet vs. speaking vs. tapping the mic) without
            // needing to press the button at all, e.g. to tell a real
            // full-scale signal apart from a stuck/clipped one.
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_level_log_us >= kLevelLogIntervalMs * 1000) {
                last_level_log_us = now_us;
                AudioStats stats = ComputeAudioStats(data.data(), data.size());
                float level = ComputeAudioLevel(data.data(), data.size());
                ESP_LOGI(TAG, "rms=%.0f min=%d max=%d level=%.2f", stats.rms, stats.min_sample, stats.max_sample,
                         level);
                {
                    std::lock_guard<std::mutex> lock(g_display_mutex);
                    g_last_stats = stats;
                    g_last_level = level;
                }
                State state = g_state.load();
                if (state == State::kRecording) {
                    if (level >= kSilenceLevelThreshold) {
                        g_had_speech.store(true);
                        g_silence_started_us.store(0);
                    } else if (g_had_speech.load()) {
                        int64_t silence_start = g_silence_started_us.load();
                        if (silence_start == 0) {
                            g_silence_started_us.store(now_us);
                        } else if (now_us - silence_start >= static_cast<int64_t>(kSilenceHangoverMs) * 1000) {
                            ESP_LOGI(TAG, "Level-threshold fallback: %dms below %.2f -- stopping",
                                     kSilenceHangoverMs, kSilenceLevelThreshold);
                            StopAndPlayback();
                            state = g_state.load();
                        }
                    }
                }
                int64_t elapsed_us = state == State::kRecording ? now_us - g_record_started_us.load() : 0;
                // Redrawn during kPlaying too (unlike before PlaybackTask
                // existed) -- this is exactly the live level reading
                // kBargeInLevelThreshold watches, so it's worth seeing on
                // screen while a recording is playing back.
                DrawStatusScreen(state, stats, level, elapsed_us);
            }
            g_engine->Feed(std::move(data));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (g_state.load() == State::kRecording &&
            (esp_timer_get_time() - g_record_started_us.load()) >
                static_cast<int64_t>(kMaxRecordSeconds) * 1000000) {
            ESP_LOGW(TAG, "Safety timeout (%ds) -- no silence detected, stopping anyway", kMaxRecordSeconds);
            StopAndPlayback();
        }
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    InitDisplay();
    RedrawWithLastStats(State::kIdle);

    static NoAudioCodecSimplex codec(
        AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
        AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    AudioCodec& codec_if = codec;
    codec_if.Start();
    codec_if.EnableInput(true);
    codec_if.EnableOutput(true);
    g_codec = &codec_if;

    static AfeAudioEngine engine;
    // models_list == nullptr -> loads from the "model" SPIFFS partition.
    // This app's partitions.csv/sdkconfig.defaults only pack a VAD model (no
    // WakeNet), so HasWakeWord() will correctly report false -- fine, this
    // app never uses wake-word detection.
    if (!engine.Initialize(&codec, 60, nullptr)) {
        ESP_LOGE(TAG, "AFE engine init failed -- check the 'model' partition has a VAD model packed");
        return;
    }
    g_engine = &engine;

    engine.OnVadStateChange([](bool speaking) {
        ESP_LOGI(TAG, "VAD: %s", speaking ? "speech started" : "speech stopped");
        g_vad_state.store(speaking ? VadState::kSpeaking : VadState::kSilent);
        State state = g_state.load();
        if (state == State::kRecording &&
            esp_timer_get_time() - g_record_started_us.load() <
                static_cast<int64_t>(kVadSettleMs) * 1000) {
            // See kVadSettleMs -- this early into a recording the transition
            // is the AFE settling, not the user.
            ESP_LOGI(TAG, "Ignoring VAD transition inside the %dms settle window", kVadSettleMs);
            RedrawWithLastStats(state);
            return;
        }
        if (speaking) {
            // Only while recording: "speech started" seen at any other time
            // (the boot warm-up below, or playback) isn't part of a turn, and
            // letting it set the flag arms the next silence to stop a
            // recording that hasn't heard the user yet.
            if (state == State::kRecording) {
                g_had_speech.store(true);
            }
            RedrawWithLastStats(state);
        } else if (g_had_speech.load() && state == State::kRecording) {
            StopAndPlayback();
        } else {
            RedrawWithLastStats(state);
        }
    });
    engine.OnOutput([](std::vector<int16_t>&& data) {
        if (g_state.load() != State::kRecording) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_recording_mutex);
        g_recording.insert(g_recording.end(), data.begin(), data.end());
    });

    // PSRAM-backed stack: engine->Feed()'s call into esp-sr's AFE costs far
    // more stack than a plain buffer copy (see main.cc's own audio_input
    // task in the real firmware, which hit a stack overflow at 16384 bytes
    // of internal RAM before moving to PSRAM -- same fix applied here up
    // front rather than after a live crash).
    constexpr size_t kAudioInputTaskStackSize = 4096 * 8;
    static StackType_t* audio_input_task_stack =
        static_cast<StackType_t*>(heap_caps_malloc(kAudioInputTaskStackSize, MALLOC_CAP_SPIRAM));
    static StaticTask_t audio_input_task_buffer;
    xTaskCreateStatic(AudioInputTask, "audio_input", kAudioInputTaskStackSize, nullptr, 5,
                       audio_input_task_stack, &audio_input_task_buffer);

    // Smaller than AudioInputTask's stack -- PlaybackTask only ever calls
    // OutputData()/Write() (a plain int16->int32 buffer copy + I2S write),
    // never into esp-sr's AFE, so it doesn't need nearly as much. Still
    // PSRAM-backed rather than internal RAM, same reasoning as above.
    constexpr size_t kPlaybackTaskStackSize = 4096 * 4;
    static StackType_t* playback_task_stack =
        static_cast<StackType_t*>(heap_caps_malloc(kPlaybackTaskStackSize, MALLOC_CAP_SPIRAM));
    static StaticTask_t playback_task_buffer;
    xTaskCreateStatic(PlaybackTask, "playback", kPlaybackTaskStackSize, nullptr, 5, playback_task_stack,
                       &playback_task_buffer);

    // One throwaway enable/disable cycle before the first button press, now
    // that AudioInputTask exists to feed the engine. Until the AFE has seen
    // audio it has no noise estimate, so the first recording after boot was
    // the one that absorbed the startup VAD pair described at kVadSettleMs;
    // it was also the only activation that skipped the AFE buffer reset,
    // because afe_audio_engine.cc only marks a reset pending when voice
    // processing is switched *off*. Warming up here makes the first real
    // press behave exactly like every press after it. Nothing is captured:
    // OnOutput() drops anything arriving outside State::kRecording.
    constexpr int kAfeWarmupMs = 1000;
    ESP_LOGI(TAG, "Warming up the AFE for %dms before accepting button presses", kAfeWarmupMs);
    engine.EnableVoiceProcessing(true);
    vTaskDelay(pdMS_TO_TICKS(kAfeWarmupMs));
    engine.EnableVoiceProcessing(false);
    g_vad_state.store(VadState::kUnknown);
    g_had_speech.store(false);
    RedrawWithLastStats(State::kIdle);

    // Registered only now, after the warm-up: OnButtonPress() starting a
    // recording mid-warm-up would have it pulled out from under itself by
    // the EnableVoiceProcessing(false) above, leaving a recording with a
    // dead AFE that nothing could stop until the safety timeout. Same
    // active_high/long_press_time as the real firmware's BOOT button
    // (synora_board.cc) -- this app has no factory-reset use for the long
    // press, just matching so a long press behaves the same (ignored by
    // OnClick) rather than being untested.
    static Button boot_button(BOOT_BUTTON_GPIO, /*active_high=*/false, FACTORY_RESET_HOLD_MS);
    boot_button.OnClick(OnButtonPress);

    ESP_LOGI(TAG, "Ready -- press the BOOT button to start recording");
}
