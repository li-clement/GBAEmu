#include "PPU.h"
#include "IORegisters.h"
#include <algorithm>
#include <cstdio>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace Core {

// Helper to expand 5-bit color to 8-bit
inline uint8_t expand5to8(uint8_t c) { return (c << 3) | (c >> 2); }

#ifdef __ARM_NEON
// Vectorized expansion of 8 pixels (15-bit color) at once
// GBA 15-bit color format: 0bbbbbgggggrrrrr
inline void expand5to8_neon_8pixels(const uint16_t* src15, uint32_t* dst32) {
    // Load 8 16-bit colors into a 128-bit NEON register
    uint16x8_t color16 = vld1q_u16(src15);

    // Mask out the 5-bit channels
    uint16x8_t r_mask = vdupq_n_u16(0x001F);
    uint16x8_t g_mask = vdupq_n_u16(0x03E0);
    uint16x8_t b_mask = vdupq_n_u16(0x7C00);

    // Extract channels as 16-bit values
    uint16x8_t r16 = vandq_u16(color16, r_mask);
    uint16x8_t g16 = vshrq_n_u16(vandq_u16(color16, g_mask), 5);
    uint16x8_t b16 = vshrq_n_u16(vandq_u16(color16, b_mask), 10);

    // Expand 5-bit to 8-bit: (c << 3) | (c >> 2)
    // First, convert 16-bit to 8-bit arrays (combining two 8x8 arrays into 1)
    // But since we have 8 elements, vmovn (narrow) gives us an 8x8 vector.
    uint8x8_t r8 = vmovn_u16(r16);
    uint8x8_t g8 = vmovn_u16(g16);
    uint8x8_t b8 = vmovn_u16(b16);

    uint8x8_t r_final = vorr_u8(vshl_n_u8(r8, 3), vshr_n_u8(r8, 2));
    uint8x8_t g_final = vorr_u8(vshl_n_u8(g8, 3), vshr_n_u8(g8, 2));
    uint8x8_t b_final = vorr_u8(vshl_n_u8(b8, 3), vshr_n_u8(b8, 2));
    uint8x8_t alpha = vdup_n_u8(0xFF);

    // Interleave channels: R, G, B, A into a single 32x8 structure
    // AArch64 intrinsic to store interleaved RGBA: vst4_lane or just vst4
    uint8x8x4_t rgba;
    rgba.val[0] = r_final;  // R
    rgba.val[1] = g_final;  // G
    rgba.val[2] = b_final;  // B
    rgba.val[3] = alpha;    // A

    // Store the 8 pixels directly into the uint32_t destination
    vst4_u8(reinterpret_cast<uint8_t*>(dst32), rgba);
}
#endif

PPU::PPU(std::shared_ptr<Bus> bus) : bus(bus) {}

PPU::~PPU() {}

