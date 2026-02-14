#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace Core {

class Debugger {
public:
  static Debugger &getInstance();

  void logInstruction(uint32_t pc, uint32_t opcode, const std::uint32_t *regs,
                      uint32_t cpsr, bool thumb);
  void logBusRead(uint32_t addr, uint32_t value, int size);
  void logBusWrite(uint32_t addr, uint32_t value, int size);
  void logUnknownOpcode(uint32_t pc, uint32_t opcode, bool thumb);
  void logMessage(const std::string &msg);

  void enable(bool e) { enabled = e; }
  bool isEnabled() const { return enabled; }
  void flush();

private:
  Debugger();
  ~Debugger();

  FILE *logFile;
  std::mutex logMutex;
  bool enabled = false;
  uint32_t instructionCount = 0;
  const uint32_t MAX_INSTRUCTIONS = 50000000; // 提升至五千万步
};

} // namespace Core
