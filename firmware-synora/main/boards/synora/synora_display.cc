#include "synora_display.h"

#include "boards/common/device_activation.h"
#include "config.h"
#include "dashboard_status.h"
#include "eyes_geometry.h"
#include "font_noto_sans_vi.h"
#include "logo_icon.h"
#include "qr_download.h"
#include "sensor_icons.h"
#include "sensor_readings.h"
#include "synora_board.h"
#include "text_renderer.h"
#include "watering_schedule.h"

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

static const char* TAG = "SynoraDisplay";

namespace {
// Two ovals, cute/"kawaii" mochi-style proportions (slightly taller than
// wide). Open = full oval; closed = squashed to a thin eyelash line at the
// same center, same width -- a blink, not a resize.
// Scaled from the original 128x128 layout by the 240/128 width ratio (used
// for radii/spacing too, so the face stays proportioned rather than
// stretched) and centered on the 240x400 canvas, with blank space above/
// below since the panel is taller than it is wide.
constexpr int kEyeCenterY = 200;
constexpr int kLeftEyeCenterX = 75;
constexpr int kRightEyeCenterX = 165;
constexpr int kEyeRadiusX = 34;
constexpr int kEyeOpenRadiusY = 41;
constexpr int kEyeClosedRadiusY = 6;
// How much extra ry a fully-loud (audio_level_ == 1.0) moment adds on top of
// kEyeOpenRadiusY -- kept small relative to kEyeOpenRadiusY so the eyes
// widen noticeably with voice without ever looking like a different shape
// or colliding with each other (kEyeCenterY has plenty of clearance above
// kEyeOpenRadiusY + this already).
constexpr int kEyeAudioReactiveMaxExtraRadiusY = 14;
// Eyes are rounded-corner squares ("squircles"), not ellipses -- corner
// radius clamped inside FillRoundedRect() against whatever ry currently is,
// so a blink (ry == kEyeClosedRadiusY, much smaller than this) degrades to
// a fully-rounded thin capsule instead of a broken/degenerate rect.
constexpr int kEyeCornerRadius = 14;

// "w" mouth (UwU-style), stamped as 4 short line strokes below the eyes --
// stays static regardless of state/audio level, drawn every DrawEyes() call.
constexpr int kMouthCenterX = 120; // midpoint between kLeftEyeCenterX/kRightEyeCenterX
constexpr int kMouthCenterY = 270; // clear of the eyes even at max audio-reactive size
constexpr int kMouthHalfWidth = 20;
constexpr int kMouthDip = 12;
constexpr int kMouthStrokeRadius = 3;

constexpr uint16_t kBackgroundColor = 0x0000; // black
constexpr uint16_t kEyeColor = 0xFFFF;         // white
constexpr uint16_t kTextColor = 0xFFFF;        // white

// 8-bit-per-channel -> RGB565, for the dashboard tile palette below (kept in
// this one 8-bit-friendly place rather than hand-computing 565 constants).
constexpr uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr int kBlinkIntervalMs = 3000; // how long eyes stay open between blinks
constexpr int kBlinkDurationMs = 150;  // how long the closed/blink pose holds
constexpr int kIdlePollMs = 200;       // how often the AnimationTask loop ticks
constexpr int kDashboardRefreshMs = 4000; // how often the dashboard re-reads sensors + redraws

// Pre-wrapped for the real Noto Sans metrics at these exact sizes (see
// scripts/gen_font.py --preview) -- a generic runtime word-wrap isn't worth
// building for two static strings. Baseline Y = the preview's top-of-line Y
// (Pillow's default anchor) + that size's ascent (26 for 24px, 20 for 18px).
constexpr const char* kPairingTitleLines[] = {"Dùng ứng dụng", "Synora để cài đặt", "thiết bị"};
constexpr int kPairingTitleBaselineY[] = {46, 76, 106};
constexpr const char* kPairingHintLines[] = {"Mở ứng dụng Synora trên", "điện thoại để ghép nối", "thiết bị"};
constexpr int kPairingHintBaselineY[] = {140, 164, 188};
constexpr int kQrTopY = 202;

// Shown instead of the pairing screen once CheckWifiLossRecovery() resumes
// BLE for an already-paired device that's lost WiFi -- same 3-line layout
// (kPairingTitleBaselineY/kPairingHintBaselineY apply to both) and same
// generic app-download QR, just re-pairing/reconnect framing instead of
// first-time-setup framing.
constexpr const char* kWifiLostTitleLines[] = {"Mất kết nối WiFi", "Mở ứng dụng Synora", "để kết nối lại"};
constexpr const char* kWifiLostHintLines[] = {"Nhập lại mật khẩu WiFi", "hoặc đợi thiết bị tự", "kết nối lại"};

// Dashboard, top to bottom: a "SYNORA" + leaf header, a hero block (clock +
// big soil-moisture % + label -- soil moisture picked as the single most
// actionable reading), a 2x2 grid of smaller colored sensor tiles (all four
// readings, including soil moisture again -- the grid is the "everything at
// a glance" view, the hero is "the one thing that matters most"), and a
// bottom auto-watering status bar. Tile fill colors reflect TileStatus (see
// dashboard_status.h); icons are simple vector shapes from the same
// primitives as the eyes/watering-drops/bell -- no bitmap/icon asset
// pipeline needed.
constexpr uint16_t kTileColorGood = Rgb565(56, 176, 108);     // green -- healthy range
constexpr uint16_t kTileColorWarn = Rgb565(230, 168, 46);     // amber -- borderline
constexpr uint16_t kTileColorBad = Rgb565(224, 82, 82);       // coral red -- needs attention
constexpr uint16_t kTileColorUnknown = Rgb565(58, 62, 70);    // neutral slate -- no reading yet
constexpr uint16_t kTileContentColor = 0xFFFF;                // icon/value/label -- white on every tile color above

// Fixed per-icon theme colors -- the icon glyph itself always draws in its
// own theme color regardless of status (the status color instead lives on
// the value text, see DrawSensorTile), so each tile keeps a consistent,
// recognizable icon color rather than one that shifts with the reading.
constexpr uint16_t kIconColorSoilMoisture = Rgb565(66, 133, 244);  // blue
constexpr uint16_t kIconColorTemperature = Rgb565(255, 152, 0);   // orange
constexpr uint16_t kIconColorHumidity = Rgb565(129, 212, 250);    // light blue
constexpr uint16_t kIconColorWaterLevel = Rgb565(13, 71, 161);    // deep blue
constexpr uint16_t kIconColorBell = Rgb565(255, 193, 7);          // yellow

// Header.
constexpr const char* kHeaderTitle = "SYNORA";
constexpr int kHeaderTitleBaselineY = 28;
constexpr int kHeaderLeafOffsetX = 14;  // gap from the title's right edge to the logo icon's left edge
constexpr int kHeaderLogoTopY = 3;
constexpr int kHeaderDividerY = 38;
constexpr int kWifiIconX = 10;  // top-left corner, bottom-aligned with the title text baseline

// Hero: the current time, big -- kFontGlyphs64 is rasterized at its own
// native 64px size (see scripts/gen_font.py's FONT_SIZES), not scaled up
// from a smaller table, so its curves stay smooth at this size.
constexpr int kHeroClockBaselineY = 130;
constexpr int kHeroDividerY = 162;

// Bottom auto-watering status bar -- declared here (rather than down by
// DrawDashboardScreen's other auto-water constants) since the sensor grid
// below needs kAutoWaterDividerY as its own bottom edge.
constexpr int kAutoWaterDividerY = 372;
constexpr int kAutoWaterBarBaselineY = 392;

// 2x2 sensor grid: no card backgrounds, no gutters -- 4 equal rects filling
// the space between the hero divider and the auto-water divider exactly,
// split by one vertical + one horizontal line (see DrawDashboardScreen).
constexpr int kDashboardGridTopY = kHeroDividerY;
constexpr int kDashboardGridBottomY = kAutoWaterDividerY;
constexpr int kDashboardGridHeight = kDashboardGridBottomY - kDashboardGridTopY;
constexpr int kDashboardRowHeight = kDashboardGridHeight / 2;

// Tile center X/Y for column 0/1, row 0/1 -- the midpoint of each equal
// quadrant, computed once here rather than re-deriving inline at each of
// the four DrawSensorTile() calls.
constexpr int kDashboardTileCenterX[] = {DISPLAY_WIDTH / 4, DISPLAY_WIDTH * 3 / 4};
constexpr int kDashboardTileCenterY[] = {
    kDashboardGridTopY + kDashboardRowHeight / 2,
    kDashboardGridTopY + kDashboardRowHeight + kDashboardRowHeight / 2,
};

// Icon center sits near the top of the tile; value/label baselines below it,
// all relative to the tile's own center Y so DrawSensorTile stays a single
// reusable call. Tighter than a full-screen tile would allow -- this grid is
// the secondary "everything at a glance" view, not the hero.
constexpr int kTileIconOffsetY = -22;
constexpr int kTileValueBaselineOffsetY = 18;
constexpr int kTileLabelBaselineOffsetY = 40;

constexpr int kFontTestLeftX = 4;
constexpr int kFontTestTopBaselineY = 20;

// Watering: 3 drops fall in a loop (top -> bottom, then wrap), phase-offset
// so they don't all move in lockstep -- same "recompute position from a
// running frame counter" approach as the eyes' blink, just continuous
// instead of two discrete poses. Positions/speeds are randomized fresh each
// time watering starts (see RandomizeWateringDrops()) rather than fixed, so
// the "rain" looks organic instead of mechanically synchronized.
constexpr const char* kWateringTitle = "Đang tưới nước...";
constexpr int kWateringTitleBaselineY = 60;
constexpr int kWateringDropMarginX = 30; // keep the drop icon clear of the screen edges
constexpr int kWateringDropTopY = 140;
constexpr int kWateringDropBottomY = 340;
constexpr int kWateringDropMinCycleTicks = 15; // 15-30 ticks (~3-6s @ kIdlePollMs) per fall, per drop
constexpr int kWateringDropMaxCycleTicks = 30;

// Ping ("find my device"): a bell whose clapper sways side to side.
constexpr const char* kPingTitle = "Tìm thiết bị";
constexpr int kPingTitleBaselineY = 60;
constexpr int kPingDurationMs = 3000; // roughly matches find_me_led_.Blink(4)'s total pattern length
constexpr int kBellCenterX = 120;
constexpr int kBellCenterY = 220;

// Shared shrinking progress bar for both transient screens -- a dim track
// at full width, with a brighter fill on top that shrinks from full width
// to nothing as the screen's remaining time runs out.
constexpr uint16_t kProgressTrackColor = 0x39E7; // dim gray
constexpr uint16_t kDividerColor = Rgb565(110, 115, 120); // dashboard separators -- clearly visible head-on, not just at an angle
constexpr int kProgressBarX = 20;
constexpr int kProgressBarWidth = 200;
constexpr int kProgressBarHeight = 10;
constexpr int kProgressBarY = 370;

struct RegisterEntry {
    uint16_t index;
    uint16_t value;
};
constexpr uint16_t kDelayMarker = 0xFFFF;

// Ported verbatim from the vendor's own ST7793 reference driver (TFT_eSPI's
// ST7793_Init.h, from the module's demo_ST7793 sketch), plus the three
// register writes its begin() applies afterwards via setRotation(0) --
// gamma/power/frame-rate values here are analog tuning parameters, not
// documented chip defaults, so this exact sequence (validated by the module
// vendor on real silicon) is used as-is rather than derived from the
// datasheet's abstract register descriptions.
constexpr RegisterEntry kInitSequence[] = {
    {0x0000, 0x0000}, {0x0000, 0x0000}, {0x0000, 0x0000}, {0x0000, 0x0000},
    {kDelayMarker, 15},
    {0x0400, 0x6200},  // NL=0x31 (49) -> 400 rows
    {0x0008, 0x0808},
    // Gamma
    {0x0300, 0x0C00}, {0x0301, 0x5A0B}, {0x0302, 0x0906}, {0x0303, 0x1017},
    {0x0304, 0x2300}, {0x0305, 0x1700}, {0x0306, 0x6309}, {0x0307, 0x0C09},
    {0x0308, 0x100C}, {0x0309, 0x2232},
    {0x0010, 0x0016}, {0x0011, 0x0101}, {0x0012, 0x0000}, {0x0013, 0x0001},
    {0x0100, 0x0330}, {0x0101, 0x0237}, {0x0103, 0x0D00}, {0x0280, 0x6100},
    {0x0102, 0xC1B0},
    {kDelayMarker, 50},
    {0x0001, 0x0100}, {0x0002, 0x0100},
    {0x0003, 0x10B0}, {0x0003, 0x10B8},
    {0x0009, 0x0001}, {0x000C, 0x0000}, {0x0090, 0x8000}, {0x000F, 0x0000},
    // Full-screen window: 240 cols x 400 rows
    {0x0210, 0x0000}, {0x0211, 0x00EF}, {0x0212, 0x0000}, {0x0213, 0x018F},
    {0x0500, 0x0000}, {0x0501, 0x0000}, {0x0502, 0x005F},
    {0x0401, 0x0001},  // REV=1
    {0x0404, 0x0000},
    {kDelayMarker, 50},
    // ST7793-specific tuning, gated by the extended-register unlock (R0FFh)
    {0x00FF, 0x0001},
    {0x0102, 0x01B0}, {0x0710, 0x0013}, {0x0712, 0x001F},
    {0x0752, 0x002F}, {0x0724, 0x001A},
    {0x00FF, 0x0000},
    {0x0007, 0x0100},  // BASEE=1: display on
    {kDelayMarker, 50},
    {0x0200, 0x0000}, {0x0201, 0x0000},
    // Rotation-0/portrait entry mode + driver output control -- the vendor
    // driver's begin() applies these (via its own setRotation(0)) after the
    // table above, and they override that table's own R400h/R003h writes.
    {0x0400, 0x6200},
    {0x0001, 0x0100},
    {0x0003, 0x1030},
};
}  // namespace

