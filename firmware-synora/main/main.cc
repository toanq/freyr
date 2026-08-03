#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>

#include "audio/codecs/no_audio_codec.h"
#include "audio/engines/afe_audio_engine.h"
#include "boards/synora/synora_board.h"
#include "boards/synora/synora_voice_session.h"
#include "boards/synora/config.h"
#include "boards/synora/test_console.h"

#define TAG "synora"

// Feeds every chunk read from the mic to both consumers: the AFE engine
// (wake-word detection, always) and the voice session (Opus-encode + publish
// over MQTT, only while a click-to-talk session is active -- FeedMic() is a
// no-op otherwise). Two copies of the chunk since AfeAudioEngine::Feed()
// moves its argument.
static void AudioInputTask(void* arg) {
    auto* ctx = static_cast<std::pair<NoAudioCodecSimplex*, AfeAudioEngine*>*>(arg);
    AudioCodec* codec = ctx->first;
    AfeAudioEngine* engine = ctx->second;

    constexpr int kChunkSamples = 160;  // 10 ms @ 16 kHz
    while (true) {
        std::vector<int16_t> data(kChunkSamples * codec->input_channels());
        if (codec->InputData(data)) {
            SynoraVoiceSession::GetInstance().FeedMic(data);
            engine->Feed(std::move(data));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Phase 1 audio bring-up. PSRAM free: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // BOOT-button factory reset (hold FACTORY_RESET_HOLD_MS) is live from here on.
    static SynoraBoard board;
    InitializeTestConsole(&board);

    // Simplex I2S: separate mic + amp buses, no I2C codec.
    static NoAudioCodecSimplex codec(
        AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
        AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
    // Drive the codec through the public AudioCodec interface: NoAudioCodec
    // declares EnableInput/EnableOutput as protected overrides on purpose.
    AudioCodec& codec_if = codec;
    codec_if.Start();
    codec_if.EnableInput(true);
    codec_if.EnableOutput(true); // voice-session playback (audio/down) needs output live.

    SynoraVoiceSession::GetInstance().Init(&codec);

    static AfeAudioEngine engine;
    engine.OnWakeWordDetected([](const std::string& wake_word) {
        ESP_LOGI(TAG, "*** WAKE WORD DETECTED: \"%s\" ***", wake_word.c_str());
    });

    // models_list == nullptr -> engine loads wake-word models from the "model"
    // SPIFFS partition via esp_srmodel_init("model").
    if (!engine.Initialize(&codec, 60, nullptr)) {
        ESP_LOGE(TAG, "Audio engine init failed");
        return;
    }
    if (!engine.HasWakeWord()) {
        ESP_LOGE(TAG, "No wake-word model found in 'model' partition -- "
                      "check the CONFIG_SR_WN_* selection");
        return;
    }
    engine.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "Wake-word detection enabled; say the wake word.");

    static auto ctx = std::make_pair(&codec, &engine);
    xTaskCreate(AudioInputTask, "audio_input", 4096, &ctx, 5, nullptr);
}
