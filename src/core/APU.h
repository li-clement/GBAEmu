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
  void write32(uint32_t addr, uint32_t value);

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

  // DirectSound Registers
  uint16_t soundcnt_l; // 0x080: Master Volume
  uint16_t soundcnt_h; // 0x082: DirectSound Control
  uint16_t soundcnt_x; // 0x084: Sound Enable

  // FIFO Structures
  struct AudioFIFO {
    int8_t data[32];
    int readPos = 0;
    int writePos = 0;
    int count = 0;
    int8_t lastSample = 0;

    void push32(uint32_t val) {
      for (int i = 0; i < 4; i++) {
        if (count < 32) {
          data[writePos] = (val >> (i * 8)) & 0xFF;
          writePos = (writePos + 1) % 32;
          count++;
        }
      }
    }

    int8_t pop() {
      if (count > 0) {
        lastSample = data[readPos];
        readPos = (readPos + 1) % 32;
        count--;
      }
      return lastSample;
    }

    void reset() {
      readPos = 0;
      writePos = 0;
      count = 0;
      lastSample = 0;
    }
  };

  AudioFIFO fifoA;
  AudioFIFO fifoB;

  // Timer Override Hooks
public:
  void onTimerOverflow(int timerId);
  int fifoA_count() const { return fifoA.count; }
  int fifoB_count() const { return fifoB.count; }
  int timerForFifoA() const { return (soundcnt_h >> 10) & 1; }
  int timerForFifoB() const { return (soundcnt_h >> 14) & 1; }

private:
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
