#pragma once

#include "Bus.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace Core {

class PPU {
public:
  PPU(std::shared_ptr<Bus> bus);
  ~PPU();

  // Render a single scanline (0-159)
  void renderScanline(int line, uint32_t *buffer);

private:
  std::shared_ptr<Bus> bus;

  struct PixelData {
    uint32_t color = 0;
    uint8_t priority = 4; // 0 (Highest) to 3 (Lowest), 4 is empty
    bool isSprite = false;
  };

  void renderBackgroundLayer(int line, PixelData *lineBuffer, int bgIndex);
  void renderSprites(int line, PixelData *lineBuffer);
};

} // namespace Core