SynoraDisplay& SynoraDisplay::GetInstance() {
    static SynoraDisplay instance;
    return instance;
}

esp_err_t SynoraDisplay::Init(SynoraBoard* board) {
    board_ = board;

    // 240x400x16bpp (~190KB) is too big to reliably fit alongside the rest of
    // the app in internal SRAM; the board has octal PSRAM, so allocate the
    // framebuffer there. The I80 driver DMA-transfers from PSRAM itself, so
    // it needs DMA capability, not just SPIRAM capacity.
    framebuffer_ = static_cast<uint16_t*>(heap_caps_malloc(
        DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    if (framebuffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        return ESP_ERR_NO_MEM;
    }

    InitializeBus();
    esp_err_t err = InitializePanel();
    if (err != ESP_OK) {
        return err;
    }

    DrawEyes(kEyeClosedRadiusY); // Closed/idle until something sets a different state.
    xTaskCreate(AnimationTask, "synora_eyes", 4096, this, 3, nullptr);
    return ESP_OK;
}

void SynoraDisplay::InitializeBus() {
    esp_lcd_i80_bus_config_t bus_config = {};
    bus_config.clk_src = LCD_CLK_SRC_DEFAULT;
    bus_config.dc_gpio_num = DISPLAY_DC_GPIO;
    bus_config.wr_gpio_num = DISPLAY_WR_GPIO;
    bus_config.data_gpio_nums[0] = DISPLAY_D0_GPIO;
    bus_config.data_gpio_nums[1] = DISPLAY_D1_GPIO;
    bus_config.data_gpio_nums[2] = DISPLAY_D2_GPIO;
    bus_config.data_gpio_nums[3] = DISPLAY_D3_GPIO;
    bus_config.data_gpio_nums[4] = DISPLAY_D4_GPIO;
    bus_config.data_gpio_nums[5] = DISPLAY_D5_GPIO;
    bus_config.data_gpio_nums[6] = DISPLAY_D6_GPIO;
    bus_config.data_gpio_nums[7] = DISPLAY_D7_GPIO;
    bus_config.bus_width = 8;
    bus_config.max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus_));
}

