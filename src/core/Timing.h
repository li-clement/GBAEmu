#pragma once

// GBA Timing Constants
// Clock: 16.78 MHz (2^24)
// Screen Refresh: 59.73 Hz
// Cycles per dot: 4
// Dots per line: 308 (240 visible + 68 HBlank)
// Lines per frame: 228 (160 visible + 68 VBlank)
// Cycles per line: 308 * 4 = 1232
// Cycles per frame: 228 * 1232 = 280896

namespace Core {
constexpr int CYCLES_PER_DOT = 4;
constexpr int DOTS_PER_LINE = 308;
constexpr int CYCLES_PER_LINE = DOTS_PER_LINE * CYCLES_PER_DOT; // 1232
constexpr int VISIBLE_LINES = 160;
constexpr int VBLANK_LINES = 68;
constexpr int TOTAL_LINES = 228;
constexpr int CYCLES_PER_FRAME = TOTAL_LINES * CYCLES_PER_LINE; // 280896
} // namespace Core
