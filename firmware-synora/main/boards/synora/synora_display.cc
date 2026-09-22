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
#include "status_icons.h"
#include "synora_board.h"
#include "text_renderer.h"
#include "watering_schedule.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
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
// stretched). Shifted up from the original 240x400 canvas's vertical center
// (was kEyeCenterY=200) on the shorter 240x320 canvas, to leave room below
// the mouth for the multi-line subtitle text (see kSubtitleTopY in
// DrawEyes()) without losing it off the bottom edge.
constexpr int kEyeCenterY = 150;
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
constexpr int kMouthCenterY = 220; // clear of the eyes even at max audio-reactive size (kEyeCenterY moved up to 150, see above)
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
constexpr int kBootLogoDurationMs = 1500; // how long the startup splash holds before Init() hands off

// Pre-wrapped for the real Noto Sans metrics at these exact sizes (see
// scripts/gen_font.py --preview) -- a generic runtime word-wrap isn't worth
// building for two static strings. Baseline Y = the preview's top-of-line Y
// (Pillow's default anchor) + that size's ascent (26 for 24px, 20 for 18px).
constexpr const char* kPairingTitleLines[] = {"Dùng ứng dụng", "Synora để cài đặt", "thiết bị"};
constexpr int kPairingTitleBaselineY[] = {26, 50, 74};
constexpr const char* kPairingHintLines[] = {"Mở ứng dụng Synora trên", "điện thoại để ghép nối", "thiết bị"};
// Below the QR code rather than above it (kQrTopY=80, 185px tall -> bottom
// 265) -- on the shorter 240x320 canvas there isn't room for title + hint +
// QR stacked at their original (240x400) sizes/spacing, so the hint moved
// under the QR and dropped to the smaller 14px font to fit the remaining
// space above the bottom edge (320). Verify on real hardware that Vietnamese
// descenders on the last line (baseline 316) aren't clipped.
constexpr int kPairingHintBaselineY[] = {284, 300, 316};
constexpr int kQrTopY = 80;

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
constexpr int kMuteIconX = 34;  // to the right of the WiFi icon (18px wide) plus a gap, same baseline

// Hero: the current time, big -- kFontGlyphs64 is rasterized at its own
// native 64px size (see scripts/gen_font.py's FONT_SIZES), not scaled up
// from a smaller table, so its curves stay smooth at this size.
constexpr int kHeroClockBaselineY = 130;
constexpr int kHeroDividerY = 162;

// Bottom auto-watering status bar -- declared here (rather than down by
// DrawDashboardScreen's other auto-water constants) since the sensor grid
// below needs kAutoWaterDividerY as its own bottom edge. Compressed from the
// original 240x400 canvas's 372/392 to fit the shorter 240x320 canvas (still
// leaves a 12px margin below the footer text's baseline).
constexpr int kAutoWaterDividerY = 290;
constexpr int kAutoWaterBarBaselineY = 308;

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
// the secondary "everything at a glance" view, not the hero. Scaled down from
// the original 240x400 canvas's -22/18/40 (which assumed a 105px-tall row) to
// fit the shorter 240x320 canvas's smaller kDashboardRowHeight (~64px).
constexpr int kTileIconOffsetY = -14;
constexpr int kTileValueBaselineOffsetY = 11;
constexpr int kTileLabelBaselineOffsetY = 24;

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
constexpr int kWateringDropBottomY = 270; // was 340 on the taller 240x400 canvas; stays clear of kProgressBarY below
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
constexpr int kProgressBarY = 290; // was 370 on the taller 240x400 canvas

}  // namespace

SynoraDisplay& SynoraDisplay::GetInstance() {
    static SynoraDisplay instance;
    return instance;
}

