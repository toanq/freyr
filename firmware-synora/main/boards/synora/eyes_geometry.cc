#include "eyes_geometry.h"

void FillEllipse(uint16_t* framebuffer, int fb_width, int fb_height, int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx <= 0 || ry <= 0) {
        return;
    }
    for (int y = -ry; y <= ry; y++) {
        int py = cy + y;
        if (py < 0 || py >= fb_height) {
            continue;
        }
        for (int x = -rx; x <= rx; x++) {
            int px = cx + x;
            if (px < 0 || px >= fb_width) {
                continue;
            }
            // (x/rx)^2 + (y/ry)^2 <= 1, scaled to avoid floating point.
            long lhs = static_cast<long>(x) * x * ry * ry + static_cast<long>(y) * y * rx * rx;
            long rhs = static_cast<long>(rx) * rx * ry * ry;
            if (lhs <= rhs) {
                framebuffer[py * fb_width + px] = color;
            }
        }
    }
}
