#include "test_console.h"

#include "audio/audio_codec.h"
#include "boards/common/synora_mqtt.h"
#include "synora_board.h"
#include "synora_display.h"
#include "synora_voice_session.h"

#include <esp_console.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <arpa/inet.h>
#include <lwip/netdb.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* TAG = "TestConsole";
static SynoraBoard* g_board = nullptr;
static AudioCodec* g_codec = nullptr;
static std::atomic<bool> g_mic_test_recording{false};

bool IsMicTestRecording() { return g_mic_test_recording.load(); }

// Minimal 44-byte canonical RIFF/WAVE header for mono 16-bit PCM, appended
// straight into the same buffer the PCM bytes go into -- matches what the
// TTS sidecar's own WAV output looks like (see tts/app.py) and what any
// standard player/decoder expects.
static void AppendWavHeader(std::string& out, uint32_t sample_rate, uint32_t data_bytes) {
    uint32_t byte_rate = sample_rate * sizeof(int16_t);
    uint16_t block_align = sizeof(int16_t);
    uint16_t bits_per_sample = 16;
    uint32_t riff_size = 36 + data_bytes;
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;  // PCM
    uint16_t channels = 1;

    auto append = [&out](const void* data, size_t size) { out.append(static_cast<const char*>(data), size); };
    append("RIFF", 4);
    append(&riff_size, 4);
    append("WAVE", 4);
    append("fmt ", 4);
    append(&fmt_size, 4);
    append(&audio_format, 2);
    append(&channels, 2);
    append(&sample_rate, 4);
    append(&byte_rate, 4);
    append(&block_align, 2);
    append(&bits_per_sample, 2);
    append("data", 4);
    append(&data_bytes, 4);
}

static int CmdTelemetry(int argc, char** argv) {
    if (g_board == nullptr) {
        return 1;
    }
    g_board->RefreshSensors();
    g_board->PublishTelemetry();
    return 0;
}

static int CmdFontTest(int argc, char** argv) {
    SynoraDisplay::GetInstance().ToggleFontTest();
    return 0;
}