esp_err_t SynoraDisplay::Init(SynoraBoard* board) {
    board_ = board;

    // 240x320x16bpp (~150KB) is too big to reliably fit alongside the rest of
    // the app in internal SRAM; the board has octal PSRAM, so allocate the
    // framebuffer there. The SPI driver DMA-transfers from PSRAM itself, so
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

    DrawBootLogoScreen();
    vTaskDelay(pdMS_TO_TICKS(kBootLogoDurationMs));

    DrawEyes(kEyeClosedRadiusY); // Closed/idle until something sets a different state.
    xTaskCreate(AnimationTask, "synora_eyes", 4096, this, 3, nullptr);
    return ESP_OK;
}

// Startup splash -- white background (every other screen is black; this one
// is deliberately inverted to read as a clean brand moment) with the green
// brand-mark logo (logo.svg, rasterized bigger than the header's kLogoIcon
// by scripts/gen_logo.py) centered. One-shot, blocking: shown for
// kBootLogoDurationMs before Init() hands off to AnimationTask's normal
// screen logic, not part of the DisplayScreen/GetScreen() rotation.
void SynoraDisplay::DrawBootLogoScreen() {
    std::fill(framebuffer_, framebuffer_ + DISPLAY_WIDTH * DISPLAY_HEIGHT, static_cast<uint16_t>(0xFFFF));
    BlitMonoBitmap((DISPLAY_WIDTH - kBootLogoWidth) / 2, (DISPLAY_HEIGHT - kBootLogoHeight) / 2, kBootLogoBitmap,
                   kBootLogoWidth, kBootLogoHeight, kTileColorGood);
    PushFramebuffer();
}

void SynoraDisplay::InitializeBus() {
    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = DISPLAY_SCK_GPIO;
    bus_config.mosi_io_num = DISPLAY_MOSI_GPIO;
    bus_config.miso_io_num = -1; // panel is write-only
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
}

esp_err_t SynoraDisplay::InitializePanel() {
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_GPIO;
    io_config.dc_gpio_num = DISPLAY_DC_GPIO;
    io_config.spi_mode = 0;
    // 20MHz over jumper wires/breadboard is a common cause of torn/garbled
    // frames (SPI signal integrity, not a software bug) -- dropped to 10MHz
    // for reliable bring-up; raise once the wiring is confirmed solid
    // (short leads, common ground) and frames come through clean.
    io_config.pclk_hz = 10 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(DISPLAY_SPI_HOST), &io_config,
                                             &panel_io_));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_GPIO;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR; // most ILI9341 modules are wired BGR; flip to RGB if colors look swapped on real hardware
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io_, &panel_config, &panel_));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
    // Confirmed on real hardware: without swap_xy, this module's native
    // orientation is landscape (the driver's default MADCTL doesn't match
    // our assumed 240-wide x 320-tall portrait mapping), so text rendered
    // along DISPLAY_WIDTH came out running along the panel's physical
    // vertical axis instead. swap_xy(true) rotates it back to portrait, and
    // mirroring both axes on top of that is a 180-degree flip -- also
    // confirmed needed on real hardware (image was upside-down otherwise).
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false)); // flip to true if colors look inverted on real hardware
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
    return ESP_OK;
}

void SynoraDisplay::PushFramebuffer() {
    // Pushed in row-bands rather than as one 240x320 (153600-byte) transfer.
    // framebuffer_ lives in PSRAM, so the SPI driver has to bounce each
    // transfer through a temporary internal-DRAM buffer -- with the whole
    // frame as a single transaction, that bounce allocation was failing
    // ("E spi_master: setup_dma_priv_buffer: Failed to allocate priv TX
    // buffer" on real hardware) since WiFi/BT/esp-sr/audio all compete for
    // the same limited internal DRAM, leaving only a partial frame drawn and
    // the rest of the panel showing stale/noise GRAM content. 40-row bands
    // (240*40*2 = 19200 bytes each, 320/40 = 8 bands) keep each bounce
    // allocation small enough to reliably succeed alongside those.
    constexpr int kBandHeight = 40;
    for (int y = 0; y < DISPLAY_HEIGHT; y += kBandHeight) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, DISPLAY_WIDTH, y + kBandHeight, framebuffer_ + y * DISPLAY_WIDTH);
    }
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

    std::string subtitle = GetSubtitle();
    if (!subtitle.empty()) {
        constexpr int kSubtitleMarginX = 16;
        constexpr int kSubtitleTopY = 250;  // clear of kMouthCenterY (220) + its dip/stroke radius
        DrawWrappedText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, kSubtitleMarginX, kSubtitleTopY,
                        subtitle.c_str(), kTextColor, kFontGlyphs18, kFontGlyphCount18, kFontLineHeight18);
    }

    PushFramebuffer();
}

