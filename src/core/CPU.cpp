#include "CPU.h"
#include "Debugger.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Core {

CPU::CPU(std::shared_ptr<Bus> bus) : bus(bus) {
  initDecodeTables();
  reset();
}

CPU::~CPU() {}

void CPU::reset() {
  // 重置所有寄存器
  for (auto &r : registers)
    r = 0;
  cpsr = 0x13; // Supervisor mode, ARM state, IRQ/FIQ disabled
  spsr = 0;

  // 初始化 Banked Registers 存储
  for (int i = 0; i < 6; i++) {
    banks[i].r13 = 0x03007F00; // 默认 SP
    banks[i].r14 = 0;
    banks[i].spsr = 0;
    for (int j = 0; j < 5; j++)
      banks[i].r8_r12[j] = 0;
  }

  // GBA 标准栈地址
  banks[0].r13 = 0x03007F00; // User/System
  banks[2].r13 = 0x03007FA0; // IRQ
  banks[3].r13 = 0x03007FE0; // SVC

  // 核心寄存器初始值 (启动时处于 SVC 模式)
  registers[13] = banks[3].r13; // SVC SP

  // PC 从 BIOS 入口开始
  registers[15] = 0x00000000;
  reloadPipeline();
}

void CPU::setEntryPoint(uint32_t addr) {
  registers[15] = addr;
  cpsr &= ~0x20; // ARM mode
  pipeline[0] = bus->read32(registers[15]);
  registers[15] += 4;
  pipeline[1] = bus->read32(registers[15]);
  registers[15] += 4;
}

int CPU::step() {
  // CPU 被 SWI IntrWait/VBlankIntrWait 暂停时，跳过执行
  if (halted)
    return 1;

  struct TraceEntry {
    uint32_t pc;
    uint32_t opc;
    uint32_t cpsr;
  };
  static TraceEntry traceBuf[200];
  static int traceIdx = 0;
  static bool crashed = false;

  if (!crashed) {
    traceBuf[traceIdx].pc = registers[15] - (cpsr & 0x20 ? 4 : 8);
    traceBuf[traceIdx].opc = pipeline[0];
    traceBuf[traceIdx].cpsr = cpsr;
    traceIdx = (traceIdx + 1) % 200;

    if (registers[15] >= 0x03000000 && registers[15] < 0x04000000 && pipeline[0] == 0) {
      crashed = true;
      FILE* fp = fopen("/tmp/gba_crash.txt", "w");
      if (fp) {
        for (int i = 0; i < 200; i++) {
          int idx = (traceIdx + i) % 200;
          fprintf(fp, "PC:%08X OPC:%08X CPSR:%X\n", traceBuf[idx].pc, traceBuf[idx].opc, traceBuf[idx].cpsr);
        }
        fclose(fp);
      }
    }
  }

  // 当前要执行的指令永远在 pipeline[0]
  uint32_t opcode = pipeline[0];

  pipelineFlushed = false;
  lastCycles_ = 1; // 默认 1 周期，各指令内部会覆写

  // 执行指令
  if (cpsr & 0x20) { // Thumb 状态
    executeThumb(opcode & 0xFFFF);
  } else { // ARM 状态
    uint32_t cond = (opcode >> 28) & 0xF;
    if (checkCondition(cond)) {
      executeARM(opcode);
    }
  }

  // 如果指令是分支跳转，reloadPipeline 已由执行函数代劳，我们不需要在此推进
  if (!pipelineFlushed) {
    // 正常推进流水线
    pipeline[0] = pipeline[1];
    if (cpsr & 0x20) { // Thumb
      pipeline[1] = bus->read16(registers[15]);
      registers[15] += 2;
    } else { // ARM
      pipeline[1] = bus->read32(registers[15]);
      registers[15] += 4;
    }
  }

  return lastCycles_;
}

void CPU::reloadPipeline() {
  uint32_t target = registers[15];
  if (cpsr & 0x20) { // Thumb
    pipeline[0] = bus->read16(target);
    registers[15] = target + 2;
    pipeline[1] = bus->read16(registers[15]);
    registers[15] += 2;
  } else { // ARM
    pipeline[0] = bus->read32(target);
    registers[15] = target + 4;
    pipeline[1] = bus->read32(registers[15]);
    registers[15] += 4;
  }
  pipelineFlushed = true;
}

bool CPU::checkCondition(uint32_t cond) {
  // Simplified flags
  bool N = (cpsr >> 31) & 1;
  bool Z = (cpsr >> 30) & 1;
  bool C = (cpsr >> 29) & 1;
  bool V = (cpsr >> 28) & 1;

  switch (cond) {
  case 0x0:
    return Z; // EQ
  case 0x1:
    return !Z; // NE
  case 0x2:
    return C; // CS
  case 0x3:
    return !C; // CC
  case 0x4:
    return N; // MI
  case 0x5:
    return !N; // PL
  case 0x6:
    return V; // VS
  case 0x7:
    return !V; // VC
  case 0x8:
    return C && !Z; // HI
  case 0x9:
    return !C || Z; // LS
  case 0xA:
    return N == V; // GE
  case 0xB:
    return N != V; // LT
  case 0xC:
    return !Z && (N == V); // GT
  case 0xD:
    return Z || (N != V); // LE
  case 0xE:
    return true; // AL
  default:
    return false;
  }
}

void CPU::executeARM(uint32_t opcode) {
  if (!checkCondition(opcode >> 28))
    return;

  // Look up instruction in precomputed table (Bits 27-16)
  uint32_t index = (opcode >> 16) & 0xFFF;
  ArithHandler handler = armTable[index];
  if (handler) {
    (this->*handler)(opcode);
    return;
  }

  // 以下都是 Bits 26-27 = 00 的指令
  // Branch and Exchange (BX): 0001 0010 xxxx xxxx xxxx 0001 xxxx
  if ((opcode & 0x0FFFFFF0) == 0x012FFF10) {
    uint8_t rn = opcode & 0xF;
    uint32_t target = registers[rn];

    if (target & 1) {
      cpsr |= 0x20; // 设置 T 位，切换到 Thumb 模式
      target &= ~1;
    } else {
      cpsr &= ~0x20;
      target &= ~3;
    }

    registers[15] = target;
    if (target == 0) {
      Debugger::getInstance().logMessage("!!! BX to ZERO at PC:" +
                                         std::to_string(registers[15]));
    }
    // 刷新流水线
    if (cpsr & 0x20) {
      pipeline[0] = bus->read16(registers[15]);
      registers[15] += 2;
      pipeline[1] = bus->read16(registers[15]);
      registers[15] += 2;
    } else {
      pipeline[0] = bus->read32(registers[15]);
      registers[15] += 4;
      pipeline[1] = bus->read32(registers[15]);
      registers[15] += 4;
    }
    pipelineFlushed = true;
    return;
  }

  // MRS: 00010 R 00 1111 Rd 0000 0000 0000
  if ((opcode & 0x0FBF0FFF) == 0x010F0000) {
    opMRS(opcode);
    return;
  }

  // MSR Register: 00010 R 10 mask 1111 0000 0000 Rm
  if ((opcode & 0x0FBFFFF0) == 0x0129F000) {
    opMSR(opcode);
    return;
  }

  // MSR Immediate: 00110 R 10 mask 1111 Rotate Imm8
  if ((opcode & 0x0DB0F000) == 0x0320F000) {
    opMSR(opcode);
    return;
  }

  // Multiply / Multiply Long: 0000 xxAS Rd Rn Rs 1001 Rm
  if (((opcode & 0x0FC000F0) == 0x00000090) ||
      ((opcode & 0x0F8000F0) == 0x00800090)) {
    opMultiply(opcode);
    return;
  }

  // Halfword Load/Store: xxxx 000x xxxx xxxx xxxx xxxx 1xx1 xxxx
  // Bits 27-25 must be 000, Bits 7 and 4 must be 1
  if (((opcode >> 25) & 0x7) == 0 && (opcode & 0x90) == 0x90 &&
      ((opcode >> 4) & 0xF) != 0x9) {
    // LDRH/STRH/LDRSB/LDRSH 等半字/有符号字节 Load/Store
    bool P = (opcode >> 24) & 1;
    bool U = (opcode >> 23) & 1;
    bool I = (opcode >> 22) & 1; // Immediate offset (1) or Register (0)
    bool W = (opcode >> 21) & 1;
    bool L = (opcode >> 20) & 1;
    uint8_t rn = (opcode >> 16) & 0xF;
    uint8_t rd = (opcode >> 12) & 0xF;
    uint8_t SH = (opcode >> 5) & 0x3; // S and H bits

    uint32_t offset;
    if (I) {
      // Immediate: high nibble | low nibble
      offset = ((opcode >> 4) & 0xF0) | (opcode & 0xF);
    } else {
      // Register
      uint8_t rm = opcode & 0xF;
      offset = registers[rm];
    }

    uint32_t base = registers[rn];
    uint32_t addr = base;

    if (P)
      addr = U ? base + offset : base - offset;

    switch (SH) {
    case 1: // Unsigned halfword
      if (L) {
        registers[rd] = bus->read16(addr);
      } else {
        bus->write16(addr, registers[rd] & 0xFFFF);
      }
      break;
    case 2: // Signed byte
      if (L) {
        int8_t val = (int8_t)bus->read8(addr);
        registers[rd] = (uint32_t)(int32_t)val;
      }
      break;
    case 3: // Signed halfword
      if (L) {
        int16_t val = (int16_t)bus->read16(addr);
        registers[rd] = (uint32_t)(int32_t)val;
      }
      break;
    }

    if (!P)
      addr = U ? base + offset : base - offset;
    if (W || !P)
      registers[rn] = addr;
    return;
  }

  // Data Processing (fallback for Bits 26-27 = 00)
  if (((opcode >> 26) & 0x3) == 0x0) {
    opDataProcessing(opcode);
    return;
  }

  Debugger::getInstance().logUnknownOpcode(registers[15] - 8, opcode, false);
}