void SynoraDisplay::ResetPanel() {
    gpio_config_t rst_config = {};
    rst_config.pin_bit_mask = 1ULL << DISPLAY_RST_GPIO;
    rst_config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst_config);

    gpio_set_level(DISPLAY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // >= TRESL (ST7793 datasheet: 1ms min)
    gpio_set_level(DISPLAY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10)); // >= TREST + RC-oscillator stabilization (1ms min each)
}

void SynoraDisplay::WriteReg(uint16_t index, uint16_t value) {
    // ST7793's 8-bit-bus protocol needs its (16-bit) register value sent
    // upper-byte-first. Don't pre-swap the bytes here: esp_lcd's i80 driver
    // already auto-swizzles the param buffer whenever lcd_param_bits (16,
    // below) exceeds the bus width (8) -- unconditionally, regardless of the
    // swap_color_bytes flag (that one only governs tx_color). Passing value
    // in its natural host byte order and letting that automatic swizzle
    // reorder it is correct; pre-swapping on top of it double-swaps and
    // corrupts every register write.
    esp_lcd_panel_io_tx_param(panel_io_, index, &value, sizeof(value));
}

esp_err_t SynoraDisplay::InitializePanel() {
    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_GPIO;
    io_config.pclk_hz = 2 * 1000 * 1000; // conservative for first bring-up; raise once verified on hardware
    io_config.trans_queue_depth = 10;
    io_config.dc_levels.dc_idle_level = 0;
    io_config.dc_levels.dc_cmd_level = 0;  // DCX=0: register index (ST7793's own DCX definition)
    io_config.dc_levels.dc_dummy_level = 0;
    io_config.dc_levels.dc_data_level = 1; // DCX=1: register value or pixel data
    io_config.flags.swap_color_bytes = 1;  // pixel data also goes out upper-byte-first
    io_config.lcd_cmd_bits = 16;   // ST7793's extended registers (e.g. R400h) are 16 bits, not 8
    io_config.lcd_param_bits = 16; // ...and so are their values
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus_, &io_config, &panel_io_));

    ResetPanel();

    for (const auto& entry : kInitSequence) {
        if (entry.index == kDelayMarker) {
            vTaskDelay(pdMS_TO_TICKS(entry.value));
        } else {
            WriteReg(entry.index, entry.value);
        }
    }
    return ESP_OK;
}

