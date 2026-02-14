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

  void renderBackground(int line, uint32_t *buffer);
  void renderBackgroundLayer(int line, uint32_t *buffer, int bgIndex);
  void renderSprites(int line, uint32_t *buffer);
};

} // namespace Core