void CPU::initDecodeTables() {
  // === ARM 指令查找表 (bits [27:16]) ===
  for (int i = 0; i < 4096; i++) {
    armTable[i] = nullptr;
  }

  for (int i = 0; i < 4096; i++) {
    int type = (i >> 8) & 0xF;

    if (type == 0xF) {
      armTable[i] = &CPU::opSWI;
    } else if ((type & 0xE) == 0xA) { // 101x
      armTable[i] = &CPU::opBranch;
    } else if ((type & 0xE) == 0x8) { // 100x
      armTable[i] = &CPU::opBlockDataTransfer;
    } else if ((type & 0xC) == 0x4) { // 01xx
      armTable[i] = &CPU::opLoadStore;
    }
  }

  // === Thumb 指令查找表 (bits [15:8]) ===
  for (int i = 0; i < 256; i++) {
    thumbTable[i] = nullptr;
  }

  for (int i = 0; i < 256; i++) {
    uint8_t hi = i;

    // SWI: 1101 1111 (0xDF)
    if (hi == 0xDF) {
      thumbTable[i] = &CPU::thumbSWI;
      continue;
    }

    // Format 19: Long Branch (1111 xxxx)
    if ((hi >> 4) == 0xF) {
      thumbTable[i] = &CPU::thumbFormat19;
      continue;
    }

    // Format 16: Conditional Branch (1101 cccc, cccc != 1111)
    if ((hi >> 4) == 0xD) {
      thumbTable[i] = &CPU::thumbFormat16;
      continue;
    }

    // Format 18: Unconditional Branch (1110 0xxx)
    if ((hi >> 3) == 0x1C || (hi >> 3) == 0x1D) {
      thumbTable[i] = &CPU::thumbFormat18;
      continue;
    }

    // Format 15: Multiple Load/Store (1100 xxxx)
    if ((hi >> 4) == 0xC) {
      thumbTable[i] = &CPU::thumbFormat15;
      continue;
    }

    // Format 11: SP-relative Load/Store (1001 xxxx)
    if ((hi >> 4) == 0x9) {
      thumbTable[i] = &CPU::thumbFormat11;
      continue;
    }

    // Format 12: Load Address (1010 xxxx)
    if ((hi >> 4) == 0xA) {
      thumbTable[i] = &CPU::thumbFormat12;
      continue;
    }

    // Format 10: Load/Store Halfword Imm (1000 xxxx)
    if ((hi >> 4) == 0x8) {
      thumbTable[i] = &CPU::thumbFormat10;
      continue;
    }

    // Format 13: Add Offset to SP (1011 0000)
    if (hi == 0xB0) {
      thumbTable[i] = &CPU::thumbFormat13;
      continue;
    }

    // Format 14: Push (1011 010x)
    if (hi == 0xB4 || hi == 0xB5) {
      thumbTable[i] = &CPU::thumbFormat14;
      continue;
    }

    // Format 14: Pop (1011 110x)
    if (hi == 0xBC || hi == 0xBD) {
      thumbTable[i] = &CPU::thumbFormat14;
      continue;
    }

    // BKPT (1011 1110)
    if (hi == 0xBE) {
      // 当作 NOP
      continue;
    }

    // Format 6: PC-relative Load (0100 1xxx)
    if ((hi >> 3) == 0x09) {
      thumbTable[i] = &CPU::thumbFormat6;
      continue;
    }

    // Format 5: Hi-Register / BX (0100 01xx)
    if ((hi >> 2) == 0x11) {
      thumbTable[i] = &CPU::thumbFormat5;
      continue;
    }

    // Format 4: ALU Operations (0100 00xx)
    if ((hi >> 2) == 0x10) {
      thumbTable[i] = &CPU::thumbFormat4;
      continue;
    }

    // Format 7/8: Load/Store Register Offset (0101 xxxx)
    if ((hi >> 4) == 0x5) {
      thumbTable[i] = &CPU::thumbFormat78;
      continue;
    }

    // Format 9: Load/Store Imm Offset (011x xxxx)
    if ((hi >> 5) == 0x3) {
      thumbTable[i] = &CPU::thumbFormat9;
      continue;
    }

    // Format 3: MOV/CMP/ADD/SUB Imm (001x xxxx)
    if ((hi >> 5) == 0x1) {
      thumbTable[i] = &CPU::thumbFormat3;
      continue;
    }

    // Format 1 & 2: Shifted Register & Add/Sub (000x xxxx)
    if ((hi >> 5) == 0x0) {
      // 00011 = Format 2 (Add/Sub)
      if ((hi >> 3) == 0x03) {
        thumbTable[i] = &CPU::thumbFormat2;
      } else {
        thumbTable[i] = &CPU::thumbFormat1;
      }
      continue;
    }
  }
}

void CPU::executeThumb(uint16_t opcode) {
  uint8_t hi = opcode >> 8;
  ThumbHandler handler = thumbTable[hi];
  if (handler) {
    (this->*handler)(opcode);
  } else {
    Debugger::getInstance().logUnknownOpcode(registers[15] - 2, opcode, true);
  }
}

void CPU::thumbSWI(uint16_t opcode) {
  opSWI(opcode);
  lastCycles_ = 3;
}