void SynoraDisplay::PushFramebuffer() {
    WriteReg(0x0200, 0); // DRAM cursor: column 0
    WriteReg(0x0201, 0); // DRAM cursor: row 0
    esp_lcd_panel_io_tx_color(panel_io_, 0x0202, framebuffer_, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
}

// Plain 1bpp bitmap blit (rows MSB-first, byte-padded -- same packing as
// GlyphBitmap in text_renderer.h) for the download QR code, which has no
// glyph metrics of its own to reuse BlitGlyph's exact signature.
void SynoraDisplay::BlitMonoBitmap(int x, int y, const uint8_t* bitmap, int width, int height, uint16_t color) {
    int row_bytes = (width + 7) / 8;
    for (int row = 0; row < height; row++) {
        int py = y + row;
        if (py < 0 || py >= DISPLAY_HEIGHT) {
            continue;
        }
        const uint8_t* row_data = bitmap + row * row_bytes;
        for (int col = 0; col < width; col++) {
            int px = x + col;
            if (px < 0 || px >= DISPLAY_WIDTH) {
                continue;
            }
            uint8_t byte = row_data[col / 8];
            bool set = (byte >> (7 - (col % 8))) & 1;
            if (set) {
                framebuffer_[py * DISPLAY_WIDTH + px] = color;
            }
        }
    }
}

// Simple bounds-clipped rectangle fill -- everything else on this display
// is either an ellipse (eyes/drops/bell) or a glyph/QR blit; the shrinking
// progress bar is the first thing that wants a plain rectangle, so this
// stays a private helper here rather than growing eyes_geometry.h's public
// (host-tested) surface for a single non-animated shape.
void SynoraDisplay::FillRect(int x, int y, int width, int height, uint16_t color) {
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
            framebuffer_[py * DISPLAY_WIDTH + px] = color;
        }
    }
}

// Union of two bands (full-height minus the corners, full-width minus the
// corners) plus a circle at each inset corner -- rounds the corners without
// needing an actual arc-fill primitive. corner_radius is clamped against
// whatever half_width/half_height currently are, so a very short/thin
// rectangle (e.g. a mid-blink eye) degrades to a fully-rounded capsule
// instead of a negative-size FillRect or overlapping corner circles.
void SynoraDisplay::FillRoundedRect(int cx, int cy, int half_width, int half_height, int corner_radius,
                                     uint16_t color) {
    int r = std::min({corner_radius, half_width, half_height});
    int x = cx - half_width;
    int y = cy - half_height;
    int w = half_width * 2;
    int h = half_height * 2;

    FillRect(x + r, y, w - 2 * r, h, color);
    FillRect(x, y + r, w, h - 2 * r, color);
    FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x + r, y + r, r, r, color);
    FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x + w - r, y + r, r, r, color);
    FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x + r, y + h - r, r, r, color);
    FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x + w - r, y + h - r, r, r, color);
}

// Stamps filled circles along the segment at ~1px steps -- a simple, cheap
// stand-in for a real line rasterizer, fine at this resolution/thickness.
void SynoraDisplay::DrawLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
    if (steps == 0) {
        FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x0, y0, thickness, thickness, color);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;
        FillEllipse(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, x, y, thickness, thickness, color);
    }
}

