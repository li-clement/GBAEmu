#pragma once

#include "APU.h"
#include "Bus.h"
#include "CPU.h"
#include "PPU.h"
#include <memory>
#include <vector>

namespace Core {

class GBA {
public:
  GBA();
  ~GBA();

  void reset();
  void step();                                     // Run one instruction
  void stepFrame(uint32_t *buffer, size_t stride); // Run a full frame

  // Input
  void setKeyStatus(uint16_t keyMask, bool pressed);

  // ROM / BIOS Loading
  void loadBIOS(const std::vector<uint8_t> &data);
  void loadROM(const std::vector<uint8_t> &data);

  // Debugging
  const CPU &getCPU() const { return *cpu; }
  Bus &getBus() { return *bus; }
  PPU &getPPU() { return *ppu; }
  APU &getAPU() { return *apu; }

private:
  std::shared_ptr<Bus> bus;
  std::unique_ptr<CPU> cpu;
  std::unique_ptr<PPU> ppu;
  std::unique_ptr<APU> apu;

  // Timers
  struct Timer {
    uint16_t reload;
    uint16_t control;
    uint16_t counter;
    int cycles; // Accumulator for cycles
  } timers[4];

  void updateTimers(int cycles);
  void requestInterrupt(int id);
  void checkInterrupts();

  // DMA
  struct DMA {
    uint32_t sad; // Internal Source
    uint32_t dad; // Internal Dest
    uint16_t count;
    uint16_t control;
    bool active;
  } dma[4];

  void checkDMA();
  void transferDMA(int channel);
  void latchDMA(int channel);

  bool hasBIOS = false;
};

} // namespace Core