void CPU::thumbFormat1(uint16_t opcode) {
  // Format 1: Move Shifted Register — 000 Op(2) Offset5 Rs Rd
  uint8_t shiftOp = (opcode >> 11) & 0x3;
  uint8_t shiftAmount = (opcode >> 6) & 0x1F;
  uint8_t rs = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;
  uint32_t val = registers[rs];
  bool carry = (cpsr >> 29) & 1;
  uint32_t result;

  switch (shiftOp) {
  case 0: // LSL
    if (shiftAmount == 0) {
      result = val; // carry 不变
    } else {
      carry = (val >> (32 - shiftAmount)) & 1;
      result = val << shiftAmount;
    }
    break;
  case 1:                   // LSR
    if (shiftAmount == 0) { // LSR #32
      carry = (val >> 31) & 1;
      result = 0;
    } else {
      carry = (val >> (shiftAmount - 1)) & 1;
      result = val >> shiftAmount;
    }
    break;
  case 2:                   // ASR
    if (shiftAmount == 0) { // ASR #32
      carry = (val >> 31) & 1;
      result = (val & 0x80000000) ? 0xFFFFFFFF : 0;
    } else {
      carry = (val >> (shiftAmount - 1)) & 1;
      result = (uint32_t)((int32_t)val >> shiftAmount);
    }
    break;
  default:
    result = val;
    break;
  }

  registers[rd] = result;
  cpsr &= ~0xF0000000;
  if (result == 0) cpsr |= (1 << 30);
  if (result & 0x80000000) cpsr |= (1 << 31);
  if (carry) cpsr |= (1 << 29);
  lastCycles_ = 1 + 1; // shift takes an extra internal cycle
}

void CPU::thumbFormat2(uint16_t opcode) {
  // Format 2: Add/Sub (3-operand) — 00011 I Op Rn/nn Rs Rd
  bool I = (opcode >> 10) & 1;
  bool sub = (opcode >> 9) & 1;
  uint8_t rs = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;
  uint32_t valS = registers[rs];
  uint32_t valN = I ? ((opcode >> 6) & 0x7) : registers[(opcode >> 6) & 0x7];
  uint32_t result;

  if (sub) {
    result = valS - valN;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if (valS >= valN) cpsr |= (1 << 29);
    if (((valS ^ valN) & (valS ^ result)) & 0x80000000) cpsr |= (1 << 28);
  } else {
    result = valS + valN;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if ((uint64_t)valS + valN > 0xFFFFFFFF) cpsr |= (1 << 29);
    if (~(valS ^ valN) & (valS ^ result) & 0x80000000) cpsr |= (1 << 28);
  }
  registers[rd] = result;
  lastCycles_ = 1;
}

void CPU::thumbFormat3(uint16_t opcode) {
  // MOV/CMP/ADD/SUB Immediate
  uint8_t op = (opcode >> 11) & 0x3;
  uint8_t rd = (opcode >> 8) & 0x7;
  uint32_t imm = opcode & 0xFF;
  uint32_t val = registers[rd];
  uint32_t result;

  switch (op) {
  case 0: // MOV
    result = imm;
    registers[rd] = result;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    break;
  case 1: // CMP
    result = val - imm;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if (val >= imm) cpsr |= (1 << 29);
    if (((val ^ imm) & (val ^ result)) & 0x80000000) cpsr |= (1 << 28);
    break;
  case 2: // ADD
    result = val + imm;
    registers[rd] = result;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if ((uint64_t)val + imm > 0xFFFFFFFF) cpsr |= (1 << 29);
    if (~(val ^ imm) & (val ^ result) & 0x80000000) cpsr |= (1 << 28);
    break;
  case 3: // SUB
    result = val - imm;
    registers[rd] = result;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if (val >= imm) cpsr |= (1 << 29);
    if (((val ^ imm) & (val ^ result)) & 0x80000000) cpsr |= (1 << 28);
    break;
  }
  lastCycles_ = 1;
}

void CPU::thumbFormat4(uint16_t opcode) {
  // ALU Operations
  uint8_t op = (opcode >> 6) & 0xF;
  uint8_t rs = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;

  uint32_t valD = registers[rd];
  uint32_t valS = registers[rs];
  uint32_t result;
  bool carry = (cpsr >> 29) & 1;
  int cycles = 1;

  switch (op) {
  case 0x0: // AND
    result = valD & valS;
    registers[rd] = result;
    break;
  case 0x1: // EOR
    result = valD ^ valS;
    registers[rd] = result;
    break;
  case 0x2: // LSL
    cycles++;
    {
      uint8_t shift = valS & 0xFF;
      if (shift == 0) { result = valD; }
      else if (shift < 32) { carry = (valD >> (32 - shift)) & 1; result = valD << shift; }
      else if (shift == 32) { carry = valD & 1; result = 0; }
      else { carry = 0; result = 0; }
      registers[rd] = result;
    }
    break;
  case 0x3: // LSR
    cycles++;
    {
      uint8_t shift = valS & 0xFF;
      if (shift == 0) { result = valD; }
      else if (shift < 32) { carry = (valD >> (shift - 1)) & 1; result = valD >> shift; }
      else if (shift == 32) { carry = (valD >> 31) & 1; result = 0; }
      else { carry = 0; result = 0; }
      registers[rd] = result;
    }
    break;
  case 0x4: // ASR
    cycles++;
    {
      uint8_t shift = valS & 0xFF;
      if (shift == 0) { result = valD; }
      else if (shift < 32) { carry = (valD >> (shift - 1)) & 1; result = (uint32_t)((int32_t)valD >> shift); }
      else { carry = (valD >> 31) & 1; result = (valD & 0x80000000) ? 0xFFFFFFFF : 0; }
      registers[rd] = result;
    }
    break;
  case 0x5: // ADC
    {
      bool oldC = (cpsr >> 29) & 1;
      uint64_t full = (uint64_t)valD + (uint64_t)valS + oldC;
      result = (uint32_t)full;
      carry = full > 0xFFFFFFFF;
      registers[rd] = result;
      cpsr &= ~0xF0000000;
      if (result == 0) cpsr |= (1 << 30);
      if (result & 0x80000000) cpsr |= (1 << 31);
      if (carry) cpsr |= (1 << 29);
      if (~(valD ^ valS) & (valD ^ result) & 0x80000000) cpsr |= (1 << 28);
      lastCycles_ = 1;
      return;
    }
  case 0x6: // SBC
    {
      bool oldC = (cpsr >> 29) & 1;
      uint32_t sub = valS + !oldC;
      result = valD - sub;
      registers[rd] = result;
      cpsr &= ~0xF0000000;
      if (result == 0) cpsr |= (1 << 30);
      if (result & 0x80000000) cpsr |= (1 << 31);
      if ((uint64_t)valD >= (uint64_t)sub) cpsr |= (1 << 29);
      if (((valD ^ valS) & (valD ^ result)) & 0x80000000) cpsr |= (1 << 28);
      lastCycles_ = 1;
      return;
    }
  case 0x7: // ROR
    cycles++;
    {
      uint8_t shift = valS & 0xFF;
      if (shift == 0) { result = valD; }
      else {
        uint8_t rot = shift & 31;
        if (rot == 0) { carry = (valD >> 31) & 1; result = valD; }
        else { carry = (valD >> (rot - 1)) & 1; result = (valD >> rot) | (valD << (32 - rot)); }
      }
      registers[rd] = result;
    }
    break;
  case 0x8: // TST
    result = valD & valS;
    break;
  case 0x9: // NEG
    result = 0 - valS;
    registers[rd] = result;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if (valS == 0) cpsr |= (1 << 29);
    if ((valS & result) & 0x80000000) cpsr |= (1 << 28);
    lastCycles_ = 1;
    return;
  case 0xA: // CMP
    result = valD - valS;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if (valD >= valS) cpsr |= (1 << 29);
    if (((valD ^ valS) & (valD ^ result)) & 0x80000000) cpsr |= (1 << 28);
    lastCycles_ = 1;
    return;
  case 0xB: // CMN
    result = valD + valS;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    if ((uint64_t)valD + valS > 0xFFFFFFFF) cpsr |= (1 << 29);
    if (~(valD ^ valS) & (valD ^ result) & 0x80000000) cpsr |= (1 << 28);
    lastCycles_ = 1;
    return;
  case 0xC: // ORR
    result = valD | valS;
    registers[rd] = result;
    break;
  case 0xD: // MUL
    cycles += 1; // Actually depends on Rs, simplified to 2-3
    result = valD * valS;
    registers[rd] = result;
    cpsr &= ~0xF0000000;
    if (result == 0) cpsr |= (1 << 30);
    if (result & 0x80000000) cpsr |= (1 << 31);
    // Carry is meaningless, V is unaffected
    lastCycles_ = cycles;
    return;
  case 0xE: // BIC
    result = valD & (~valS);
    registers[rd] = result;
    break;
  case 0xF: // MVN
    result = ~valS;
    registers[rd] = result;
    break;
  default:
    result = 0; break;
  }

  cpsr &= ~0xF0000000;
  if (result == 0) cpsr |= (1 << 30);
  if (result & 0x80000000) cpsr |= (1 << 31);
  if (carry) cpsr |= (1 << 29);
  lastCycles_ = cycles;
}