// Same 0.0-1.0 normalization as SynoraVoiceSession's (file-local, not
// exported) ComputeAudioLevel() -- duplicated here rather than shared so
// this diagnostic has no dependency on the voice session, but deliberately
// kept numerically identical (same kAudioLevelReferenceRms) so the printed
// numbers are on the same scale as the voice session's local silence-
// threshold auto-stop (SynoraVoiceSession::kSilenceLevelThreshold).
constexpr float kAudioLevelReferenceRms = 6000.0f;
static float ComputeChunkLevel(const int16_t* pcm, size_t samples) {
    if (samples == 0) {
        return 0.0f;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t s = pcm[i];
        sum_sq += s * s;
    }
    float rms = sqrtf(static_cast<float>(sum_sq) / static_cast<float>(samples));
    float level = rms / kAudioLevelReferenceRms;
    return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

// Records N seconds of raw mic input, plays it straight back through the
// speaker (a pure hardware loopback that never touches the real voice
// pipeline, for judging mic pickup quality by ear), and uploads it as a WAV
// over MQTT to synora/devices/{deviceId}/mictest/upload -- the server stores
// it and an admin-only web view lists/plays them (so a recording can be
// pulled up and A/B'd, e.g. before/after a wiring fix, without needing
// physical access to this console). Also prints min/avg/max level and how
// much of the recording sits above kSpeechDetectedThreshold, to check with
// real numbers how noisy the mic actually is (confirmed live: a wiring issue
// once made every chunk read as full-scale even in silence).
//
// AudioInputTask (main.cc) is *also* continuously reading from this same
// I2S RX channel the whole time (wake-word detection never stops) --
// esp-idf's I2S driver doesn't tolerate two concurrent readers on one
// channel (confirmed live: "channel is not enabled" errors, not just
// choppy audio), so g_mic_test_recording tells AudioInputTask to stand
// down for the duration.
static int CmdMicTest(int argc, char** argv) {
    if (g_codec == nullptr) {
        printf("Codec not ready yet\n");
        return 1;
    }
    float seconds = argc > 1 ? static_cast<float>(atof(argv[1])) : 3.0f;
    if (seconds <= 0.0f || seconds > 10.0f) {
        printf("Duration must be between 0 and 10 seconds\n");
        return 1;
    }

    constexpr int kChunkSamples = 160;  // 10ms @ 16kHz, same chunking as AudioInputTask
    constexpr float kSpeechDetectedThreshold = SynoraVoiceSession::kSilenceLevelThreshold;
    int channels = g_codec->input_channels();
    size_t target_samples = static_cast<size_t>(seconds * g_codec->input_sample_rate()) * channels;

    std::vector<int16_t> recording;
    recording.reserve(target_samples);
    float min_level = 1.0f, max_level = 0.0f, sum_level = 0.0f;
    int chunk_count = 0, above_threshold_count = 0;
    printf("Recording %.1fs...\n", seconds);
    g_mic_test_recording.store(true);
    // Give AudioInputTask a moment to notice the flag and stop calling
    // InputData() before this loop starts reading -- otherwise the very
    // first chunk or two can still race it.
    vTaskDelay(pdMS_TO_TICKS(20));
    while (recording.size() < target_samples) {
        std::vector<int16_t> chunk(kChunkSamples * channels);
        if (g_codec->InputData(chunk)) {
            // channels > 1 means a software AEC reference channel is
            // interleaved in (see NoAudioCodec::EnableSoftwareAecReference)
            // -- this diagnostic only cares about the real mic (channel 0);
            // recording/playback/WAV export all stay mono.
            std::vector<int16_t> mono;
            const int16_t* mic_pcm = chunk.data();
            size_t mic_samples = chunk.size();
            if (channels > 1) {
                mono.resize(chunk.size() / channels);
                for (size_t i = 0, j = 0; i < chunk.size(); i += channels, ++j) {
                    mono[j] = chunk[i];
                }
                mic_pcm = mono.data();
                mic_samples = mono.size();
            }
            float level = ComputeChunkLevel(mic_pcm, mic_samples);
            min_level = std::min(min_level, level);
            max_level = std::max(max_level, level);
            sum_level += level;
            chunk_count++;
            if (level >= kSpeechDetectedThreshold) {
                above_threshold_count++;
            }
            recording.insert(recording.end(), mic_pcm, mic_pcm + mic_samples);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    g_mic_test_recording.store(false);

    printf("Recorded %u samples, playing back...\n", static_cast<unsigned>(recording.size()));
    if (chunk_count > 0) {
        printf("Level (0.0-1.0, threshold=%.2f): min=%.3f avg=%.3f max=%.3f -- %d/%d chunks (%.0f%%) above threshold\n",
               kSpeechDetectedThreshold, min_level, sum_level / chunk_count, max_level, above_threshold_count,
               chunk_count, 100.0f * above_threshold_count / chunk_count);
    }
    g_codec->OutputData(recording);

    std::string wav;
    wav.reserve(44 + recording.size() * sizeof(int16_t));
    AppendWavHeader(wav, static_cast<uint32_t>(g_codec->input_sample_rate()),
                    static_cast<uint32_t>(recording.size() * sizeof(int16_t)));
    wav.append(reinterpret_cast<const char*>(recording.data()), recording.size() * sizeof(int16_t));

    // Confirmed live: a bare fire-and-forget Publish() reports "success" the
    // instant esp-mqtt accepts the call locally, which proves nothing about
    // the broker actually receiving it -- over the public WSS tunnel path,
    // which disconnects/reconnects every 20-100s, a payload this size can
    // easily land in a dead window and silently vanish. PublishAndWaitForAck
    // actually waits for the QoS 1 PUBACK; retrying gives the flaky
    // connection several chances to have a stable moment somewhere in a
    // ~50s span rather than gambling on exactly one attempt.
    constexpr int kMaxAttempts = 5;
    constexpr int kAckTimeoutMs = 8000;
    constexpr int kRetryDelayMs = 2000;
    bool uploaded = false;
    for (int attempt = 1; attempt <= kMaxAttempts && !uploaded; attempt++) {
        if (attempt > 1) {
            printf("Upload attempt %d/%d timed out, retrying...\n", attempt - 1, kMaxAttempts);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }
        uploaded = SynoraMqtt::GetInstance().PublishAndWaitForAck("mictest/upload", wav, /*qos=*/1, kAckTimeoutMs);
    }

    if (uploaded) {
        printf("Uploaded %u bytes to the server, confirmed (admin-only web view)\n",
               static_cast<unsigned>(wav.size()));
    } else {
        printf("Upload failed after %d attempts -- server never acked it (check connection)\n", kMaxAttempts);
    }
    printf("Done\n");
    return 0;
}

// Diagnostic for the "device can't resolve synora.carcise.dev over MQTT but
// a phone on the same network resolves it fine" report -- calls the exact
// same getaddrinfo() esp-tls uses (esp_tls_hostname_to_fd() in esp-tls.c)
// directly from the console, and prints what DNS servers esp_netif actually
// has configured for the WiFi station interface, so a live device can be
// checked without waiting out esp-mqtt's own reconnect/retry cycle.
static int CmdDns(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "synora.carcise.dev";

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        printf("No WIFI_STA_DEF netif -- station not up\n");
    } else {
        for (int type = 0; type < 3; type++) {
            esp_netif_dns_info_t dns_info;
            if (esp_netif_get_dns_info(netif, static_cast<esp_netif_dns_type_t>(type), &dns_info) == ESP_OK &&
                dns_info.ip.u_addr.ip4.addr != 0) {
                printf("DNS[%d] = " IPSTR "\n", type, IP2STR(&dns_info.ip.u_addr.ip4));
            }
        }
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    int err = getaddrinfo(host, nullptr, &hints, &result);
    printf("getaddrinfo(%s) = %d\n", host, err);
    for (auto* p = result; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* addr = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            printf("  -> %s\n", inet_ntoa(addr->sin_addr));
        }
    }
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    return 0;
}

void InitializeTestConsole(SynoraBoard* board, AudioCodec* codec) {
    g_board = board;
    g_codec = codec;

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "synora>";

    // Native USB-Serial-JTAG (/dev/ttyACM0), not UART0 -- UART0/CONFIG_ESP_
    // CONSOLE_UART was the first thing tried, but produced zero bytes in
    // either direction on this board's ST-Link VCP (/dev/ttyACM1), including
    // plain ESP_LOG mirroring, so its physical wiring/routing on this custom
    // board was never actually confirmed reachable. USB-Serial-JTAG is the
    // one port already known-good all session (flashing, log capture), and
    // sdkconfig.defaults makes it the sole (primary, no secondary) console
    // to match -- see its comment for why running both would double-install
    // the same underlying driver.
    esp_console_dev_usb_serial_jtag_config_t usb_jtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_jtag_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t telemetry_cmd = {
        .command = "telemetry",
        .help = "Read sensors now and publish an MQTT telemetry/keepalive message",
        .hint = nullptr,
        .func = &CmdTelemetry,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&telemetry_cmd));

    const esp_console_cmd_t font_test_cmd = {
        .command = "fonttest",
        .help = "Toggle a screen dumping every Vietnamese vowel/tone glyph, to check font rendering",
        .hint = nullptr,
        .func = &CmdFontTest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&font_test_cmd));

    const esp_console_cmd_t dns_cmd = {
        .command = "dns",
        .help = "Show configured DNS servers and resolve a hostname (default synora.carcise.dev)",
        .hint = "[hostname]",
        .func = &CmdDns,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&dns_cmd));

    const esp_console_cmd_t mictest_cmd = {
        .command = "mictest",
        .help = "Record N seconds of mic input (default 3, max 10), play it back, and upload it to the server",
        .hint = "[seconds]",
        .func = &CmdMicTest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mictest_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Test console ready on /dev/ttyACM0 -- type 'help' to list commands");
}
