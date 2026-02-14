#pragma once
#include "Bus.h"
#include <cstdint>
#include <vector>

namespace Core {

class APU {
public:
  APU();
  ~APU();

  void reset();
  void step(int cycles); // Advance audio state

  // Register Access
  uint8_t read8(uint32_t addr);
  void write8(uint32_t addr, uint8_t value);
  // Helpers for Bus
  uint16_t read16(uint32_t addr);
  void write16(uint32_t addr, uint16_t value);

  // Audio Output
  std::vector<int16_t> outputBuffer;
  void clearBuffer() { outputBuffer.clear(); }
  const std::vector<int16_t> &getBuffer() const { return outputBuffer; }

  // Sample Generation
  // Generates 2 samples (Left, Right) for the current time step
  void getSamples(int16_t *left, int16_t *right);

private:
  float sampleBucket;                           // Accumulator for downsampling
  static constexpr int CYCLES_PER_SAMPLE = 380; // 16.78MHz / 44100Hz ~= 380.49
  // Basic Register State
  uint16_t sound1cnt_l;
  uint16_t sound1cnt_h;
  uint16_t sound1cnt_x;

  // Channel 1 State (Square 1)
  struct SquareChannel {
    bool enabled;
    int lengthCounter;
    int envelopeStep;
    int envelopeVolume;
    int frequency;
    int timer; // Count down
    int dutyStep;
    int duty; // 0-3
  } square1;

  void updateSquare1(int cycles);
};

} // namespace Core
