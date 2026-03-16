#include "APU.h"
#include "IORegisters.h"
#include <algorithm>

namespace Core {

APU::APU() { reset(); }

APU::~APU() {}

void APU::reset() {
  sound1cnt_l = 0;
  sound1cnt_h = 0;
  sound1cnt_x = 0;

  soundcnt_l = 0;
  soundcnt_h = 0;
  soundcnt_x = 0;
  fifoA.reset();
  fifoB.reset();

  square1.enabled = false;
  square1.lengthCounter = 0;
  square1.envelopeVolume = 0;
  square1.frequency = 0;
  square1.timer = 0;
  square1.duty = 0;
  square1.dutyStep = 0;

  sampleBucket = 0;
  outputBuffer.reserve(2048); // Pre-allocate for a frame
}

void APU::step(int cycles) {
  // Downsample to 44.1kHz
  sampleBucket += cycles;
  while (sampleBucket >= CYCLES_PER_SAMPLE) {
    sampleBucket -= CYCLES_PER_SAMPLE;

    int16_t l, r;
    getSamples(&l, &r);
    outputBuffer.push_back(l);
    outputBuffer.push_back(r);
  }

  if (!square1.enabled)
    return;

  // Simple Square Wave Generation Logic
  // Timer counts down at CPU rate (16.78MHz)
  // Frequency formula: Rate = 131072 / (2048 - X)
  // Period in cycles = 16777216 / Rate = 128 * (2048 - X)

  square1.timer -= cycles;
  if (square1.timer <= 0) {
    // Reload
    square1.timer += (2048 - square1.frequency) *
                     4; // *4 for 4 cycles per tick approximation?
    // Actually formula is Period = (2048 - n) * 32?
    // GBA Audio clock is 16MHz.
    // Let's use a simpler counter for now.
    // 2048-X is the period in "GB cycles".

    square1.dutyStep = (square1.dutyStep + 1) % 8;
  }
}

void APU::getSamples(int16_t *left, int16_t *right) {
  int16_t sample = 0;

  if (square1.enabled) {
    // Duty Cycles:
    // 0: 12.5% (1 0 0 0 0 0 0 0)
    // 1: 25%   (1 1 0 0 0 0 0 0)
    // 2: 50%   (1 1 1 1 0 0 0 0)
    // 3: 75%   (1 1 1 1 1 1 0 0)

    int bit = 0;
    switch (square1.duty) {
    case 0:
      bit = (square1.dutyStep == 0);
      break;
    case 1:
      bit = (square1.dutyStep < 2);
      break;
    case 2:
      bit = (square1.dutyStep < 4);
      break;
    case 3:
      bit = (square1.dutyStep < 6);
      break;
    }

    if (bit) {
      sample = square1.envelopeVolume * 500; // Arbitrary amplification
    } else {
      sample = -square1.envelopeVolume * 500;
    }
  }

  *left = sample;
  *right = sample;

  // Mix DirectSound (FIFO A and B)
  int dsA = fifoA.lastSample;
  int dsB = fifoB.lastSample;

  int dsAVol = (soundcnt_h & 0x0004) ? 100 : 50; // Bit 2
  int dsBVol = (soundcnt_h & 0x0008) ? 100 : 50; // Bit 3

  dsA = (dsA * dsAVol) / 100;
  dsB = (dsB * dsBVol) / 100;

  int dsLeft = 0;
  int dsRight = 0;

  if (soundcnt_h & 0x0100)
    dsRight += dsA; // Bit 8
  if (soundcnt_h & 0x0200)
    dsLeft += dsA; // Bit 9
  if (soundcnt_h & 0x1000)
    dsRight += dsB; // Bit 12
  if (soundcnt_h & 0x2000)
    dsLeft += dsB; // Bit 13

  // Arbitrary scaling for 16-bit audio output
  int outL = *left + dsLeft * 64;
  int outR = *right + dsRight * 64;
  if (outL < -32768)
    outL = -32768;
  else if (outL > 32767)
    outL = 32767;
  if (outR < -32768)
    outR = -32768;
  else if (outR > 32767)
    outR = 32767;
  *left = outL;
  *right = outR;
}

uint8_t APU::read8(uint32_t addr) {
  return 0; // TODO
}

void APU::write8(uint32_t addr, uint8_t value) {
  // Handle 8-bit writes. Usually GBA uses 16-bit or 32-bit for audio regs.
  // implementation for byte-level access if needed.
}

uint16_t APU::read16(uint32_t addr) {
  return 0; // TODO
}

void APU::write16(uint32_t addr, uint16_t value) {
  uint32_t offset = addr - 0x04000000;



  switch (offset) {
  case IO::SOUND1CNT_L: // 0x60 Sweep
    sound1cnt_l = value;
    break;
  case IO::SOUND1CNT_H: // 0x62 Duty/Len/Env
    sound1cnt_h = value;
    square1.duty = (value >> 6) & 3;
    square1.envelopeVolume = (value >> 12) & 0xF;
    break;
  case IO::SOUND1CNT_X: // 0x64 Freq/Control
    sound1cnt_x = value;
    square1.frequency = value & 0x7FF;
    if (value & 0x8000) { // Initial
      square1.enabled = true;
      square1.timer = (2048 - square1.frequency) * 4;
      square1.dutyStep = 0;
      // Reload envelope, etc.
    }
    break;
  case IO::SOUNDCNT_L: // 0x80
    soundcnt_l = value;
    break;
  case IO::SOUNDCNT_H: // 0x82
    soundcnt_h = value;
    if (value & 0x0800)
      fifoA.reset();
    if (value & 0x8000)
      fifoB.reset();
    break;
  case IO::SOUNDCNT_X: // 0x84
    soundcnt_x = value;
    break;
  }
}

void APU::write32(uint32_t addr, uint32_t value) {
  uint32_t offset = addr - 0x04000000;
  if (offset == IO::FIFO_A) {
    fifoA.push32(value);
  } else if (offset == IO::FIFO_B) {
    fifoB.push32(value);
  } else {
    write16(addr, value & 0xFFFF);
    write16(addr + 2, (value >> 16) & 0xFFFF);
  }
}

void APU::onTimerOverflow(int timerId) {
  int dmaATimer = (soundcnt_h >> 10) & 1; // Bit 10
  int dmaBTimer = (soundcnt_h >> 14) & 1; // Bit 14

  if (timerId == dmaATimer) {
    fifoA.pop();
  }
  if (timerId == dmaBTimer) {
    fifoB.pop();
  }
}

} // namespace Core