// Simple single-arc smile (corners up, center down) approximated as short
// straight segments along a parabola -- a gentler, friendlier look than the
// old "UwU" double-wave mouth (see the reference plant-companion face).
void SynoraDisplay::DrawMouth() {
    constexpr int kSegments = 8;
    int cx = kMouthCenterX;
    int cy = kMouthCenterY;
    int hw = kMouthHalfWidth;

    int prev_x = cx - hw;
    int prev_y = cy;
    for (int i = 1; i <= kSegments; i++) {
        int x = cx - hw + (2 * hw * i) / kSegments;
        float t = static_cast<float>(x - cx) / hw;  // -1 (left corner) .. 0 (center) .. 1 (right corner)
        int y = cy + static_cast<int>(kMouthDip * (1.0f - t * t));
        DrawLine(prev_x, prev_y, x, y, kMouthStrokeRadius, kEyeColor);
        prev_x = x;
        prev_y = y;
    }
}

void SynoraDisplay::DrawEyes(int ry) {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    FillRoundedRect(kLeftEyeCenterX, kEyeCenterY, kEyeRadiusX, ry, kEyeCornerRadius, kEyeColor);
    FillRoundedRect(kRightEyeCenterX, kEyeCenterY, kEyeRadiusX, ry, kEyeCornerRadius, kEyeColor);
    DrawMouth();
    PushFramebuffer();
}