// Shared by DrawPairingScreen()/DrawWifiLostScreen()/DrawConnectingWifiScreen().
// With a hint (DrawWifiLostScreen: 3-line title + 3-line hint + the generic
// app-download QR, title_line_count/hint_line_count matching
// kPairingTitleBaselineY/kPairingHintBaselineY's fixed near-top placement),
// title and hint sit above where the QR fills the rest of the screen. With
// hint_line_count == 0 (DrawPairingScreen()/DrawConnectingWifiScreen() --
// title only, no phone-pairing flow left to point at once a device is
// unpaired, or mid-Wi-Fi-connect), the title block is vertically centered on
// the panel instead, since there's nothing below it to lay out around.
void SynoraDisplay::DrawPairingStyleScreen(const char* const* title_lines, int title_line_count,
                                           const char* const* hint_lines, int hint_line_count) {
    std::memset(framebuffer_, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    if (hint_line_count > 0) {
        for (int i = 0; i < title_line_count; i++) {
            int width = MeasureTextWidth(title_lines[i], kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
            DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2,
                     kPairingTitleBaselineY[i], title_lines[i], kTextColor, kFontGlyphs24, kFontGlyphCount24,
                     kFontLineHeight24);
        }
        for (int i = 0; i < hint_line_count; i++) {
            int width = MeasureTextWidth(hint_lines[i], kFontGlyphs14, kFontGlyphCount14, kFontLineHeight14);
            DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2,
                     kPairingHintBaselineY[i], hint_lines[i], kTextColor, kFontGlyphs14, kFontGlyphCount14,
                     kFontLineHeight14);
        }
        BlitMonoBitmap((DISPLAY_WIDTH - kQrDownloadWidth) / 2, kQrTopY, kQrDownloadBitmap, kQrDownloadWidth,
                       kQrDownloadHeight, kTextColor);
    } else {
        constexpr int kTitleLineSpacing = 30;  // matches kPairingTitleBaselineY's own step
        int first_baseline = DISPLAY_HEIGHT / 2 - (title_line_count - 1) * kTitleLineSpacing / 2;
        for (int i = 0; i < title_line_count; i++) {
            int width = MeasureTextWidth(title_lines[i], kFontGlyphs24, kFontGlyphCount24, kFontLineHeight24);
            DrawText(framebuffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT, (DISPLAY_WIDTH - width) / 2,
                     first_baseline + i * kTitleLineSpacing, title_lines[i], kTextColor, kFontGlyphs24,
                     kFontGlyphCount24, kFontLineHeight24);
        }
    }

    PushFramebuffer();
}

void SynoraDisplay::DrawPairingScreen() {
    DrawPairingStyleScreen(kPairingTitleLines, sizeof(kPairingTitleLines) / sizeof(kPairingTitleLines[0]),
                           nullptr, 0);
}

// Reuses DrawPairingStyleScreen's title-only layout (see DrawPairingScreen()
// above) for a 2-line dynamic title instead of the usual static 3-line
// array -- "title_line_count=2" only touches kPairingTitleBaselineY[0]/[1],
// well within that array's bounds.
void SynoraDisplay::DrawConnectingWifiScreen() {
    std::string ssid;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ssid = connecting_ssid_;
    }
    const char* title_lines[] = {"Đang kết nối WiFi", ssid.c_str()};
    DrawPairingStyleScreen(title_lines, 2, nullptr, 0);
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

