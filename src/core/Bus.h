#pragma once

#include <cstdint>
#include <vector>

namespace Core {

class APU; // Forward decl
class CPU; // Forward decl

class Bus {
public:
  Bus();
  ~Bus();

  void setAPU(APU *apu) { this->apu = apu; }
  void setCPU(CPU *cpu) { this->cpu = cpu; }

  // Basic memory access
  template <typename T> T read(uint32_t addr);

  template <typename T> void write(uint32_t addr, T value);

  // Specialized for 8/16/32 bits
  uint8_t read8(uint32_t addr);
  uint16_t read16(uint32_t addr);
  uint32_t read32(uint32_t addr);

  void write8(uint32_t addr, uint8_t value);
  void write16(uint32_t addr, uint16_t value);
  void write32(uint32_t addr, uint32_t value);

  // Special IO methods
  void setKeyInput(uint16_t value);
  void requestInterrupt(uint16_t flag);
  void lockBIOSVectorTable() { vectorTableWritable_ = false; }

  void loadBIOS(const std::vector<uint8_t> &data);
  void loadROM(const std::vector<uint8_t> &data);

private:
  std::vector<uint8_t> bios; // 16KB BIOS
  bool vectorTableWritable_ = true; // 无 BIOS 时可写 0x00-0x3F；有 BIOS 时只读
  std::vector<uint8_t> wram_board; // 256KB On-board WRAM
  std::vector<uint8_t> wram_chip;  // 32KB On-chip WRAM
  std::vector<uint8_t> io_regs;    // 1KB IO Registers
  std::vector<uint8_t> palette;    // 1KB Palette RAM
  std::vector<uint8_t> vram;       // 96KB VRAM
  std::vector<uint8_t> oam;        // 1KB OAM
  std::vector<uint8_t> rom;        // Cartridge ROM (up to 32MB)

  // Memory map regions
  // 00000000-00003FFF BIOS
  // 02000000-0203FFFF On-board WRAM
  // 03000000-03007FFF On-chip WRAM
  // 08000000-.......  Game ROM

  APU *apu = nullptr;
  CPU *cpu = nullptr;
};

} // namespace Core
