#include "Debugger.h"
#include <ctime>

namespace Core {

Debugger &Debugger::getInstance() {
  static Debugger instance;
  return instance;
}

Debugger::Debugger() {
  logFile = fopen("debug_trace.log", "w");
  if (logFile) {
    fprintf(logFile, "--- GBAEmu Trace Start ---\n");
  }
}

Debugger::~Debugger() {
  if (logFile) {
    fclose(logFile);
  }
}

void Debugger::logInstruction(uint32_t pc, uint32_t opcode,
                              const std::uint32_t *regs, uint32_t cpsr,
                              bool thumb) {
  if (!enabled || instructionCount >= MAX_INSTRUCTIONS)
    return;

  std::lock_guard<std::mutex> lock(logMutex);
  fprintf(logFile, "[%08u] PC:%08X OP:%08X %s CPSR:%08X ", instructionCount++,
          pc, opcode, thumb ? "T" : "A", cpsr);

  for (int i = 0; i < 16; i++) {
    fprintf(logFile, "R%d:%08X ", i, regs[i]);
  }
  fprintf(logFile, "\n");

  if (instructionCount % 1000 == 0)
    fflush(logFile);
}

void Debugger::logBusRead(uint32_t addr, uint32_t value, int size) {
  if (!enabled)
    return;

  // 只记录 ROM 访问或 BIOS 关键区，其余读取 (I/O, WRAM, BIOS fetch) 屏蔽以提速
  if (addr < 0x08000000)
    return;

  std::lock_guard<std::mutex> lock(logMutex);
  fprintf(logFile, "<<< Bus Read:  ADDR:%08X VAL:%08X SIZE:%d\n", addr, value,
          size);
}

void Debugger::logUnknownOpcode(uint32_t pc, uint32_t opcode, bool thumb) {
  if (!enabled)
    return;
  std::lock_guard<std::mutex> lock(logMutex);
  fprintf(logFile, "!!! Unknown %s Opcode: %08X at PC:%08X\n",
          thumb ? "Thumb" : "ARM", opcode, pc);
}

void Debugger::logBusWrite(uint32_t addr, uint32_t value, int size) {
  if (!enabled)
    return;
  std::lock_guard<std::mutex> lock(logMutex);
  fprintf(logFile, ">>> Bus Write: ADDR:%08X VAL:%08X SIZE:%d\n", addr, value,
          size);
}

void Debugger::logMessage(const std::string &msg) {
  if (!enabled)
    return;
  std::lock_guard<std::mutex> lock(logMutex);
  fprintf(logFile, "INFO: %s\n", msg.c_str());
}

void Debugger::flush() {
  if (logFile)
    fflush(logFile);
}

} // namespace Core