// Top-left header WiFi status icon -- real art (svgs/wifi*.svg, thresholded
// to 1bpp by scripts/gen_status_icons.py), one glyph per TileStatus rather
// than a single shape recolored: plain arcs for connected, arcs +
// exclamation for scanning/connecting, arcs + slash for disconnected. Tinted
// via TileColorForStatus so it still reuses the sensor tiles' green/amber/
// red semantics. x/baseline_y is the glyph's left edge / bottom point,
// matching the old hand-drawn bars' bottom-aligned placement.
void SynoraDisplay::DrawWifiStatusIcon(int x, int baseline_y, TileStatus status) {
    const uint8_t* bitmap;
    switch (status) {
        case TileStatus::kGood:
            bitmap = kIconWifiGoodBitmap;
            break;
        case TileStatus::kWarn:
            bitmap = kIconWifiWarnBitmap;
            break;
        default:  // kBad
            bitmap = kIconWifiBadBitmap;
            break;
    }
    BlitMonoBitmap(x, baseline_y - kIconWifiGoodSize, bitmap, kIconWifiGoodSize, kIconWifiGoodSize,
                   TileColorForStatus(status));
}

// Header speaker/mute icon -- same real-art approach as the WiFi icon above
// (svgs/speaker-2.svg unmuted, svgs/speaker-cross.svg muted -- a genuinely
// different glyph, not a slash overlaid on the same shape). muted is
// server-driven (Device.Muted -- see MqttService.PublishStateAsync's
// "muted" field, consumed by SynoraMqtt::HandleState -- and
// SynoraBoard::IsMuted()); the glyph turns kTileColorBad instead of the
// header's neutral white to make an unexpectedly-muted device obvious at a
// glance, not just on close reading. x/baseline_y matches
// DrawWifiStatusIcon's placement convention.
void SynoraDisplay::DrawMuteStatusIcon(int x, int baseline_y, bool muted) {
    const uint8_t* bitmap = muted ? kIconSpeakerMutedBitmap : kIconSpeakerUnmutedBitmap;
    uint16_t color = muted ? kTileColorBad : kTextColor;
    BlitMonoBitmap(x, baseline_y - kIconSpeakerUnmutedSize, bitmap, kIconSpeakerUnmutedSize, kIconSpeakerUnmutedSize,
                   color);
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
    DrawWifiStatusIcon(kWifiIconX, kHeaderTitleBaselineY, board_->GetWifiStatus());
    DrawMuteStatusIcon(kMuteIconX, kHeaderTitleBaselineY, board_->IsMuted());
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

void SynoraDisplay::SetSubtitle(const std::string& text) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subtitle_ = text;
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

void SynoraDisplay::ShowConnectingToWifi(const std::string& ssid) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connecting_ssid_ = ssid;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    // Comfortably longer than BleProvisioning::ConnectAndActivate()'s own
    // 30s connect timeout -- this is a backstop, not the primary way this
    // screen ends (ClearConnectingToWifi() below does that in the normal
    // case), so it should never fire before that timeout would have.
    constexpr int64_t kConnectingWifiMaxDurationMs = 35000;
    connecting_wifi_until_ms_.store(now_ms + kConnectingWifiMaxDurationMs);
}

void SynoraDisplay::ClearConnectingToWifi() {
    connecting_wifi_until_ms_.store(0);
}

EyesState SynoraDisplay::GetState() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

std::string SynoraDisplay::GetSubtitle() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return subtitle_;
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
    if (now_ms < connecting_wifi_until_ms_.load()) {
        return DisplayScreen::kConnectingWifi;
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
        } else if (screen == DisplayScreen::kConnectingWifi) {
            if (screen_changed) {
                self->DrawConnectingWifiScreen();
            }
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
