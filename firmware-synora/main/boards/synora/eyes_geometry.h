#pragma once

#include <cstdint>

// Pure geometry, no ESP-IDF/hardware dependency -- kept separate from
// synora_display.cc (which owns the actual panel/framebuffer) specifically
// so it can be exercised by a plain host unit test (see firmware-synora/tests/).
//
// Fills an axis-aligned ellipse centered at (cx, cy) with radii (rx, ry) into
// a fb_width x fb_height RGB565 framebuffer. Pixels outside the framebuffer
// bounds are silently clipped. A radius <= 0 draws nothing.
void FillEllipse(uint16_t* framebuffer, int fb_width, int fb_height, int cx, int cy, int rx, int ry, uint16_t color);