// Shared by DrawPairingScreen()/DrawWifiLostScreen() -- both are a 3-line
// title + 3-line hint + the same generic app-download QR, just with
// different copy. title_line_count/hint_line_count must each be 3, matching
// kPairingTitleBaselineY/kPairingHintBaselineY (the only baseline tables
// that exist, since both current callers use exactly 3 lines each).
void SynoraDisplay::DrawPairingStyleScreen(const char* const* title_lines, int title_line_count,
                                           const char* const* hint_lines, int hint_line_count) {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    for (int i = 0; i < title_line_count; i++) {
        int width = MeasureTextWidth(title_lines[i], kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
        DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2, kPairingTitleBaselineY[i],
                 title_lines[i], kTextColor, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
    }
    for (int i = 0; i < hint_line_count; i++) {
        int width = MeasureTextWidth(hint_lines[i], kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
        DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2, kPairingHintBaselineY[i],
                 hint_lines[i], kTextColor, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
    }
    BlitMonoBitmap((DISPLAY_WIDTH - kQrDownloadWidth) / 2, kQrTopY, kQrDownloadBitmap, kQrDownloadWidth,
                   kQrDownloadHeight, kTextColor);

    PushFramebuffer();
}

void SynoraDisplay::DrawPairingScreen() {
    DrawPairingStyleScreen(kPairingTitleLines, sizeof(kPairingTitleLines) / sizeof(kPairingTitleLines[0]),
                           kPairingHintLines, sizeof(kPairingHintLines) / sizeof(kPairingHintLines[0]));
}

void SynoraDisplay::DrawWifiLostScreen() {
    DrawPairingStyleScreen(kWifiLostTitleLines, sizeof(kWifiLostTitleLines) / sizeof(kWifiLostTitleLines[0]),
                           kWifiLostHintLines, sizeof(kWifiLostHintLines) / sizeof(kWifiLostHintLines[0]));
}

namespace {
// "--" for a sensor that hasn't produced a valid reading yet (or ever) --
// deliberately not a blocking wait for the first read; see AnimationTask.
void FormatReading(char* out, size_t out_size, bool valid, float value, const char* unit) {
    if (!valid) {
        std::snprintf(out, out_size, "--");
    } else {
        std::snprintf(out, out_size, "%.1f%s", value, unit);
    }
}

uint16_t TileColorForStatus(TileStatus status) {
    switch (status) {
        case TileStatus::kGood:
            return kTileColorGood;
        case TileStatus::kWarn:
            return kTileColorWarn;
        case TileStatus::kBad:
            return kTileColorBad;
        default:
            return kTileColorUnknown;
    }
}
}  // namespace

// The four dashboard tile icons -- real art (svgs/*.svg, thresholded to 1bpp
// by scripts/gen_sensor_icons.py), not hand-drawn primitives, unlike the
// eyes/mouth/watering-drops/bell-before-this-pass. (cx, cy) is the icon's
// center; BlitMonoBitmap wants a top-left corner, hence the "- size / 2".
void SynoraDisplay::DrawThermometerIcon(int cx, int cy, uint16_t /*color*/) {
    BlitMonoBitmap(cx - kIconTemperatureSize / 2, cy - kIconTemperatureSize / 2, kIconTemperatureBitmap,
                   kIconTemperatureSize, kIconTemperatureSize, kIconColorTemperature);
}

// Water-drop bitmap -- soil moisture tile.
void SynoraDisplay::DrawDropIcon(int cx, int cy, uint16_t /*color*/) {
    BlitMonoBitmap(cx - kIconSoilMoistureSize / 2, cy - kIconSoilMoistureSize / 2, kIconSoilMoistureBitmap,
                   kIconSoilMoistureSize, kIconSoilMoistureSize, kIconColorSoilMoisture);
}

// Wind bitmap -- humidity tile.
void SynoraDisplay::DrawWindIcon(int cx, int cy, uint16_t /*color*/) {
    BlitMonoBitmap(cx - kIconHumiditySize / 2, cy - kIconHumiditySize / 2, kIconHumidityBitmap, kIconHumiditySize,
                   kIconHumiditySize, kIconColorHumidity);
}

// Wave bitmap -- water level tile.
void SynoraDisplay::DrawWaveIcon(int cx, int cy, uint16_t /*color*/) {
    BlitMonoBitmap(cx - kIconWaterLevelSize / 2, cy - kIconWaterLevelSize / 2, kIconWaterLevelBitmap,
                   kIconWaterLevelSize, kIconWaterLevelSize, kIconColorWaterLevel);
}

// Top-left header WiFi status icon: 3 ascending signal-strength bars (not a
// wifi-arcs glyph -- plain FillRect bars stay legible at this small a size,
// no new hollow-circle/arc primitive needed), bottom-aligned at baseline_y,
// colored via TileColorForStatus(board_->GetWifiStatus()) so it reuses the
// same green/amber/red semantics as the sensor tiles.
void SynoraDisplay::DrawWifiStatusIcon(int x, int baseline_y, uint16_t color) {
    constexpr int kBarWidth = 3;
    constexpr int kBarGap = 2;
    constexpr int kBarHeights[3] = {4, 7, 10};
    for (int i = 0; i < 3; ++i) {
        int bar_height = kBarHeights[i];
        FillRect(x + i * (kBarWidth + kBarGap), baseline_y - bar_height, kBarWidth, bar_height, color);
    }
}

// One dashboard tile: rounded-rect card in `status`'s color, an icon (drawn
// via the given member-function pointer -- DrawThermometerIcon/DrawDropIcon/
// DrawWindIcon/DrawWaveIcon all share this (cx, cy, color) signature) and
// centered value/label text underneath, all in kTileContentColor for
// contrast against every tile color in the palette above.
void SynoraDisplay::DrawSensorTile(int cx, int cy, TileStatus status, const char* label, const char* value,
                                   void (SynoraDisplay::*draw_icon)(int, int, uint16_t)) {
    // No card background -- just icon + value colored by status (the
    // at-a-glance status signal now lives in that color, not a filled tile)
    // and a plain label, separated from its neighbors by grid divider lines
    // (see DrawDashboardScreen) rather than by tile edges.
    uint16_t status_color = TileColorForStatus(status);
    (this->*draw_icon)(cx, cy + kTileIconOffsetY, status_color);

    int value_width = MeasureTextWidth(value, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, cx - value_width / 2, cy + kTileValueBaselineOffsetY, value,
             status_color, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);

    int label_width = MeasureTextWidth(label, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, cx - label_width / 2, cy + kTileLabelBaselineOffsetY, label,
             kTextColor, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
}

void SynoraDisplay::DrawDashboardScreen() {
    SensorReadings readings = board_->GetSensorReadings();

    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    // Header: brand + leaf, no back-arrow/gear -- this device has no
    // touchscreen (one BOOT button), so neither would do anything.
    int header_width = MeasureTextWidth(kHeaderTitle, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
    int header_left = (DISPLAY_WIDTH - header_width) / 2;
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, header_left, kHeaderTitleBaselineY, kHeaderTitle, kTextColor,
             kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
    BlitMonoBitmap(header_left + header_width + kHeaderLeafOffsetX, kHeaderLogoTopY, kLogoIconBitmap, kLogoIconWidth,
                   kLogoIconHeight, kTileColorGood);
    DrawWifiStatusIcon(kWifiIconX, kHeaderTitleBaselineY, TileColorForStatus(board_->GetWifiStatus()));
    FillRect(0, kHeaderDividerY, DISPLAY_WIDTH, 1, kDividerColor);

    // Hero: the current time, big -- "--:--" until the server has ever
    // pushed a time (see watering_schedule.h's IsTimeValid and
    // WateringScheduler::ApplyServerTime). No status color here (unlike the
    // sensor tiles below) -- a clock isn't "good/warn/bad".
    char clock_str[8];
    time_t now = time(nullptr);
    if (IsTimeValid(now)) {
        struct tm utc_time;  // ApplyServerTime stores local time as if it were UTC -- see its own doc comment
        gmtime_r(&now, &utc_time);
        std::snprintf(clock_str, sizeof(clock_str), "%02d:%02d", utc_time.tm_hour, utc_time.tm_min);
    } else {
        std::snprintf(clock_str, sizeof(clock_str), "--:--");
    }
    int clock_width = MeasureTextWidth(clock_str, kFontGlyphs64, kFontGlyphCount64, kFontLineHeight64);
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - clock_width) / 2, kHeroClockBaselineY,
             clock_str, kTextColor, kFontGlyphs64, kFontGlyphCount64, kFontLineHeight64);
    FillRect(0, kHeroDividerY, DISPLAY_WIDTH, 1, kDividerColor);

    // 2x2 grid: all four readings, including soil moisture (no longer
    // hero-only) -- this is the "everything at a glance" view. Order:
    // top-left soil, top-right temperature, bottom-left humidity,
    // bottom-right water level. No tile card backgrounds -- a "+" of divider
    // lines separates the four instead (matching the header/hero dividers).
    FillRect(DISPLAY_WIDTH / 2, kDashboardGridTopY, 1, kDashboardGridHeight, kDividerColor);
    FillRect(0, kDashboardGridTopY + kDashboardRowHeight, DISPLAY_WIDTH, 1, kDividerColor);

    char value[16];

    FormatReading(value, sizeof(value), readings.soil_moisture_valid, readings.soil_moisture_percent, "%");
    DrawSensorTile(kDashboardTileCenterX[0], kDashboardTileCenterY[0],
                   SoilMoistureTileStatus(readings.soil_moisture_valid, readings.soil_moisture_percent), "Đất", value,
                   &SynoraDisplay::DrawDropIcon);

    FormatReading(value, sizeof(value), readings.temperature_valid, readings.temperature_celsius, "°C");
    DrawSensorTile(kDashboardTileCenterX[1], kDashboardTileCenterY[0],
                   TemperatureTileStatus(readings.temperature_valid, readings.temperature_celsius), "Nhiệt độ", value,
                   &SynoraDisplay::DrawThermometerIcon);

    FormatReading(value, sizeof(value), readings.humidity_valid, readings.humidity_percent, "%");
    DrawSensorTile(kDashboardTileCenterX[0], kDashboardTileCenterY[1],
                   HumidityTileStatus(readings.humidity_valid, readings.humidity_percent), "Độ ẩm", value,
                   &SynoraDisplay::DrawWindIcon);

    FormatReading(value, sizeof(value), readings.water_level_valid, readings.water_level_percent, "%");
    DrawSensorTile(kDashboardTileCenterX[1], kDashboardTileCenterY[1],
                   WaterLevelTileStatus(readings.water_level_valid, readings.water_level_percent), "Nước", value,
                   &SynoraDisplay::DrawWaveIcon);

    // Divider above the bottom auto-watering status bar, same as the
    // header/hero separators.
    FillRect(0, kAutoWaterDividerY, DISPLAY_WIDTH, 1, kDividerColor);

    // Bottom auto-watering status bar -- reflects whether the device has any
    // enabled schedule (server-pushed, persisted locally; see
    // SynoraBoard::IsAutoWateringEnabled/WateringScheduler).
    bool auto_watering = board_->IsAutoWateringEnabled();
    const char* auto_water_text = auto_watering ? "TỰ ĐỘNG TƯỚI: BẬT" : "TỰ ĐỘNG TƯỚI: TẮT";
    int auto_water_width = MeasureTextWidth(auto_water_text, kFontGlyphs14, kFontGlyphCount14, kFontLineHeight14);
    int auto_water_left = (DISPLAY_WIDTH - auto_water_width) / 2;
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, auto_water_left, kAutoWaterBarBaselineY, auto_water_text,
             kTextColor, kFontGlyphs14, kFontGlyphCount14, kFontLineHeight14);

    PushFramebuffer();
}

// Every glyph the 18px font has for a Vietnamese vowel + tone combination
// (see gen_font.py's vietnamese_test_string()), word-wrapped to fill the
// screen -- unlike DrawPairingScreen/DrawDashboardScreen's fixed, pre-
// measured lines, this string's length isn't known ahead of time, hence
// DrawWrappedText instead of hand-placed baselines.
void SynoraDisplay::DrawFontTestScreen() {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    DrawWrappedText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, kFontTestLeftX, kFontTestTopBaselineY,
                    kVietnameseTestString, kTextColor, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
    PushFramebuffer();
}

// Falling-drops animation, redrawn every AnimationTask tick (unlike the
// static pairing/font-test screens) since frame keeps advancing the whole
// time this screen is showing. remaining_fraction (1.0 -> 0.0 over the
// pump's run) drives the shrinking progress bar underneath.
void SynoraDisplay::DrawWateringScreen(int frame, float remaining_fraction) {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    int width = MeasureTextWidth(kWateringTitle, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2, kWateringTitleBaselineY,
             kWateringTitle, kTextColor, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);

    for (const auto& drop : watering_drops_) {
        int phase = (frame + drop.start_offset_ticks) % drop.cycle_ticks;
        int y = kWateringDropTopY + (kWateringDropBottomY - kWateringDropTopY) * phase / drop.cycle_ticks;
        BlitMonoBitmap(drop.x - kIconSoilMoistureSize / 2, y - kIconSoilMoistureSize / 2, kIconSoilMoistureBitmap,
                       kIconSoilMoistureSize, kIconSoilMoistureSize, kIconColorWaterLevel);
    }

    FillRect(kProgressBarX, kProgressBarY, kProgressBarWidth, kProgressBarHeight, kProgressTrackColor);
    FillRect(kProgressBarX, kProgressBarY, static_cast<int>(kProgressBarWidth * remaining_fraction),
             kProgressBarHeight, kTextColor);

    PushFramebuffer();
}

// Picks a fresh random column/fall-speed/phase for every drop -- called once
// per watering screen entry (AnimationTask, on kWatering's screen_changed),
// not per frame, so a given watering pulse has one randomized "rain" layout
// rather than reshuffling every tick.
void SynoraDisplay::RandomizeWateringDrops() {
    for (auto& drop : watering_drops_) {
        drop.x = kWateringDropMarginX +
                 static_cast<int>(esp_random() % (DISPLAY_WIDTH - 2 * kWateringDropMarginX));
        drop.cycle_ticks = kWateringDropMinCycleTicks +
                            static_cast<int>(esp_random() % (kWateringDropMaxCycleTicks - kWateringDropMinCycleTicks + 1));
        drop.start_offset_ticks = static_cast<int>(esp_random() % drop.cycle_ticks);
    }
}

void SynoraDisplay::DrawPingScreen(float remaining_fraction) {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    int width = MeasureTextWidth(kPingTitle, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
    DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2, kPingTitleBaselineY, kPingTitle,
             kTextColor, kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);

    // Real bell art (svgs/bell-ring.svg), static -- no more side-to-side sway.
    BlitMonoBitmap(kBellCenterX - kIconBellSize / 2, kBellCenterY - kIconBellSize / 2, kIconBellBitmap, kIconBellSize,
                   kIconBellSize, kIconColorBell);

    FillRect(kProgressBarX, kProgressBarY, kProgressBarWidth, kProgressBarHeight, kProgressTrackColor);
    FillRect(kProgressBarX, kProgressBarY, static_cast<int>(kProgressBarWidth * remaining_fraction),
             kProgressBarHeight, kTextColor);

    PushFramebuffer();
}

void SynoraDisplay::SetState(EyesState state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = state;
}

void SynoraDisplay::ToggleFontTest() {
    bool now_active = !font_test_active_.load();
    font_test_active_.store(now_active);
    ESP_LOGI(TAG, "Font test screen %s", now_active ? "ON" : "OFF");
}

void SynoraDisplay::SetAudioLevel(float level) {
    audio_level_.store(level);
}

void SynoraDisplay::ShowWatering(int duration_ms) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    watering_until_ms_.store(now_ms + duration_ms);
    watering_total_ms_.store(duration_ms);
}

