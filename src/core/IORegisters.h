#pragma once
#include <cstdint>

namespace Core {
namespace IO {

// LCD Control
const uint32_t DISPCNT = 0x000;  // LCD Control
const uint32_t DISPSTAT = 0x004; // General LCD Status
const uint32_t VCOUNT = 0x006;   // Vertical Counter (Scanline)

// Background Control
const uint32_t BG0CNT = 0x008;
const uint32_t BG1CNT = 0x00A;
const uint32_t BG2CNT = 0x00C;
const uint32_t BG3CNT = 0x00E;

// Background Scrolling
const uint32_t BG0HOFS = 0x010;
const uint32_t BG0VOFS = 0x012;
const uint32_t BG1HOFS = 0x014;
const uint32_t BG1VOFS = 0x016;
const uint32_t BG2HOFS = 0x018;
const uint32_t BG2VOFS = 0x01A;
const uint32_t BG3HOFS = 0x01C;
const uint32_t BG3VOFS = 0x01E;

// Constants for Bit Manipulation
const uint16_t DISPCNT_MODE_MASK = 0x7;
const uint16_t DISPCNT_BG0_ENABLE = 0x0100;
const uint16_t DISPCNT_BG1_ENABLE = 0x0200;
const uint16_t DISPCNT_BG2_ENABLE = 0x0400;
const uint16_t DISPCNT_BG3_ENABLE = 0x0800;
const uint16_t DISPCNT_OBJ_ENABLE = 0x1000;
const uint16_t DISPCNT_OBJ_1D_MAP = 0x0040;

// Timers
const uint32_t TM0CNT_L = 0x100;
const uint32_t TM0CNT_H = 0x102;
const uint32_t TM1CNT_L = 0x104;
const uint32_t TM1CNT_H = 0x106;
const uint32_t TM2CNT_L = 0x108;
const uint32_t TM2CNT_H = 0x10A;
const uint32_t TM3CNT_L = 0x10C;
const uint32_t TM3CNT_H = 0x10E;

// Interrupts
const uint32_t IE = 0x200;  // Interrupt Enable
const uint32_t IF = 0x202;  // Interrupt Request Flags
const uint32_t IME = 0x208; // Interrupt Master Enable

// Key Input
const uint32_t KEYINPUT = 0x130;
const uint16_t KEY_A = 0x0001;
const uint16_t KEY_B = 0x0002;
const uint16_t KEY_SELECT = 0x0004;
const uint16_t KEY_START = 0x0008;
const uint16_t KEY_RIGHT = 0x0010;
const uint16_t KEY_LEFT = 0x0020;
const uint16_t KEY_UP = 0x0040;
const uint16_t KEY_DOWN = 0x0080;
const uint16_t KEY_R = 0x0100;
const uint16_t KEY_L = 0x0200;

// Audio
const uint32_t SOUND1CNT_L = 0x060;
const uint32_t SOUND1CNT_H = 0x062;
const uint32_t SOUND1CNT_X = 0x064;
const uint32_t SOUNDCNT_L = 0x080;
const uint32_t SOUNDCNT_H = 0x082;
const uint32_t SOUNDCNT_X = 0x084;
const uint32_t SOUNDBIAS = 0x088;
const uint32_t FIFO_A = 0x0A0;
const uint32_t FIFO_B = 0x0A4;

// DMA
// DMA0
const uint32_t DMA0SAD = 0x0B0;   // Source Address (32-bit internal)
const uint32_t DMA0DAD = 0x0B4;   // Dest Address (32-bit internal)
const uint32_t DMA0CNT_L = 0x0B8; // Word Count
const uint32_t DMA0CNT_H = 0x0BA; // Control

// DMA1
const uint32_t DMA1SAD = 0x0BC;
const uint32_t DMA1DAD = 0x0C0;
const uint32_t DMA1CNT_L = 0x0C4;
const uint32_t DMA1CNT_H = 0x0C6;

// DMA2
const uint32_t DMA2SAD = 0x0C8;
const uint32_t DMA2DAD = 0x0CC;
const uint32_t DMA2CNT_L = 0x0D0;
const uint32_t DMA2CNT_H = 0x0D2;

// DMA3
const uint32_t DMA3SAD = 0x0D4;
const uint32_t DMA3DAD = 0x0D8;
const uint32_t DMA3CNT_L = 0x0DC;
const uint32_t DMA3CNT_H = 0x0DE;

} // namespace IO
} // namespace Core
