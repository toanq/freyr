#pragma once

#include <atomic>
#include <mutex>

#include <esp_err.h>
#include <esp_lcd_panel_io.h>

#include "dashboard_status.h"

class SynoraBoard;  // only a pointer is needed here; see synora_display.cc

// What the eyes should be doing right now. Idle = closed/"asleep"; the other
// two both render open-with-periodic-blink -- talking doesn't (yet) animate
// differently from listening, since there's no mouth/waveform to distinguish
// them with. Revisit once the voice pipeline (task blocked on the MQTT
// topic scheme) actually distinguishes the two in a way worth showing.
enum class EyesState {
    kIdle,
    kListening,
    kTalking,
};

// Which full screen is currently showing. Derived, not independently set --
// see SynoraDisplay::GetScreen(): an active voice session (EyesState !=
// kIdle) always wins over pairing/dashboard, so there's no separate "force
// back to eyes" step needed anywhere else.
enum class DisplayScreen {
    kEyes,
    kPairing,
    kDashboard,
    kFontTest,
    kWatering,
    kPing,
};

// Drives the ST7793 display (config.h's DISPLAY_* pins) with a simple
// two-circle "Mochi" face -- no LVGL/display framework yet (Phase 4 stub in
// idf_component.yml is still unused): direct esp_lcd_panel_io register/pixel
// writes into an RGB565 framebuffer are all two blinking circles need, and
// pulling in a whole UI toolkit for that would be pure speculative scope.
//
// No esp_lcd "vendor" driver exists for ST7793 (unlike ST7789/96/etc.), so
// this talks to esp_lcd_panel_io directly instead of going through an
// esp_lcd_panel_handle_t: register index writes use the panel_io "cmd" phase
// (16 bits wide -- ST7793's extended registers, e.g. R400h, don't fit in the
// 8-bit "cmd" convention most vendor drivers assume) and register values use
// the "param" phase, matching the vendor's own reference driver.
class SynoraDisplay {
public:
    static SynoraDisplay& GetInstance();

    // board is used to pull sensor readings for the dashboard screen (owned
    // by SynoraBoard, not this class -- see project decision in the
    // "display integration" plan). Not retained beyond storing the pointer;
    // SynoraBoard outlives SynoraDisplay for the life of the program.
    esp_err_t Init(SynoraBoard* board);
    void SetState(EyesState state);

    // 0.0 (silent) to 1.0 (loud) -- SynoraVoiceSession calls this on every
    // mic chunk while listening and every decoded chunk while talking, so
    // the eyes widen with voice volume during an active session (see
    // DrawEyes()/AnimationTask). Has no effect while idle -- nothing reads
    // it then.
    void SetAudioLevel(float level);

    // Debug-only, driven by the UART test console's "fonttest" command --
    // toggles a screen dumping every Vietnamese vowel/tone combination (see
    // font_noto_sans_vi.h's kVietnameseTestString) so font/glyph-table
    // regressions can be checked visually on real hardware without waiting
    // on the pairing/dashboard screens' fixed, narrower text. Wins over
    // eyes/pairing/dashboard in GetScreen() while active; an active voice
    // session still can't be reached while this is on, same as any other
    // non-eyes screen -- toggle it off first.
    void ToggleFontTest();

    // Shows a falling-water-drops animation for duration_ms, then reverts to
    // whatever screen would normally show -- called from SynoraBoard's
    // "commands/water" handler with the same (already-clamped) duration
    // PumpDriver::RunFor() actually used, so the animation and the physical
    // pump run for exactly the same length of time.
    void ShowWatering(int duration_ms);

    // Shows a shaking-bell screen for a fixed duration -- called from
    // SynoraBoard's "commands/ping" handler alongside find_me_led_.Blink().
    void ShowPing();

    SynoraDisplay(const SynoraDisplay&) = delete;
    SynoraDisplay& operator=(const SynoraDisplay&) = delete;

private:
    SynoraDisplay() = default;
    ~SynoraDisplay() = default;

    void InitializeBus();
    esp_err_t InitializePanel();
    void ResetPanel();
    void WriteReg(uint16_t index, uint16_t value);
    void PushFramebuffer();
    void BlitMonoBitmap(int x, int y, const uint8_t* bitmap, int width, int height, uint16_t color);
    void DrawEyes(int ry);
    void DrawMouth();
    void DrawLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color);
    void FillRoundedRect(int cx, int cy, int half_width, int half_height, int corner_radius, uint16_t color);
    void DrawPairingScreen();
    void DrawDashboardScreen();
    void DrawSensorTile(int cx, int cy, TileStatus status, const char* label, const char* value,
                        void (SynoraDisplay::*draw_icon)(int, int, uint16_t));
    void DrawThermometerIcon(int cx, int cy, uint16_t color);
    void DrawDropIcon(int cx, int cy, uint16_t color);
    void DrawWindIcon(int cx, int cy, uint16_t color);
    void DrawWaveIcon(int cx, int cy, uint16_t color);
    void DrawFontTestScreen();
    void DrawWateringScreen(int frame, float remaining_fraction);
    void RandomizeWateringDrops();
    void DrawPingScreen(float remaining_fraction);
    void FillRect(int x, int y, int width, int height, uint16_t color);
    EyesState GetState();
    DisplayScreen GetScreen();
    static void AnimationTask(void* arg);

    esp_lcd_i80_bus_handle_t i80_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    uint16_t* framebuffer_ = nullptr;
    SynoraBoard* board_ = nullptr;

    std::mutex state_mutex_;
    EyesState state_ = EyesState::kIdle;
    std::atomic<bool> font_test_active_ = false;
    // Millisecond timestamps (esp_timer_get_time() / 1000 base, matching
    // AnimationTask's own clock) marking when each transient screen should
    // stop showing -- 0/expired means "not active". Plain int64_t atomics
    // rather than a mutex since each is a single independently-meaningful
    // value written by one side (MQTT event handler / board) and read by
    // another (AnimationTask). watering_total_ms_ is the duration the
    // current watering pulse started with, kept alongside the deadline so
    // AnimationTask can compute a remaining-time fraction for the shrinking
    // progress bar (ping's duration is a fixed constant, so it doesn't need
    // an equivalent).
    std::atomic<int64_t> watering_until_ms_ = 0;
    std::atomic<int64_t> watering_total_ms_ = 0;
    std::atomic<int64_t> ping_until_ms_ = 0;
    std::atomic<float> audio_level_ = 0.0f;

    // Per-drop randomized fall parameters for the watering screen -- only
    // ever written by RandomizeWateringDrops(), called from AnimationTask's
    // own task (on kWatering's screen_changed) so there's no cross-task
    // access to guard against.
    struct WateringDropState {
        int x = 0;
        int start_offset_ticks = 0;
        int cycle_ticks = 20;
    };
    static constexpr int kWateringDropCount = 5;
    WateringDropState watering_drops_[kWateringDropCount];
};