void SynoraDisplay::ShowPing() {
    int64_t now_ms = esp_timer_get_time() / 1000;
    ping_until_ms_.store(now_ms + kPingDurationMs);
}

EyesState SynoraDisplay::GetState() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

// Font-test wins over everything (debug override, only ever on because
// someone at a UART console just asked for it). Watering/ping are next --
// transient actuator feedback that's worth interrupting even an active
// voice session for, but only for their own short duration, after which
// they fall through to eyes/pairing/dashboard exactly as before. An active
// voice session otherwise always wins over pairing/dashboard -- derived
// fresh every call rather than stored, so there's no separate "force back
// to eyes" step needed when SetState() runs.
DisplayScreen SynoraDisplay::GetScreen() {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (font_test_active_.load()) {
        return DisplayScreen::kFontTest;
    }
    if (now_ms < watering_until_ms_.load()) {
        return DisplayScreen::kWatering;
    }
    if (now_ms < ping_until_ms_.load()) {
        return DisplayScreen::kPing;
    }
    if (GetState() != EyesState::kIdle) {
        return DisplayScreen::kEyes;
    }
    if (!DeviceActivation::GetInstance().IsPaired()) {
        return DisplayScreen::kPairing;
    }
    if (board_->IsWifiLostRecoveryActive()) {
        return DisplayScreen::kWifiLost;
    }
    return DisplayScreen::kDashboard;
}