void CPU::thumbFormat5(uint16_t opcode) {
  // Hi-Register Operations / BX
  uint8_t op = (opcode >> 8) & 0x3;
  bool H1 = (opcode >> 7) & 1;
  bool H2 = (opcode >> 6) & 1;
  uint8_t rd = (opcode & 0x7) | (H1 << 3);
  uint8_t rs = ((opcode >> 3) & 0x7) | (H2 << 3);

  lastCycles_ = 1;
  switch (op) {
  case 0: // ADD
    registers[rd] += registers[rs];
    if (rd == 15) {
      registers[15] &= ~1;
      reloadPipeline();
      lastCycles_ = 3;
    }
    break;
  case 1: // CMP
    {
      uint32_t val1 = registers[rd];
      uint32_t val2 = registers[rs];
      uint32_t res = val1 - val2;
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = (uint64_t)val1 >= (uint64_t)val2;
      bool V = ((val1 ^ val2) & (val1 ^ res) & 0x80000000) != 0;
      cpsr = (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
    break;
  case 2: // MOV
    registers[rd] = registers[rs];
    if (rd == 15) {
      registers[15] &= ~1;
      reloadPipeline();
      lastCycles_ = 3;
    }
    break;
  case 3: // BX
    {
      uint32_t target = registers[rs];
      if (target & 1) {
        target &= ~1;
        cpsr |= 0x20;
        registers[15] = target;
      } else {
        target &= ~3;
        cpsr &= ~0x20;
        registers[15] = target;
      }
      reloadPipeline();
      lastCycles_ = 3;
    }
    break;
  }
}

void CPU::thumbFormat6(uint16_t opcode) {
  // PC-relative Load
  uint8_t rd = (opcode >> 8) & 0x7;
  uint32_t word8 = opcode & 0xFF;
  uint32_t pc = registers[15] & ~2;
  uint32_t addr = pc + (word8 << 2);

  registers[rd] = bus->read32(addr);
  lastCycles_ = 3;
}

void CPU::thumbFormat78(uint16_t opcode) {
  // Load/Store with Register Offset
  uint8_t op = (opcode >> 9) & 0x7;
  uint8_t ro = (opcode >> 6) & 0x7;
  uint8_t rb = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;
  uint32_t addr = registers[rb] + registers[ro];

  lastCycles_ = 3;
  switch (op) {
  case 0: bus->write32(addr, registers[rd]); break; // STR
  case 1: bus->write16(addr, registers[rd] & 0xFFFF); break; // STRH
  case 2: bus->write8(addr, registers[rd] & 0xFF); break; // STRB
  case 3: registers[rd] = (int32_t)(int8_t)bus->read8(addr); break; // LDSB
  case 4: registers[rd] = bus->read32(addr); break; // LDR
  case 5: registers[rd] = bus->read16(addr); break; // LDRH
  case 6: registers[rd] = bus->read8(addr); break; // LDRB
  case 7: registers[rd] = (int32_t)(int16_t)bus->read16(addr); break; // LDSH
  }
}

void CPU::thumbFormat9(uint16_t opcode) {
  // Load/Store with Immediate Offset
  bool B = (opcode >> 12) & 1;
  bool L = (opcode >> 11) & 1;
  uint8_t imm5 = (opcode >> 6) & 0x1F;
  uint8_t rb = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;

  uint32_t offset = imm5;
  if (!B) offset <<= 2;
  uint32_t addr = registers[rb] + offset;

  lastCycles_ = 3;
  if (L) {
    if (B) registers[rd] = bus->read8(addr);
    else registers[rd] = bus->read32(addr);
  } else {
    if (B) bus->write8(addr, registers[rd] & 0xFF);
    else bus->write32(addr, registers[rd]);
  }
}

void CPU::thumbFormat10(uint16_t opcode) {
  // Load/Store Halfword with Immediate Offset
  bool L = (opcode >> 11) & 1;
  uint8_t imm5 = (opcode >> 6) & 0x1F;
  uint8_t rb = (opcode >> 3) & 0x7;
  uint8_t rd = opcode & 0x7;

  uint32_t offset = imm5 << 1;
  uint32_t addr = registers[rb] + offset;

  lastCycles_ = 3;
  if (L) registers[rd] = bus->read16(addr);
  else bus->write16(addr, registers[rd] & 0xFFFF);
}

void CPU::thumbFormat11(uint16_t opcode) {
  // SP-relative Load/Store
  bool L = (opcode >> 11) & 1;
  uint8_t rd = (opcode >> 8) & 0x7;
  uint32_t imm8 = opcode & 0xFF;
  uint32_t addr = registers[13] + (imm8 << 2);

  lastCycles_ = 3;
  if (L) registers[rd] = bus->read32(addr);
  else bus->write32(addr, registers[rd]);
}

void CPU::thumbFormat12(uint16_t opcode) {
  // Load Address
  bool SP = (opcode >> 11) & 1;
  uint8_t rd = (opcode >> 8) & 0x7;
  uint32_t imm8 = opcode & 0xFF;
  uint32_t src = SP ? registers[13] : (registers[15] & ~2);
  registers[rd] = src + (imm8 << 2);
  lastCycles_ = 1;
}

void CPU::thumbFormat13(uint16_t opcode) {
  // Add Offset to Stack Pointer
  bool S = (opcode >> 7) & 1;
  uint8_t imm7 = opcode & 0x7F;
  uint32_t val = imm7 * 4;
  if (S) registers[13] -= val;
  else registers[13] += val;
  lastCycles_ = 1;
}

void CPU::thumbFormat14(uint16_t opcode) {
  // Push/Pop Registers
  uint16_t rlist = opcode & 0xFF;
  int count = 0;
  for (int i = 0; i < 8; i++) if ((rlist >> i) & 1) count++;
  lastCycles_ = 2 + count; // N+1

  if ((opcode >> 11) == 0x16) { // PUSH (1011 010L)
    bool LR = (opcode >> 8) & 1;
    if (LR) count++;
    uint32_t startAddr = registers[13] - (count * 4);
    uint32_t addr = startAddr;
    for (int i = 0; i < 8; i++) {
      if ((rlist >> i) & 1) {
        bus->write32(addr, registers[i]);
        addr += 4;
      }
    }
    if (LR) bus->write32(addr, registers[14]);
    registers[13] = startAddr;

  } else { // POP (1011 110P)
    bool PC = (opcode >> 8) & 1;
    if (PC) lastCycles_ = 2 + count + 1 + 2; // Branch adds 2+1
    uint32_t addr = registers[13];
    for (int i = 0; i < 8; i++) {
      if ((rlist >> i) & 1) {
        registers[i] = bus->read32(addr);
        addr += 4;
      }
    }
    if (PC) {
      uint32_t newPC = bus->read32(addr);
      addr += 4;
      if (newPC & 1) {
        registers[15] = newPC & ~1;
        cpsr |= 0x20;
      } else {
        registers[15] = newPC & ~3;
        cpsr &= ~0x20;
      }
      reloadPipeline();
    }
    registers[13] = addr;
  }
}

void CPU::thumbFormat15(uint16_t opcode) {
  // Multiple Load/Store
  bool L = (opcode >> 11) & 1;
  uint8_t rn = (opcode >> 8) & 0x7;
  uint8_t rlist = opcode & 0xFF;
  uint32_t addr = registers[rn];

  int count = 0;
  for (int i = 0; i < 8; i++) if ((rlist >> i) & 1) count++;
  lastCycles_ = 2 + count;

  for (int i = 0; i < 8; i++) {
    if ((rlist >> i) & 1) {
      if (L) registers[i] = bus->read32(addr);
      else bus->write32(addr, registers[i]);
      addr += 4;
    }
  }
  registers[rn] = addr;
}

void CPU::thumbFormat16(uint16_t opcode) {
  // Conditional Branch
  uint8_t cond = (opcode >> 8) & 0xF;
  int8_t offset = opcode & 0xFF;
  lastCycles_ = 1;
  if (checkCondition(cond)) {
    registers[15] += (offset << 1);
    reloadPipeline();
    lastCycles_ = 3;
  }
}

void CPU::thumbFormat18(uint16_t opcode) {
  // Unconditional Branch
  int32_t offset = opcode & 0x7FF;
  if ((offset >> 10) & 1) offset |= 0xFFFFF800;
  registers[15] += (offset << 1);
  reloadPipeline();
  lastCycles_ = 3;
}

void CPU::thumbFormat19(uint16_t opcode) {
  // Long Branch with Link
  bool H = (opcode >> 11) & 1;
  int32_t offset = opcode & 0x7FF;
  lastCycles_ = 1;

  if (!H) {
    if ((offset >> 10) & 1) offset |= 0xFFFFF800;
    registers[14] = registers[15] + (offset << 12);
  } else {
    uint32_t returnAddr = (registers[15] - 2) | 1;
    uint32_t target = registers[14] + (offset << 1);
    registers[14] = returnAddr;
    registers[15] = target;
    reloadPipeline();
    lastCycles_ = 3;
  }
}


void CPU::opBranch(uint32_t opcode) {
  // B/BL
  int32_t offset = opcode & 0xFFFFFF;
  if ((offset >> 23) & 1) {
    offset |= 0xFF000000;
  }
  offset <<= 2;

  // BL: 保存返回地址 (下一条指令地址 = execute_PC + 4 = R15 - 4)
  if ((opcode >> 24) & 1) {
    registers[14] = registers[15] - 4;
  }

  // R15 = execute_PC + 8
  // 分支目标 = execute_PC + 8 + offset = R15 + offset
  registers[15] = registers[15] + offset;

  reloadPipeline();
}

void CPU::opDataProcessing(uint32_t opcode) {
  // Format: cond(4) 00 I(1) Opcode(4) S(1) Rn(4) Rd(4) ShifterOperand(12)
  uint8_t op = (opcode >> 21) & 0xF;
  uint8_t rn = (opcode >> 16) & 0xF;
  uint8_t rd = (opcode >> 12) & 0xF;

  bool carryOut = (cpsr >> 29) & 1;
  bool I = (opcode >> 25) & 1;
  uint32_t op2 = 0;

  if (I) {
    // Immediate 8-bit rotated
    uint32_t imm8 = opcode & 0xFF;
    uint32_t rotate = ((opcode >> 8) & 0xF) * 2;
    // ROR equal to rotate
    if (rotate > 0) {
      carryOut = (imm8 >> (rotate - 1)) & 1;
      op2 = (imm8 >> rotate) | (imm8 << (32 - rotate));
    } else {
      op2 = imm8;
      // Carry unaffected
    }
  } else {
    // Register shift
    uint8_t shiftType = (opcode >> 5) & 0x3;
    uint8_t shiftAmt = 0;
    bool immediateShift = !((opcode >> 4) & 1);

    if (immediateShift) {
      // Immediate shift amount
      shiftAmt = (opcode >> 7) & 0x1F;
    } else {
      // Register shift amount
      uint8_t rs = (opcode >> 8) & 0xF;
      shiftAmt = registers[rs] & 0xFF; // Only bottom byte
    }

    // Perform Shift
    op2 = barrelShifter(registers[opcode & 0xF], shiftType, shiftAmt, carryOut,
                        immediateShift);
  }

  bool S = (opcode >> 20) & 1;
  uint32_t res = 0;

  // Logic: 00I Opcode S Rn Rd ShifterOperand
  switch (op) {
  case 0x0: // AND
    res = registers[rn] & op2;
    break;
  case 0x1: // EOR
    res = registers[rn] ^ op2;
    break;
  case 0x2: // SUB
  {
    uint32_t op1 = registers[rn];
    res = op1 - op2;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = (uint64_t)op1 >= (uint64_t)op2;
      bool V = ((op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x3: // RSB (Reverse Subtract)
  {
    uint32_t op1 = registers[rn];
    res = op2 - op1;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = (uint64_t)op2 >= (uint64_t)op1;
      bool V = ((op2 ^ op1) & (op2 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x4: // ADD
  {
    uint32_t op1 = registers[rn];
    res = op1 + op2;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = ((uint64_t)op1 + (uint64_t)op2) > 0xFFFFFFFF;
      bool V = (~(op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x5: // ADC
  {
    uint32_t op1 = registers[rn];
    uint32_t carry = (cpsr >> 29) & 1;
    uint64_t fullRes = (uint64_t)op1 + (uint64_t)op2 + carry;
    res = (uint32_t)fullRes;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = fullRes > 0xFFFFFFFF;
      bool V = (~(op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x6: // SBC
  {
    uint32_t op1 = registers[rn];
    uint32_t carry = (cpsr >> 29) & 1;
    uint32_t sub = op2 + !carry;
    res = op1 - sub;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = (uint64_t)op1 >= (uint64_t)sub;
      bool V = ((op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x7: // RSC
  {
    uint32_t op1 = registers[rn];
    uint32_t carry = (cpsr >> 29) & 1;
    uint32_t sub = op1 + !carry;
    res = op2 - sub;
    if (S) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = (uint64_t)op2 >= (uint64_t)sub;
      bool V = ((op2 ^ op1) & (op2 ^ res) & 0x80000000) != 0;
      cpsr =
          (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    }
  } break;
  case 0x8: // TST
    res = registers[rn] & op2;
    break;
  case 0x9: // TEQ
    res = registers[rn] ^ op2;
    break;
  case 0xA: // CMP
  {
    uint32_t op1 = registers[rn];
    res = op1 - op2;
    bool N = (res >> 31) & 1;
    bool Z = (res == 0);
    bool C = (uint64_t)op1 >= (uint64_t)op2;
    bool V = ((op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
    cpsr = (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    return; // Don't write to Rd
  }
  case 0xB: // CMN
  {
    uint32_t op1 = registers[rn];
    res = op1 + op2;
    bool N = (res >> 31) & 1;
    bool Z = (res == 0);
    bool C = ((uint64_t)op1 + (uint64_t)op2) > 0xFFFFFFFF;
    bool V = (~(op1 ^ op2) & (op1 ^ res) & 0x80000000) != 0;
    cpsr = (cpsr & 0x0FFFFFFF) | (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    return; // Don't write to Rd
  }
  case 0xC: // ORR
    res = registers[rn] | op2;
    break;
  case 0xD: // MOV
    res = op2;
    break;
  case 0xE: // BIC
    res = registers[rn] & (~op2);
    break;
  case 0xF: // MVN
    res = ~op2;
    break;
  }

  // 逻辑运算的标志更新
  if (S && op != 0xA && op != 0xB) {
    if (op == 0x8 || op == 0x9 || op == 0x0 || op == 0x1 || op == 0xC ||
        op == 0xD || op == 0xE || op == 0xF) {
      bool N = (res >> 31) & 1;
      bool Z = (res == 0);
      bool C = carryOut;
      cpsr = (cpsr & 0x1FFFFFFF) | (N << 31) | (Z << 30) | (C << 29);
    }
  }

  if (op != 0x8 && op != 0x9) {
    registers[rd] = res;
  }

  // 写 R15 且 S=1：从异常返回，完整恢复 CPSR 并刷新流水线
  if (rd == 15) {
    if (S) {
      uint32_t saved_spsr = spsr;
      switchMode(saved_spsr & 0x1F);
      cpsr = saved_spsr; // 使用保存的 spsr，防止 switchMode 覆盖它
    }
    reloadPipeline();
  }
}

void CPU::opLoadStore(uint32_t opcode) {
  // Format: cond(4) 01 I(1) P(1) U(1) B(1) W(1) L(1) Rn(4) Rd(4) Offset(12)

  bool I = (opcode >> 25) & 1; // 0=Immediate Offset, 1=Register Offset
  bool P = (opcode >> 24) & 1; // Pre/Post indexing
  bool U = (opcode >> 23) & 1; // Up/Down
  bool B = (opcode >> 22) & 1; // Byte/Word
  bool W = (opcode >> 21) & 1; // Write-back
  bool L = (opcode >> 20) & 1; // Load/Store

  uint8_t rn = (opcode >> 16) & 0xF;
  uint8_t rd = (opcode >> 12) & 0xF;

  uint32_t offset = 0;
  if (!I) {
    // 立即数偏移
    offset = opcode & 0xFFF;
  } else {
    // 寄存器偏移 (带移位)
    uint8_t rm = opcode & 0xF;
    uint8_t shiftType = (opcode >> 5) & 0x3;
    uint8_t shiftAmount = (opcode >> 7) & 0x1F;
    uint32_t rmVal = registers[rm];

    switch (shiftType) {
    case 0: // LSL
      offset = rmVal << shiftAmount;
      break;
    case 1: // LSR
      offset = shiftAmount ? (rmVal >> shiftAmount) : 0;
      break;
    case 2: // ASR
      if (shiftAmount)
        offset = (uint32_t)((int32_t)rmVal >> shiftAmount);
      else
        offset = (rmVal & 0x80000000) ? 0xFFFFFFFF : 0;
      break;
    case 3: // ROR
      if (shiftAmount)
        offset = (rmVal >> shiftAmount) | (rmVal << (32 - shiftAmount));
      else
        offset = ((cpsr >> 29) & 1) << 31 | (rmVal >> 1); // RRX
      break;
    }
  }

  uint32_t addr = registers[rn];

  if (P) {
    // Pre-indexing
    if (U)
      addr += offset;
    else
      addr -= offset;
  }

  if (L) {
    // LDR
    if (B) {
      uint8_t val = bus->read8(addr);
      registers[rd] = val;
    } else {
      uint32_t val = bus->read32(addr);
      // Rotate if unaligned? ARMv4 behavior
      registers[rd] = val;
    }
    if (rd == 15) {
      if (cpsr & 0x20)
        registers[15] &= ~1;
      else
        registers[15] &= ~3;
      reloadPipeline();
    }
  } else {
    // STR
    uint32_t val = registers[rd];
    // If R15 is source, store PC+12? (Simplified to PC)

    if (B) {
      bus->write8(addr, val & 0xFF);
    } else {
      bus->write32(addr, val);
    }
  }

  // Write-back or Post-indexing
  if (!P) {
    // Post-indexing: always write back
    if (U)
      registers[rn] += offset;
    else
      registers[rn] -= offset;
  } else if (W) {
    // Pre-indexing with Write-back
    registers[rn] = addr;
  }
}

uint32_t CPU::getRegister(int reg) const { return registers[reg]; }

void CPU::setRegister(int reg, uint32_t value) {
  if (reg >= 0 && reg < 16) {
    registers[reg] = value;
    if (reg == 15) {
      reloadPipeline();
    }
  }
}

void CPU::setPC(uint32_t val) {
  registers[15] = val;
  reloadPipeline();
}

void CPU::setCPSR(uint32_t val) {
  uint32_t oldMode = cpsr & 0x1F;
  uint32_t newMode = val & 0x1F;
  cpsr = val;
  if (oldMode != newMode) {
    switchMode(newMode);
  }
}

int CPU::getBankIndex(uint32_t mode) const {
  switch (mode) {
  case 0x11:
    return 1; // FIQ
  case 0x12:
    return 2; // IRQ
  case 0x13:
    return 3; // SVC
  case 0x17:
    return 4; // Abort
  case 0x1B:
    return 5; // Undef
  case 0x10:
  case 0x1F:
  default:
    return 0; // User/System
  }
}

void CPU::switchMode(uint32_t newMode) {
  uint32_t oldMode = cpsr & 0x1F;
  if (oldMode == newMode)
    return;

  int oldIdx = getBankIndex(oldMode);
  int newIdx = getBankIndex(newMode);

  // Save current registers to old bank
  banks[oldIdx].r13 = registers[13];
  banks[oldIdx].r14 = registers[14];
  banks[oldIdx].spsr = spsr;
  if (oldMode == 0x11) { // FIQ
    for (int i = 0; i < 5; i++)
      banks[oldIdx].r8_r12[i] = registers[8 + i];
  }

  // Update CPSR
  cpsr = (cpsr & ~0x1F) | (newMode & 0x1F);

  // Load registers from new bank
  registers[13] = banks[newIdx].r13;
  registers[14] = banks[newIdx].r14;
  spsr = banks[newIdx].spsr;
  if (newMode == 0x11) { // FIQ
    for (int i = 0; i < 5; i++)
      registers[8 + i] = banks[newIdx].r8_r12[i];
  } else if (oldMode == 0x11) { // Transitioning AWAY from FIQ
    // Restore R8-R12 (shared by all other modes) from bank 0
    for (int i = 0; i < 5; i++)
      registers[8 + i] = banks[0].r8_r12[i];
  }
}

void CPU::irq() {
  // IRQ 唤醒 halted 状态的 CPU
  halted = false;
  uint32_t oldCPSR = cpsr;
  // Adjust PC: logical PC is current instruction address.
  // registers[15] is pipeline head.
  // ARM: PC+8. We want PC+4. -> R15-4.
  // Thumb: PC+4. We want PC+2/4?
  // Standard formula used in other emus/docs:
  uint32_t oldPC = (cpsr & 0x20) ? registers[15] : registers[15] - 4;

  switchMode(0x12); // Switch to IRQ mode

  cpsr &= ~0x20; // ARM mode
  cpsr |= 0x80;  // Disable IRQ

  spsr = oldCPSR;
  registers[14] = oldPC;

  registers[15] = 0x18;
  reloadPipeline();
}

uint32_t CPU::barrelShifter(uint32_t val, uint8_t shiftType,
                            uint8_t shiftAmount, bool &carryOut,
                            bool immediateShift) {
  // shiftType: 0=LSL, 1=LSR, 2=ASR, 3=ROR
  switch (shiftType) {
  case 0: // LSL
    if (shiftAmount == 0) {
      // No shift, carry unaffected (unless immediate shift, then it is
      // unaffected) Wait, for Register shift LSL#0 is no shift.
      if (immediateShift) {
        // LSL#0: No shift, C unaffected
      }
    } else if (shiftAmount < 32) {
      carryOut = (val >> (32 - shiftAmount)) & 1;
      val <<= shiftAmount;
    } else if (shiftAmount == 32) {
      carryOut = val & 1;
      val = 0;
    } else {
      carryOut = 0;
      val = 0;
    }
    break;

  case 1: // LSR
    if (shiftAmount == 0) {
      if (immediateShift) {
        // LSR#0 means LSR#32
        carryOut = (val >> 31) & 1;
        val = 0;
      }
    } else if (shiftAmount < 32) {
      carryOut = (val >> (shiftAmount - 1)) & 1;
      val >>= shiftAmount;
    } else if (shiftAmount == 32) {
      carryOut = (val >> 31) & 1;
      val = 0;
    } else {
      carryOut = 0;
      val = 0;
    }
    break;

  case 2: // ASR
    if (shiftAmount == 0) {
      if (immediateShift) {
        // ASR#0 means ASR#32
        carryOut = (val >> 31) & 1;
        val = ((int32_t)val) >> 31; // Fill with sign bit
      }
    } else if (shiftAmount < 32) {
      carryOut = (val >> (shiftAmount - 1)) & 1;
      val = ((int32_t)val) >> shiftAmount;
    } else {
      carryOut = (val >> 31) & 1;
      val = ((int32_t)val) >> 31;
    }
    break;

  case 3: // ROR
    if (shiftAmount == 0) {
      if (immediateShift) {
        // ROR#0 means RRX
        bool oldC = (cpsr >> 29) & 1;
        carryOut = val & 1;
        val = (val >> 1) | (oldC << 31);
      }
    } else {
      shiftAmount &= 31;
      if (shiftAmount == 0) {
        // ROR#32?
        carryOut = (val >> 31) & 1;
        // val unchanged
      } else {
        carryOut = (val >> (shiftAmount - 1)) & 1;
        val = (val >> shiftAmount) | (val << (32 - shiftAmount));
      }
    }
    break;
  }
  return val;
}

void CPU::opMultiply(uint32_t opcode) {
  // MUL{cond}{S} Rd, Rm, Rs
  // MUL/MLA: 0000 00AS Rd Rn Rs 1001 Rm
  // Long: 0000 1UAS RdHi RdLo Rs 1001 Rm
  bool L = (opcode >> 23) & 1;
  bool U = (opcode >> 22) & 1;
  bool A = (opcode >> 21) & 1;
  bool S = (opcode >> 20) & 1;

  uint8_t rdHi = (opcode >> 16) & 0xF;
  uint8_t rdLo = (opcode >> 12) & 0xF;
  uint8_t rs = (opcode >> 8) & 0xF;
  uint8_t rm = opcode & 0xF;

  if (!L) {
    // 32-bit Multiply (MUL/MLA)
    // MUL: Rd = Rm * Rs,  MLA: Rd = (Rm * Rs) + Rn
    uint32_t rd = rdHi; // In short MUL, bit 16-19 is Rd
    uint32_t rn = rdLo; // In short MLA, bit 12-15 is Rn
    uint32_t res = registers[rm] * registers[rs];
    if (A)
      res += registers[rn];
    registers[rd] = res;

    if (S) {
      cpsr = (cpsr & 0x3FFFFFFF) | (((res >> 31) & 1) << 31); // N
      if (res == 0)
        cpsr |= (1 << 30); // Z
    }
  } else {
    // 64-bit Multiply
    uint64_t res = 0;
    if (U) {
      // Signed (SMULL/SMLAL)
      res = (int64_t)(int32_t)registers[rm] * (int64_t)(int32_t)registers[rs];
      if (A) {
        uint64_t accum = ((uint64_t)registers[rdHi] << 32) | registers[rdLo];
        res += accum;
      }
    } else {
      // Unsigned (UMULL/UMLAL)
      res = (uint64_t)registers[rm] * (uint64_t)registers[rs];
      if (A) {
        uint64_t accum = ((uint64_t)registers[rdHi] << 32) | registers[rdLo];
        res += accum;
      }
    }

    registers[rdHi] = (res >> 32) & 0xFFFFFFFF;
    registers[rdLo] = res & 0xFFFFFFFF;

    if (S) {
      cpsr = (cpsr & 0x3FFFFFFF) | (((res >> 63) & 1) << 31); // N
      if (res == 0)
        cpsr |= (1 << 30); // Z
    }
  }
}

// Helper for HLE
// Assuming registers[0]..[3] are arguments.
// CpuSet/FastSet use R0, R1, R2.

void CPU::opSWI(uint32_t opcode) {
  uint8_t swi = 0;
  if (cpsr & 0x20) {
    swi = opcode & 0xFF;
  } else {
    swi = (opcode >> 16) & 0xFF;
  }

  // SWI handler (no debug log)

  // 有真实 BIOS 时，走硬件 SWI 路径（跳转到 0x08 执行 BIOS 代码）
  if (hasBIOS_) {
    uint32_t oldCPSR = cpsr;
    uint32_t oldPC = (cpsr & 0x20) ? (registers[15] - 2) : (registers[15] - 4);

    switchMode(0x13); // Supervisor mode

    spsr = oldCPSR;
    registers[14] = oldPC;
    cpsr &= ~0x20; // ARM mode
    cpsr |= 0x80;  // I=1

    registers[15] = 0x08;
    reloadPipeline();
    return;
  }

  // HLE Implementation (无 BIOS 时)
  switch (swi) {
  case 0x01: // RegisterRamReset
    return;
  case 0x05: // VBlankIntrWait
  {
    // 清除 IntrWait flags 中的 VBlank 位
    uint16_t flags = bus->read16(0x03007FF8);
    bus->write16(0x03007FF8, flags & ~0x1);
    // 设置 halted，等待 VBlank 中断唤醒
    halted = true;
    return;
  }
  case 0x04: // IntrWait
  {
    // R0: 1 = 丢弃旧标志, 0 = 检查已有标志
    // R1: 要等待的中断掩码
    if (registers[0]) {
      uint16_t flags = bus->read16(0x03007FF8);
      bus->write16(0x03007FF8, flags & ~(uint16_t)registers[1]);
    }
    halted = true;
    return;
  }
  case 0x06: // Div
  {
    int32_t num = (int32_t)registers[0];
    int32_t den = (int32_t)registers[1];
    if (den != 0) {
      registers[0] = num / den;
      registers[1] = num % den;
      registers[3] = abs(num / den);
    }
    return;
  }
  case 0x0B: // CpuSet
    hleCpuSet(registers[0], registers[1], registers[2]);
    return;
  case 0x0C: // CpuFastSet
    hleCpuFastSet(registers[0], registers[1], registers[2]);
    return;
  case 0x11: // LZ77UnComp
    hleLZ77UnComp(registers[0], registers[1]);
    return;
  } // end switch

  // Log unhandled SWI and return immediately instead of crashing into BIOS
  printf("Unhandled SWI %02X at %08X, ignoring...\n", swi, registers[15]);
  return;
}

void CPU::opMRS(uint32_t opcode) {
  // MRS{cond} Rd, Psr
  // 0001 0R00 1111 Rd 0000 0000 0000
  // R=0: CPSR, R=1: SPSR

  bool R = (opcode >> 22) & 1;
  uint8_t rd = (opcode >> 12) & 0xF;

  if (R) {
    registers[rd] = spsr;
  } else {
    registers[rd] = cpsr;
  }
}

void CPU::opMSR(uint32_t opcode) {
  // MSR{cond} Psr{_fields}, Op2
  // Register: 0001 0R10 1001 1111 0000 0000Rm
  // Immediate: 0011 0R10 1000 1111 Rotate Imm8
  // I bit (25) distinguishes

  bool I = (opcode >> 25) & 1;
  bool R = (opcode >> 22) & 1;

  uint32_t op2 = 0;

  if (I) {
    uint32_t imm8 = opcode & 0xFF;
    uint32_t rotate = ((opcode >> 8) & 0xF) * 2;
    if (rotate > 0) {
      op2 = (imm8 >> rotate) | (imm8 << (32 - rotate));
    } else {
      op2 = imm8;
    }
  } else {
    uint8_t rm = opcode & 0xF;
    op2 = registers[rm];
  }

  // Mask logic (Bits 16-19: f, s, x, c)
  // Bit 16: Control (0-7)
  // Bit 17: Extension (8-15)
  // Bit 18: Status (16-23)
  // Bit 19: Flags (24-31)

  uint32_t mask = 0;
  if ((opcode >> 16) & 1)
    mask |= 0x000000FF;
  if ((opcode >> 17) & 1)
    mask |= 0x0000FF00;
  if ((opcode >> 18) & 1)
    mask |= 0x00FF0000;
  if ((opcode >> 19) & 1)
    mask |= 0xFF000000;

  if (R) {
    spsr = (spsr & ~mask) | (op2 & mask);
  } else {
    // Modifying CPSR
    uint32_t newVal = (cpsr & ~mask) | (op2 & mask);
    if (mask & 0x1F) {
      // Mode bits are being changed
      switchMode(newVal & 0x1F);
      // switchMode already updated cpsr, but we might have other bits in mask
      cpsr = (cpsr & ~mask) | (op2 & mask);
    } else {
      cpsr = newVal;
    }
  }
}

void CPU::opBlockDataTransfer(uint32_t opcode) {
  // Cond 100 P U S W L Rn RegisterList
  bool P = (opcode >> 24) & 1; // Pre/Post
  bool U = (opcode >> 23) & 1; // Up/Down
  bool S = (opcode >> 22) & 1; // PSR/Force User
  bool W = (opcode >> 21) & 1; // Write-back
  bool L = (opcode >> 20) & 1; // Load/Store
  uint8_t rn = (opcode >> 16) & 0xF;
  uint16_t regList = opcode & 0xFFFF;

  // Calculate total size
  int distinctRegs = 0;
  for (int i = 0; i < 16; i++) {
    if ((regList >> i) & 1)
      distinctRegs++;
  }

  uint32_t addr = registers[rn];
  uint32_t startAddr = addr;

  // Adjust start address for Down/Pre modes logic
  // Simplified:
  // Up: Addr increases. P=1: start at addr+4. P=0: start at addr.
  // Down: Addr decreases. P=1: end at addr-4. P=0: end at addr.
  // The "Base" is always the lowest address in memory accessed?
  // Actually, ARM always stores lowest register to lowest address.
  // So we just need to find the "Base Address" of the transfer block.

  uint32_t finalAddr = addr;
  if (U) {
    // Increment
    finalAddr = addr + (distinctRegs * 4);
    if (P) {
      // Pre-increment: First transfer at addr+4
      addr += 4;
    }
    // If Post-increment: First transfer at addr.
  } else {
    // Decrement
    finalAddr = addr - (distinctRegs * 4);
    addr = finalAddr; // Lowest address
    if (P) {
      // Pre-decrement: Last register transfer at Base-4
      // Range is [Base-4*N, Base-4]
      // Lowest address: Base - 4*N
    } else {
      // Post-decrement: Range [Base - 4*N + 4, Base]
      addr += 4;
    }
  }

  // Correction:
  // It's easier to compute "Base Address" of the block.
  // If U=1 (Increment): Base = Rn (if P=0) or Rn+4 (if P=1)?
  // If U=0 (Decrement): Base = Rn - 4*Count (if P=0? No).
  // Let's us standard definitions:
  // IA (Inc After): Start=Rn, End=Rn+4*N. Rn becomes Rn+4*N.
  // IB (Inc Before): Start=Rn+4, End=Rn+4*N+4.
  // DA (Dec After): Start=Rn-4*N+4, End=Rn.
  // DB (Dec Before): Start=Rn-4*N, End=Rn-4.

  // Wait, ARM stores lowest reg at lowest address ALWAYS.
  // So we just need to calculate the START ADDRESS of the block.
  uint32_t base = registers[rn];
  if (U) {
    if (P)
      base += 4; // IB
                 // else IA: base = base
  } else {
    base -= (distinctRegs * 4);
    if (!P)
      base += 4; // DA
                 // else DB: base = base
  }

  // Transfer
  uint32_t currentAddr = base;
  for (int i = 0; i < 16; i++) {
    if ((regList >> i) & 1) {
      if (L) {
        registers[i] = bus->read32(currentAddr);
      } else {
        bus->write32(currentAddr, registers[i]);
      }
      currentAddr += 4;
    }
  }

  // Write-back
  // For Load, if Rn is in list... result unpredictable/implementation
  // defined. Usually if Rn is not first, it's overwritten by load? GBA
  // (ARM7TDMI) writeback if W=1. Exception: If L=1 and Rn is in list, W
  // shouldn't be set?
  if (W && !(L && ((regList >> rn) & 1))) {
    if (U) {
      registers[rn] += distinctRegs * 4;
    } else {
      registers[rn] -= distinctRegs * 4;
    }
  }

  // S-bit handling
  if (S) {
    if (L && ((regList >> 15) & 1)) {
      // LDM with PC -> Restore CPSR from SPSR
      uint32_t saved_spsr = spsr;
      switchMode(saved_spsr & 0x1F);
      cpsr = saved_spsr; // 使用保存的 spsr，防止 switchMode 覆盖它
    } else {
      // User Bank Transfer (Not fully implemented, requires banked registers)
    }
  }

  // 如果是 LDM 且 PC 在列表中，需要刷新流水线
  if (L && ((regList >> 15) & 1)) {
    if (cpsr & 0x20)
      registers[15] &= ~1;
    else
      registers[15] &= ~3;
    reloadPipeline();
  }
}

// HLE Implementations
void Core::CPU::hleCpuSet(uint32_t src, uint32_t dst, uint32_t control) {
  int count = control & 0x1FFFFF;
  bool fixedSrc = (control >> 24) & 1;
  bool is32 = (control >> 26) & 1;

  if (count == 0)
    return;

  if (is32) {
    uint32_t val = bus->read32(src);
    for (int i = 0; i < count; i++) {
      bus->write32(dst, val);
      dst += 4;
      if (!fixedSrc) {
        src += 4;
        val = bus->read32(src);
      }
    }
  } else {
    uint16_t val = bus->read16(src);
    for (int i = 0; i < count; i++) {
      bus->write16(dst, val);
      dst += 2;
      if (!fixedSrc) {
        src += 2;
        val = bus->read16(src);
      }
    }
  }
}

void Core::CPU::hleCpuFastSet(uint32_t src, uint32_t dst, uint32_t control) {
  int count = (control & 0x1FFFFF) * 8; // CpuFastSet copies blocks of 8 words
  bool fixedSrc = (control >> 24) & 1;

  if (count == 0)
    return;

  uint32_t val = bus->read32(src);
  for (int i = 0; i < count; i++) {
    bus->write32(dst, val);
    dst += 4;
    if (!fixedSrc) {
      src += 4;
      val = bus->read32(src);
    }
  }
}

void Core::CPU::hleLZ77UnComp(uint32_t src, uint32_t dst) {
  // LZ77 Format:
  // Header (32-bit):
  // Bit 0-3: Reserved
  // Bit 4-7: Compression Type (0x1 = LZ77)
  // Bit 8-31: Decompressed Size

  uint32_t header = bus->read32(src);
  src += 4;

  uint32_t decompressedSize = header >> 8;

  // We need to keep track of written bytes for lookback.
  // Since we are writing to memory, we can just read back from DST!
  // But DST might be VRAM (write-only sometimes? not really).
  // GBA VRAM is readable.

  // However, repeated VRAM reads might be slow or disallowed in some modes?
  // Actually safe to assume readable for our emulator.

  uint32_t currentDst = dst;
  uint32_t endDst = dst + decompressedSize;

  while (currentDst < endDst) {
    uint8_t flags = bus->read8(src++);

    for (int i = 0; i < 8; i++) {
      if (currentDst >= endDst)
        break;

      // Flag 0: Copy direct byte
      // Flag 1: Compressed block
      if (((flags >> (7 - i)) & 1) == 0) {
        uint8_t byte = bus->read8(src++);
        bus->write8(currentDst++, byte);
      } else {
        // Block:
        // Byte 1: Disp MSB(4) | Count-3(4)
        // Byte 2: Disp LSB(8)
        // Disp = ((MSB<<8) | LSB) + 1
        // Length = (Count) + 3

        uint8_t byte1 = bus->read8(src++);
        uint8_t byte2 = bus->read8(src++);

        int disp = (((byte1 & 0xF) << 8) | byte2) + 1;
        int len = (byte1 >> 4) + 3;

        // Copy length bytes from (currentDst - disp)
        for (int j = 0; j < len; j++) {
          if (currentDst >= endDst)
            break;
          uint8_t val = bus->read8(currentDst - disp);
          bus->write8(currentDst++, val);
        }
      }
    }
  }
}

} // namespace Core
