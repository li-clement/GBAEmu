#pragma once

#include "Bus.h"
#include <array>
#include <cstdint>
#include <memory>

namespace Core {

// Processor Modes
enum class Mode : uint32_t {
  User = 0x10,
  FIQ = 0x11,
  IRQ = 0x12,
  Supervisor = 0x13,
  Abort = 0x17,
  Undefined = 0x1B,
  System = 0x1F
};

// Condition Codes
enum class Condition {
  EQ,
  NE,
  CS,
  CC,
  MI,
  PL,
  VS,
  VC,
  HI,
  LS,
  GE,
  LT,
  GT,
  LE,
  AL,
  NV
};

class CPU {
public:
  CPU(std::shared_ptr<Bus> bus);
  ~CPU();

  void reset();
  uint32_t getPC() const { return registers[15]; }
  bool isIRQEnabled() const { return (cpsr & 0x80) == 0; }
  bool isHalted() const { return halted; }
  void setHalted(bool h) { halted = h; }
  void setHasBIOS(bool b) { hasBIOS_ = b; }
  void
  setEntryPoint(uint32_t addr); // 设置 PC 并重填流水线（有 BIOS 时从 0 启动）
  void step();

  void irq();

  // Registers
  // R0-R12: General purpose
  // R13: SP (Banked)
  // R14: LR (Banked)
  // R15: PC
  // CPSR: Current Program Status Register
  // SPSR: Saved Program Status Register (Banked)

  uint32_t getRegister(int reg) const;
  void setRegister(int reg, uint32_t value);

  uint32_t getCPSR() const { return cpsr; }
  void setCPSR(uint32_t val);
  void setPC(uint32_t val);

private:
  std::shared_ptr<Bus> bus;

  std::array<uint32_t, 16> registers; // Current visible registers
  uint32_t cpsr;
  uint32_t spsr;

  // Banked registers:
  // R13 and R14 are banked in: FIQ, IRQ, SVC, Abort, Undef
  // R8-R12 are banked in: FIQ
  // SPSR is banked in: FIQ, IRQ, SVC, Abort, Undef
  struct {
    uint32_t r8_r12[5];
    uint32_t r13, r14;
    uint32_t spsr;
  } banks[6]; // User/System(0), FIQ(1), IRQ(2), SVC(3), Abort(4), Undef(5)

  int getBankIndex(uint32_t mode) const;
  void switchMode(uint32_t newMode);
  void reloadPipeline();

  uint32_t pipeline[2];
  bool pipelineFlushed;
  bool halted = false;   // SWI IntrWait/VBlankIntrWait 暂停 CPU
  bool hasBIOS_ = false; // 是否加载了真实 BIOS

  // Instruction decoding
  void executeARM(uint32_t opcode);
  void executeThumb(uint16_t opcode); // Later

  bool checkCondition(uint32_t cond);

  uint32_t barrelShifter(uint32_t val, uint8_t shiftType, uint8_t shiftAmount,
                         bool &carryOut, bool immediateShift);

  // Instruction groups
  void opDataProcessing(uint32_t opcode);
  void opMultiply(uint32_t opcode);
  void opSWI(uint32_t opcode);
  void opMRS(uint32_t opcode);
  void opMSR(uint32_t opcode);
  void opBranch(uint32_t opcode);
  void opLoadStore(uint32_t opcode);
  void opBlockDataTransfer(uint32_t opcode);

  // HLE BIOS Helpers
  void hleCpuSet(uint32_t src, uint32_t dst, uint32_t control);
  void hleCpuFastSet(uint32_t src, uint32_t dst, uint32_t control);
  void hleLZ77UnComp(uint32_t src, uint32_t dst);
};

} // namespace Core
