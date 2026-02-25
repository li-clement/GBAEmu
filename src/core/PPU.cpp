#include "PPU.h"
#include "IORegisters.h"
#include <algorithm>
#include <cstdio>

namespace Core {

// Helper to expand 5-bit color to 8-bit
uint8_t expand5to8(uint8_t c) { return (c << 3) | (c >> 2); }

PPU::PPU(std::shared_ptr<Bus> bus) : bus(bus) {}

PPU::~PPU() {}

void PPU::renderScanline(int line, uint32_t *buffer) {
  // Backdrop from first palette entry (0x05000000)
  uint16_t backdrop15 = bus->read16(0x05000000);
  uint8_t br = expand5to8(backdrop15 & 0x1F);
  uint8_t bg = expand5to8((backdrop15 >> 5) & 0x1F);
  uint8_t bb = expand5to8((backdrop15 >> 10) & 0x1F);
  uint32_t backdrop = (0xFFu << 24) | (bb << 16) | (bg << 8) | br;
  std::fill_n(buffer, 240, backdrop);

  uint16_t dispcnt = bus->read16(0x04000000 + IO::DISPCNT);
  int mode = dispcnt & IO::DISPCNT_MODE_MASK;

  static int logCounter = 0;
  if (line == 80 && logCounter++ % 60 == 0) {
    printf("PPU: Line 80 Mode=%d CNT=%04X BG0=%d BG1=%d BG2=%d BG3=%d OBJ=%d\n",
           mode, dispcnt, (dispcnt & IO::DISPCNT_BG0_ENABLE) ? 1 : 0,
           (dispcnt & IO::DISPCNT_BG1_ENABLE) ? 1 : 0,
           (dispcnt & IO::DISPCNT_BG2_ENABLE) ? 1 : 0,
           (dispcnt & IO::DISPCNT_BG3_ENABLE) ? 1 : 0,
           (dispcnt & IO::DISPCNT_OBJ_ENABLE) ? 1 : 0);
  }

  if (mode == 0 || mode == 1) {
    PixelData lineBuffer[240];
    for (int x = 0; x < 240; x++) {
      lineBuffer[x].color = backdrop;
      lineBuffer[x].priority = 4; // Backdrop has lowest priority
    }

    // Mode 0/1: Tiled layers
    // Priority 3 is lowest (drawn first), Priority 0 is highest (drawn last).
    // For same priority, higher indexed BG has lower priority (drawn first).
    for (int p = 3; p >= 0; p--) {
      for (int bg = 3; bg >= 0; bg--) {
        if (dispcnt & (1 << (8 + bg))) {
          static const uint32_t BGCNT_OFFS[] = {IO::BG0CNT, IO::BG1CNT,
                                                IO::BG2CNT, IO::BG3CNT};
          uint16_t bgcnt = bus->read16(0x04000000 + BGCNT_OFFS[bg]);
          if ((bgcnt & 0x3) == p) { // BG priority is bits 0-1 of BGCNT
            renderBackgroundLayer(line, lineBuffer, bg);
          }
        }
      }
    }

    // Sprites on top (when OBJ enabled)
    if (dispcnt & IO::DISPCNT_OBJ_ENABLE) {
      renderSprites(line, lineBuffer);
    }

    // Composite lineBuffer back into buffer
    for (int x = 0; x < 240; x++) {
      if (lineBuffer[x].priority < 4) { // Only draw if not backdrop
        buffer[x] = lineBuffer[x].color;
      }
    }
  } else if (mode == 3) {
    // Mode 3: 240x160 15-bit bitmap, single frame at 0x06000000
    uint32_t lineBase = 0x06000000 + (line * 240 * 2);
    for (int x = 0; x < 240; x++) {
      uint16_t color15 = bus->read16(lineBase + x * 2);
      uint8_t r = expand5to8(color15 & 0x1F);
      uint8_t g = expand5to8((color15 >> 5) & 0x1F);
      uint8_t b = expand5to8((color15 >> 10) & 0x1F);
      buffer[x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
    }
  } else if (mode == 4) {
    // Mode 4: 240x160 8-bit palette, two frames; DISPCNT bit 4 = frame select
    uint32_t frameBase = (dispcnt & 0x10) ? 0x0600A000 : 0x06000000;
    uint32_t lineBase = frameBase + (line * 240);
    for (int x = 0; x < 240; x++) {
      uint8_t index = bus->read8(lineBase + x);
      if (index == 0) {
        // Transparent in Mode 4 typically shows BG color 0
      } else {
        uint16_t color15 = bus->read16(0x05000000 + (index * 2));
        uint8_t r = expand5to8(color15 & 0x1F);
        uint8_t g = expand5to8((color15 >> 5) & 0x1F);
        uint8_t b = expand5to8((color15 >> 10) & 0x1F);
        buffer[x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
      }
    }
  } else if (mode == 5) {
    // Mode 5: 160x128 15-bit, two frames; we render 160-wide centered / scaled
    // later if needed
    uint32_t frameBase = (dispcnt & 0x10) ? 0x0600A000 : 0x06000000;
    if (line < 128) {
      uint32_t lineBase = frameBase + (line * 160 * 2);
      for (int x = 0; x < 160; x++) {
        uint16_t color15 = bus->read16(lineBase + x * 2);
        uint8_t r = expand5to8(color15 & 0x1F);
        uint8_t g = expand5to8((color15 >> 5) & 0x1F);
        uint8_t b = expand5to8((color15 >> 10) & 0x1F);
        buffer[x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
      }
      // 160->240: leave right 80 pixels as black, or repeat; keep simple
    }
  }

  // Sprites on top (when OBJ enabled)
  // This block is now handled inside the mode 0/1 check for proper compositing
  // if (dispcnt & IO::DISPCNT_OBJ_ENABLE) {
  //   renderSprites(line, buffer);
  // }
}

void PPU::renderBackgroundLayer(int line, PixelData *lineBuffer, int bgIndex) {
  static const uint32_t BGCNT_OFFS[] = {IO::BG0CNT, IO::BG1CNT, IO::BG2CNT,
                                        IO::BG3CNT};
  static const uint32_t BGHOFS_OFFS[] = {IO::BG0HOFS, IO::BG1HOFS, IO::BG2HOFS,
                                         IO::BG3HOFS};
  static const uint32_t BGVOFS_OFFS[] = {IO::BG0VOFS, IO::BG1VOFS, IO::BG2VOFS,
                                         IO::BG3VOFS};
  uint32_t base = 0x04000000;
  uint16_t bgcnt = bus->read16(base + BGCNT_OFFS[bgIndex]);
  int charBaseBlock = (bgcnt >> 2) & 0x3;
  int colorMode = (bgcnt >> 7) & 1;
  int screenBaseBlock = (bgcnt >> 8) & 0x1F;
  int scx = bus->read16(base + BGHOFS_OFFS[bgIndex]) & 0x1FF;
  int scy = bus->read16(base + BGVOFS_OFFS[bgIndex]) & 0x1FF;

  uint32_t vramBase = 0x06000000;
  uint32_t mapBase = vramBase + (screenBaseBlock * 2048);
  uint32_t tileBase = vramBase + (charBaseBlock * 16384);

  int screenSize = (bgcnt >> 14) & 3;
  int width = (screenSize & 1) ? 512 : 256;
  int height = (screenSize & 2) ? 512 : 256;

  for (int x = 0; x < 240; x++) {
    int effectiveX = (x + scx) & (width - 1);
    int effectiveY = (line + scy) & (height - 1);

    int tileX = effectiveX / 8;
    int tileY = effectiveY / 8;

    int blockX = tileX / 32;
    int blockY = tileY / 32;
    int sbbOffset = 0;
    if (screenSize == 1)
      sbbOffset = blockX;
    else if (screenSize == 2)
      sbbOffset = blockY;
    else if (screenSize == 3)
      sbbOffset = blockY * 2 + blockX;

    int localTileX = tileX % 32;
    int localTileY = tileY % 32;
    int mapIndex = localTileY * 32 + localTileX;

    uint32_t currentMapBase = mapBase + (sbbOffset * 2048);
    uint16_t tileInfo = bus->read16(currentMapBase + (mapIndex * 2));

    int tileIndex = tileInfo & 0x3FF;
    int hFlip = (tileInfo >> 10) & 1;
    int vFlip = (tileInfo >> 11) & 1;
    int paletteBank = (tileInfo >> 12) & 0xF;
    int localX = effectiveX % 8;
    int localY = effectiveY % 8;
    if (hFlip)
      localX = 7 - localX;
    if (vFlip)
      localY = 7 - localY;

    uint8_t index = 0;
    if (colorMode == 0) {
      uint32_t tileDataAddr = tileBase + (tileIndex * 32);
      uint8_t byte = bus->read8(tileDataAddr + (localY * 4) + (localX / 2));
      index = (localX & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
    } else {
      index = bus->read8(tileBase + (tileIndex * 64) + (localY * 8) + localX);
    }

    if (index != 0) {
      uint32_t paletteAddr =
          colorMode == 0 ? (0x05000000 + (paletteBank * 32) + (index * 2))
                         : (0x05000000 + (index * 2));
      uint16_t color15 = bus->read16(paletteAddr);
      uint8_t r = expand5to8(color15 & 0x1F);
      uint8_t g = expand5to8((color15 >> 5) & 0x1F);
      uint8_t b = expand5to8((color15 >> 10) & 0x1F);
      uint32_t finalColor = (0xFFu << 24) | (b << 16) | (g << 8) | r;

      // Only draw if not transparent (color 0 in palette is transparent)
      // or if we have higher priority. BGs with same priority draw over each
      // other if bgIndex is lower (handled by loop order in renderScanline).
      if (lineBuffer[x].priority > (bgcnt & 3)) {
        lineBuffer[x].color = finalColor;
        lineBuffer[x].priority = bgcnt & 3;
        lineBuffer[x].isSprite = false;
      }
    }
  }
}

void PPU::renderSprites(int line, PixelData *lineBuffer) {
  uint16_t dispcnt = bus->read16(0x04000000 + IO::DISPCNT);

  // Check if OBJ is enabled
  if (!(dispcnt & IO::DISPCNT_OBJ_ENABLE))
    return;

  // Iterate over 128 OAM entries
  // OAM Base: 0x07000000
  // Each entry is 8 bytes (Attributes 0, 1, 2, filler)

  // We should iterate backwards to handle priority if we are strict,
  // but for simple implementation forward iteration + Z-buffer or just
  // painter's algorithm is a start. GBA hardware draws sprites with lower
  // priority ON TOP of higher priority? Actually simpler: Priority decides rel
  // to BG, but within sprites, lower index = higher priority (drawn on top)?
  // No, GBA renders sprites in a specific order.
  // Let's stick to simple Painter's: Iterate 0 to 127.

  // Note: This needs to handle priority vs BG, which requires a line buffer
  // with priority info. For now, we just draw on top of everything to verify.

  for (int i = 0; i < 128; i++) {
    uint32_t oamAddr = 0x07000000 + (i * 8);
    uint16_t attr0 = bus->read16(oamAddr);
    uint16_t attr1 = bus->read16(oamAddr + 2);
    uint16_t attr2 = bus->read16(oamAddr + 4);

    int y = attr0 & 0xFF;
    // int mode = (attr0 >> 8) & 0x3; // 0=Normal, 1=Semi-transparent,
    // 2=ObjWindow, 3=Invalid int gfxMode = (attr0 >> 10) & 0x3; // 0=Normal,
    // 1=Alpha, 2=Window int mosaic = (attr0 >> 12) & 1;
    int colorMode = (attr0 >> 13) & 1; // 0=4bpp, 1=8bpp
    int shape = (attr0 >> 14) & 0x3;   // 0=Square, 1=Horizontal, 2=Vertical

    int x = attr1 & 0x1FF;
    int horizontalFlip = (attr1 >> 12) & 1;
    int verticalFlip = (attr1 >> 13) & 1;
    int size = (attr1 >> 14) & 0x3;

    int tileIndex = attr2 & 0x3FF;
    // int priority = (attr2 >> 10) & 0x3;
    int paletteBank = (attr2 >> 12) & 0xF;

    // Handle Y Wrapping (0-255)
    if (y > 160)
      y -= 256;

    // Determine Width/Height based on Shape & Size
    int width = 8, height = 8;
    // Table of sizes [Shape][Size]
    static const int sizes[3][4][2] = {// Square
                                       {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                       // Horizontal
                                       {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                       // Vertical
                                       {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};

    if (shape < 3) {
      width = sizes[shape][size][0];
      height = sizes[shape][size][1];
    }

    // Check if sprite intersects current line
    if (line >= y && line < y + height) {
      // Render this sprite row
      int spriteRow = line - y;
      if (verticalFlip)
        spriteRow = height - 1 - spriteRow;

      for (int col = 0; col < width; col++) {
        int screenX = x + col;
        if (screenX >= 240)
          continue; // Clip Horizontal
        if (screenX < 0)
          continue;

        int spriteCol = col;
        if (horizontalFlip)
          spriteCol = width - 1 - spriteCol;
        int tx = spriteCol / 8;
        int ty = spriteRow / 8;

        // VRAM mapping for sprites is either 1D or 2D based on DISPCNT bit 6
        bool mapping1D = (dispcnt & IO::DISPCNT_OBJ_1D_MAP) != 0;
        int currentTileIndex = tileIndex;

        if (mapping1D) {
          // 1D mapping: tile offsets linearly
          // stride depends on color depth (1 tile is 32 bytes for 4bpp, 64
          // bytes for 8bpp) Width in tiles: width / 8
          int offset = (ty * (width / 8) + tx) * (colorMode ? 2 : 1);
          currentTileIndex = (currentTileIndex + offset) & 0x3FF;
        } else {
          // 2D mapping: VRAM organized as a 32x32 matrix of tiles
          currentTileIndex =
              (currentTileIndex + ty * 32 + tx * (colorMode ? 2 : 1)) & 0x3FF;
        }

        int lx = spriteCol % 8;
        int ly = spriteRow % 8;

        uint32_t charBase = 0x06010000;
        uint32_t tileAddr = charBase + (currentTileIndex * 32);

        uint8_t index = 0;
        if (colorMode == 0) { // 4bpp
          uint32_t pixelAddr = tileAddr + (ly * 4) + (lx / 2);
          uint8_t byte = bus->read8(pixelAddr);
          index = (lx & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
        } else { // 8bpp
          uint32_t pixelAddr =
              charBase + (currentTileIndex * 32) + (ly * 8) + lx;
          index = bus->read8(pixelAddr);
        }

        if (index != 0) {
          uint32_t palAddr =
              colorMode == 0 ? (0x05000200 + (paletteBank * 32) + (index * 2))
                             : (0x05000200 + (index * 2));
          uint16_t color15 = bus->read16(palAddr);
          uint8_t r = expand5to8(color15 & 0x1F);
          uint8_t g = expand5to8((color15 >> 5) & 0x1F);
          uint8_t b = expand5to8((color15 >> 10) & 0x1F);
          int priority = (attr2 >> 10) & 0x3;
          uint32_t finalColor = (0xFF << 24) | (b << 16) | (g << 8) | r;

          // Only draw if current pixel is empty or we have higher priority
          // Note: sprites with same priority as BG are drawn on top of BG.
          // Lower priority number = higher priority.
          if (lineBuffer[screenX].priority > priority ||
              (lineBuffer[screenX].priority == priority &&
               !lineBuffer[screenX].isSprite)) {
            lineBuffer[screenX].color = finalColor;
            lineBuffer[screenX].priority = priority;
            lineBuffer[screenX].isSprite = true;
          }
        }
      }
    }
  }
}

} // namespace Core