void PPU::renderScanline(int line, uint32_t *buffer) {
  const uint8_t* pal = bus->getPalettePointer();
  const uint8_t* vram = bus->getVRAMPointer();

  // Backdrop from first palette entry (0x05000000)
  uint16_t backdrop15 = *reinterpret_cast<const uint16_t*>(&pal[0]);
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
    uint32_t lineOffset = line * 240 * 2;
    int x = 0;
#ifdef __ARM_NEON
    // Vectorize 8 pixels at a time
    for (; x <= 240 - 8; x += 8) {
      expand5to8_neon_8pixels(reinterpret_cast<const uint16_t*>(&vram[lineOffset + x * 2]), &buffer[x]);
    }
#endif
    for (; x < 240; x++) {
      uint16_t color15 = *reinterpret_cast<const uint16_t*>(&vram[lineOffset + x * 2]);
      uint8_t r = expand5to8(color15 & 0x1F);
      uint8_t g = expand5to8((color15 >> 5) & 0x1F);
      uint8_t b = expand5to8((color15 >> 10) & 0x1F);
      buffer[x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
    }
  } else if (mode == 4) {
    // Mode 4: 240x160 8-bit palette, two frames; DISPCNT bit 4 = frame select
    uint32_t frameOffset = (dispcnt & 0x10) ? 0xA000 : 0x0000;
    uint32_t lineOffset = frameOffset + (line * 240);
    // Palette lookup is non-contiguous in memory, so NEON is harder to apply directly
    // without a gather instruction (which ARM NEON lacks efficiently for 16-bit scattered loads)
    // We will stick to the scalar approach here for Mode 4.
    for (int x = 0; x < 240; x++) {
      uint8_t index = vram[lineOffset + x];
      if (index == 0) {
        // Transparent in Mode 4 typically shows BG color 0
      } else {
        uint16_t color15 = *reinterpret_cast<const uint16_t*>(&pal[index * 2]);
        uint8_t r = expand5to8(color15 & 0x1F);
        uint8_t g = expand5to8((color15 >> 5) & 0x1F);
        uint8_t b = expand5to8((color15 >> 10) & 0x1F);
        buffer[x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
      }
    }
  } else if (mode == 5) {
    // Mode 5: 160x128 15-bit, two frames; we render 160-wide centered / scaled
    // later if needed
    uint32_t frameOffset = (dispcnt & 0x10) ? 0xA000 : 0x0000;
    if (line < 128) {
      uint32_t lineOffset = frameOffset + (line * 160 * 2);
      int x = 0;
#ifdef __ARM_NEON
      for (; x <= 160 - 8; x += 8) {
        expand5to8_neon_8pixels(reinterpret_cast<const uint16_t*>(&vram[lineOffset + x * 2]), &buffer[x]);
      }
#endif
      for (; x < 160; x++) {
        uint16_t color15 = *reinterpret_cast<const uint16_t*>(&vram[lineOffset + x * 2]);
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

  uint32_t mapOffsetBase = (screenBaseBlock * 2048);
  uint32_t tileOffsetBase = (charBaseBlock * 16384);

  const uint8_t* vram = bus->getVRAMPointer();
  const uint8_t* pal = bus->getPalettePointer();

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

    uint32_t currentMapOffset = mapOffsetBase + (sbbOffset * 2048);
    uint16_t tileInfo = *reinterpret_cast<const uint16_t*>(&vram[currentMapOffset + (mapIndex * 2)]);

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
      uint32_t tileDataOffset = tileOffsetBase + (tileIndex * 32);
      uint8_t byte = vram[tileDataOffset + (localY * 4) + (localX / 2)];
      index = (localX & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
    } else {
      index = vram[tileOffsetBase + (tileIndex * 64) + (localY * 8) + localX];
    }

    if (index != 0) {
      uint32_t palOffset =
          colorMode == 0 ? ((paletteBank * 32) + (index * 2))
                         : (index * 2);
      uint16_t color15 = *reinterpret_cast<const uint16_t*>(&pal[palOffset]);
      
      uint32_t finalColor;
#ifdef __ARM_NEON
      // Use NEON for a single pixel expansion just to utilize the logic (fallback to scalar if 1 pixel is not worth moving to vector registers)
      // Since NEON is best for 8 pixels, applying it purely for a single pixel per loop iteration adds overhead.
      // But we can manually expand it or keep the scalar for scattered non-contiguous reads.
      uint8_t r = expand5to8(color15 & 0x1F);
      uint8_t g = expand5to8((color15 >> 5) & 0x1F);
      uint8_t b = expand5to8((color15 >> 10) & 0x1F);
      finalColor = (0xFFu << 24) | (b << 16) | (g << 8) | r;
#else
      uint8_t r = expand5to8(color15 & 0x1F);
      uint8_t g = expand5to8((color15 >> 5) & 0x1F);
      uint8_t b = expand5to8((color15 >> 10) & 0x1F);
      finalColor = (0xFFu << 24) | (b << 16) | (g << 8) | r;
#endif

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

  const uint8_t* oam = bus->getOAMPointer();
  const uint8_t* vram = bus->getVRAMPointer();
  const uint8_t* pal = bus->getPalettePointer();

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
    int oamOffset = i * 8;
    uint16_t attr0 = *reinterpret_cast<const uint16_t*>(&oam[oamOffset]);
    uint16_t attr1 = *reinterpret_cast<const uint16_t*>(&oam[oamOffset + 2]);
    uint16_t attr2 = *reinterpret_cast<const uint16_t*>(&oam[oamOffset + 4]);

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

        uint32_t vramOffsetBase = 0x010000;

        uint8_t index = 0;
        if (colorMode == 0) { // 4bpp
          uint32_t pixelOffset = vramOffsetBase + (currentTileIndex * 32) + (ly * 4) + (lx / 2);
          uint8_t byte = vram[pixelOffset];
          index = (lx & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
        } else { // 8bpp
          uint32_t pixelOffset = vramOffsetBase + (currentTileIndex * 32) + (ly * 8) + lx;
          index = vram[pixelOffset];
        }

        if (index != 0) {
          uint32_t palOffset =
              colorMode == 0 ? (0x200 + (paletteBank * 32) + (index * 2))
                             : (0x200 + (index * 2));
          uint16_t color15 = *reinterpret_cast<const uint16_t*>(&pal[palOffset]);
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
