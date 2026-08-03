#include "test_util.h"

#include "../main/boards/synora/eyes_geometry.h"

#include <vector>

namespace {

std::vector<uint16_t> MakeBlankFramebuffer(int width, int height) {
    return std::vector<uint16_t>(static_cast<size_t>(width) * height, 0);
}

}  // namespace

int RunEyesGeometryTests() {
    TEST_SUITE_BEGIN();

    // Center pixel of a filled ellipse must be set.
    {
        auto fb = MakeBlankFramebuffer(128, 128);
        FillEllipse(fb.data(), 128, 128, 64, 64, 20, 25, 0xFFFF);
        TEST_CHECK_EQ(fb[64 * 128 + 64], 0xFFFF);
    }

    // A pixel well outside the ellipse's bounding box stays untouched.
    {
        auto fb = MakeBlankFramebuffer(128, 128);
        FillEllipse(fb.data(), 128, 128, 64, 64, 20, 25, 0xFFFF);
        TEST_CHECK_EQ(fb[0], 0);
        TEST_CHECK_EQ(fb[127 * 128 + 127], 0);
    }

    // A non-positive radius draws nothing (matches "closed eyes" not being a
    // negative-radius crash).
    {
        auto fb = MakeBlankFramebuffer(128, 128);
        FillEllipse(fb.data(), 128, 128, 64, 64, 0, 25, 0xFFFF);
        for (auto pixel : fb) {
            TEST_CHECK_EQ(pixel, 0);
        }
    }

    // An ellipse straddling the framebuffer edge clips instead of writing
    // out of bounds (the real regression this guards: DISPLAY_WIDTH/HEIGHT
    // used to be baked into the class instead of passed in, so an off-panel
    // eye center was never exercised in isolation).
    {
        auto fb = MakeBlankFramebuffer(128, 128);
        FillEllipse(fb.data(), 128, 128, 0, 0, 18, 22, 0xFFFF);
        TEST_CHECK_EQ(fb[0], 0xFFFF);   // top-left corner, inside the clipped quarter-ellipse
        TEST_CHECK_EQ(fb.size(), static_cast<size_t>(128) * 128);  // no out-of-bounds resize/crash
    }

    // Two non-overlapping ellipses (the actual two-eyes case) don't bleed
    // into each other.
    {
        auto fb = MakeBlankFramebuffer(128, 128);
        FillEllipse(fb.data(), 128, 128, 40, 64, 18, 22, 0xFFFF);
        FillEllipse(fb.data(), 128, 128, 88, 64, 18, 22, 0xFFFF);
        TEST_CHECK_EQ(fb[64 * 128 + 40], 0xFFFF);
        TEST_CHECK_EQ(fb[64 * 128 + 88], 0xFFFF);
        TEST_CHECK_EQ(fb[64 * 128 + 64], 0);  // midpoint between the two eyes, untouched
    }

    TEST_SUITE_END();
}