void SynoraDisplay::AnimationTask(void* arg) {
    auto* self = static_cast<SynoraDisplay*>(arg);
    DisplayScreen last_screen = DisplayScreen::kEyes; // matches Init()'s initial DrawEyes(false)
    bool eyes_open = false;
    int64_t next_blink_change_ms = 0;
    int64_t next_dashboard_refresh_ms = 0;
    int last_drawn_minute = -1; // -1 so the very first dashboard draw always counts as "changed"
    int animation_frame = 0; // drives the watering screen's falling-drops animation; reset whenever it becomes active

    while (true) {
        DisplayScreen screen = self->GetScreen();
        bool screen_changed = (screen != last_screen);
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (screen == DisplayScreen::kFontTest) {
            if (screen_changed) {
                self->DrawFontTestScreen();
            }
        } else if (screen == DisplayScreen::kWatering) {
            if (screen_changed) {
                animation_frame = 0;
                self->RandomizeWateringDrops();
            }
            int64_t total_ms = self->watering_total_ms_.load();
            float remaining = total_ms > 0
                ? std::clamp(static_cast<float>(self->watering_until_ms_.load() - now_ms) / total_ms, 0.0f, 1.0f)
                : 0.0f;
            self->DrawWateringScreen(animation_frame++, remaining);
        } else if (screen == DisplayScreen::kPing) {
            float remaining =
                std::clamp(static_cast<float>(self->ping_until_ms_.load() - now_ms) / kPingDurationMs, 0.0f, 1.0f);
            self->DrawPingScreen(remaining);
        } else if (screen == DisplayScreen::kPairing) {
            if (screen_changed) {
                self->DrawPairingScreen();
            }
        } else if (screen == DisplayScreen::kWifiLost) {
            if (screen_changed) {
                self->DrawWifiLostScreen();
            }
        } else if (screen == DisplayScreen::kDashboard) {
            // The hero clock has its own minute-boundary timer, independent
            // of the (much coarser, sensor-driven) kDashboardRefreshMs poll
            // below -- so the displayed HH:MM never lags more than one
            // AnimationTask tick behind a real minute change, even if the
            // sensor refresh interval hasn't elapsed yet.
            time_t now_time = time(nullptr);
            struct tm now_tm;
            gmtime_r(&now_time, &now_tm);
            int current_minute = now_tm.tm_hour * 60 + now_tm.tm_min;
            bool minute_changed = current_minute != last_drawn_minute;

            if (screen_changed || now_ms >= next_dashboard_refresh_ms || minute_changed) {
                self->board_->RefreshSensors();
                self->DrawDashboardScreen();
                next_dashboard_refresh_ms = now_ms + kDashboardRefreshMs;
                last_drawn_minute = current_minute;
            }
        } else {  // DisplayScreen::kEyes
            EyesState state = self->GetState();
            if (state == EyesState::kIdle) {
                if (eyes_open) {
                    self->DrawEyes(kEyeClosedRadiusY);
                    eyes_open = false;
                }
            } else {
                if (now_ms >= next_blink_change_ms) {
                    // next_blink_change_ms starts at (and, after any idle
                    // period, remains) in the past, so the first tick after
                    // becoming active opens the eyes immediately rather
                    // than waiting out a stale blink schedule.
                    eyes_open = !eyes_open;
                    next_blink_change_ms = now_ms + (eyes_open ? kBlinkIntervalMs : kBlinkDurationMs);
                }
                // Redrawn every tick (not just on the open/closed flip
                // above) so eye size keeps tracking SetAudioLevel() while
                // open between blinks -- a blink still always fully closes
                // regardless of volume.
                int ry = eyes_open ? kEyeOpenRadiusY + static_cast<int>(self->audio_level_.load() *
                                                                         kEyeAudioReactiveMaxExtraRadiusY)
                                    : kEyeClosedRadiusY;
                self->DrawEyes(ry);
            }
        }

        last_screen = screen;
        vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
    }
}
