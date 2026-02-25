#include "GBA.h"
#include "APU.h"
#include "Debugger.h"
#include "IORegisters.h"
#include "Timing.h"
#include <memory>

namespace Core {

GBA::GBA() {
  bus = std::make_shared<Bus>();
  cpu = std::make_unique<CPU>(bus);
  ppu = std::make_unique<PPU>(bus);
  apu = std::make_unique<APU>();

  bus->setAPU(apu.get());
  bus->setCPU(cpu.get());
}

GBA::~GBA() {}

void GBA::reset() {
  // 清除 IO 寄存器
  for (int i = 0; i < 1024; i++) {
    bus->write8(0x04000000 + i, 0);
  }
  // KEYINPUT 默认值
  bus->write16(0x04000130, 0x03FF);

  cpu->reset();

  // FORCE SKIP BIOS (HLE BOOT)
  bool skipBios = true;
  if (skipBios) {
    // Initialize Stacks by switching modes

    // System Mode (User/System Stack)
    cpu->setCPSR(0x1F);
    cpu->setRegister(13, 0x03007F00);

    // IRQ Mode
    cpu->setCPSR(0x12);
    cpu->setRegister(13, 0x03007FA0);

    // Supervisor Mode
    cpu->setCPSR(0x13);
    cpu->setRegister(13, 0x03007FE0);

    // Switch back to System Mode for Entry
    cpu->setCPSR(0x1F);
    cpu->setPC(0x08000000);

    printf("GBAEmu: RESET - HLE BOOT ACTIVATED. PC=0x08000000\n");
    printf("ROM Header: %08X %08X %08X %08X\n", bus->read32(0x08000000),
           bus->read32(0x08000004), bus->read32(0x08000008),
           bus->read32(0x0800000C)); // Fix read32 static? No method is not
                                     // static. bus->read32 is correct.
    printf("ROM Header: %08X %08X %08X %08X\n", bus->read32(0x08000000),
           bus->read32(0x08000004), bus->read32(0x08000008),
           bus->read32(0x0800000C));
  } else {
    if (hasBIOS)
      cpu->setEntryPoint(0x00000000u);
  }
}

void GBA::step() {
  cpu->step();
  updateTimers(2);
  checkDMA();
  checkInterrupts();
}

void GBA::stepFrame(uint32_t *buffer, size_t stride) {
  if (!buffer) {
    printf("stepFrame: null buffer!\n");
    fflush(stdout);
    return;
  }
  static int frameCount = 0;
  frameCount++;

  // Disable heavy logging
  // Debugger::getInstance().enable(true);

  static bool biosAdvanced = false;
  if (!biosAdvanced && cpu->getPC() > 0x1000 && cpu->getPC() < 0x4000) {
    biosAdvanced = true;
    printf("GBAEmu: BIOS ADVANCED PAST 0x1000! PC=%08X\n", cpu->getPC());
  }

  // HLE BOOT TRACE
  // if (cpu->getPC() >= 0x08000000 && frameCount < 3) {
  //     Debugger::getInstance().enable(true);
  // } else {
  //     Debugger::getInstance().enable(false);
  // }

  // Debug Loop at 082C9027 -> Caller at 082D8200
  if (cpu->getPC() >= 0x082D8200 && cpu->getPC() <= 0x082D8220) {
    Debugger::getInstance().enable(true);
  } else {
    Debugger::getInstance().enable(false);
  }

  // Trace Frame 3-20 (Transitions to loop?)
  if (frameCount >= 3 && frameCount < 20) {
    Debugger::getInstance().enable(true);
  } else {
    Debugger::getInstance().enable(false);
  }

  if ((frameCount % 60) == 0 && cpu->getPC() >= 0x08000000) {
    uint16_t dispcnt = bus->read16(0x04000000);
    uint16_t dispstat = bus->read16(0x04000004);
    uint16_t ie = bus->read16(0x04000200);
    uint16_t interrupt_flag = bus->read16(0x04000202);
    printf("PPU DEBUG: CNT=%04X STAT=%04X IE=%04X IF=%04X PC=%08X\n", dispcnt,
           dispstat, ie, interrupt_flag, cpu->getPC());
  }

  for (int line = 0; line < TOTAL_LINES; line++) {
    bus->write16(0x04000006, line);

    // 更新 DISPSTAT 的 VBlank 和 VCount match 标志
    uint16_t dispstat = bus->read16(0x04000004);
    uint8_t vcountTarget = (dispstat >> 8) & 0xFF;

    // 清除状态标志 (保留控制位)
    dispstat &= ~0x7; // 清除 VBlank、HBlank、VCount match 标志

    // VBlank: 160-227
    if (line >= VISIBLE_LINES) {
      dispstat |= 0x1; // VBlank flag
    }

    // VCount match
    if (line == vcountTarget) {
      dispstat |= 0x4;         // VCount match flag
      if (dispstat & 0x20) {   // VCount IRQ enable
        requestInterrupt(0x4); // VCount IRQ
      }
    }

    bus->write16(0x04000004, dispstat);

    // Run CPU for one scanline (1232 cycles)
    int cyclesRun = 0;
    while (cyclesRun < CYCLES_PER_LINE) {
      cpu->step();
      int stepCycles = 4;
      cyclesRun += stepCycles;

      updateTimers(stepCycles);
      apu->step(stepCycles);
      checkDMA();
      checkInterrupts();
    }

    if (line < VISIBLE_LINES) {
      size_t pixelStride = stride / 4;
      ppu->renderScanline(line, buffer + (line * pixelStride));
    }

    // HBlank DMA
    if (line < VISIBLE_LINES) {
      uint16_t hdispstat = bus->read16(0x04000004);
      bus->write16(0x04000004, hdispstat | 2); // Set HBlank

      for (int c = 0; c < 4; c++) {
        uint32_t regBase = 0x04000000 + 0xB0 + (c * 0xC);
        uint16_t cnt_h = bus->read16(regBase + 0xA);
        if ((cnt_h & 0x8000) && ((cnt_h >> 12) & 3) == 2) {
          transferDMA(c);
        }
      }
    }

    // Check for ROM entry
    static bool romEntered = false;
    if (!romEntered && cpu->getPC() >= 0x08000000) {
      romEntered = true;
      printf("GBAEmu: JUMP TO ROM CONFIRMED! PC=0x%08X\n", cpu->getPC());
    }

    static int palDumpCount = 0;
    if (line == 159 && palDumpCount++ % 60 == 0) {
      printf("PALETTE DUMP (Frame %d): ", palDumpCount / 60);
      for (int i = 0; i < 16; i++) {
        printf("%04X ", bus->read16(0x05000000 + i * 2));
      }
      printf("\n");
      printf("VRAM DUMP BG: ");
      for (int i = 0; i < 8; i++) {
        printf("%04X ", bus->read16(0x06000000 + i * 2));
      }
      printf("\n");
    }

    // VBlank 开始时触发 VBlank IRQ 和 DMA
    if (line == VISIBLE_LINES) {
      uint16_t dstat = bus->read16(0x04000004);
      if (dstat & 0x8) {
        requestInterrupt(0x1); // VBlank IRQ
      }

      // Check DMA VBlank Trigger
      for (int c = 0; c < 4; c++) {
        uint32_t regBase = 0x04000000 + 0xB0 + (c * 0xC);
        uint16_t cnt_h = bus->read16(regBase + 0xA);
        if ((cnt_h & 0x8000) && ((cnt_h >> 12) & 3) == 1) {
          transferDMA(c);
        }
      }
    }
  }
  // Debugger::getInstance().flush();
}

void GBA::setKeyStatus(uint16_t keyMask, bool pressed) {
  // KEYINPUT (0x4000130)
  // 0 = Pressed, 1 = Released
  // We need to read current state, modify bit, and write back.
  // Or better, let Bus handle it or just write to memory directly if we can
  // access IO regs? Bus read16/write16 handles mapping.

  uint32_t addr = 0x04000000 + 0x130; // 0x130 is IO::KEYINPUT
  uint16_t current = bus->read16(addr);

  if (pressed) {
    current &= ~keyMask; // Clear bit (0 = pressed)
  } else {
    current |= keyMask; // Set bit (1 = released)
  }

  bus->write16(addr, current);
}

void GBA::latchDMA(int c) {
  uint32_t regBase = 0x04000000 + 0xB0 + (c * 0xC);
  uint16_t cnt_h = bus->read16(regBase + 0xA);

  if ((cnt_h & 0x8000) && !dma[c].active) {
    dma[c].active = true;
    dma[c].sad = bus->read32(regBase);
    dma[c].dad = bus->read32(regBase + 4);
    dma[c].count = bus->read16(regBase + 8);
  } else if (!(cnt_h & 0x8000)) {
    dma[c].active = false;
  }
}

void GBA::checkDMA() {
  // Check all 4 channels
  for (int i = 0; i < 4; i++) {
    latchDMA(i);

    if (!dma[i].active)
      continue; // Not enabled

    uint32_t regBase = 0x04000000 + 0xB0 + (i * 0xC);
    uint16_t cnt_h = bus->read16(regBase + 0xA);

    // Timing Mode (Bits 12-13)
    int timing = (cnt_h >> 12) & 0x3;

    if (timing == 0) { // Immediate
      transferDMA(i);
    }
  }
}

void GBA::transferDMA(int channel) {
  uint32_t regBase = 0x04000000 + 0xB0 + (channel * 0xC);

  // Read internal state
  uint32_t sad = dma[channel].sad;
  uint32_t dad = dma[channel].dad;
  uint16_t cnt_h = bus->read16(regBase + 0xA);

  int count = dma[channel].count;
  if (count == 0)
    count = (channel == 3) ? 0x10000 : 0x4000;

  int run_count = count;
  int timing = (cnt_h >> 12) & 0x3;

  // Sound DMA（timing=3, channel 1/2）：固定传输 4 个 word
  bool isSoundDMA = (timing == 3 && (channel == 1 || channel == 2));
  if (isSoundDMA) {
    run_count = 4;
    dma[channel].count -= 4;
    if (dma[channel].count < 0)
      dma[channel].count = 0;
  } else {
    dma[channel].count = 0;
  }

  int width = (cnt_h & 0x0400) ? 4 : 2;
  int srcAdj = (cnt_h >> 7) & 0x3;
  int dstAdj = (cnt_h >> 5) & 0x3;

  // Sound DMA 强制：32-bit 传输，目标地址固定（FIFO 地址不变）
  if (isSoundDMA) {
    width = 4;
    dstAdj = 2; // Fixed
  }

  // DEBUG SOUND DMA
  if (dad == 0x040000A0 || dad == 0x040000A4) {
    static int soundDmaLog = 0;
    if (soundDmaLog++ < 100) {
      if (soundDmaLog % 10 == 0)
        printf("SOUND DMA: channel=%d timing=%d sad=%08X dad=%08X count=%d "
               "width=%d\n",
               channel, timing, sad, dad, dma[channel].count, width);
    }
  }

  static int dmaLog = 0;
  if (dad == 0x05000000 && dmaLog++ < 5) {
    printf("DMA%d PALETTE: SAD=%08X DAD=%08X CNT=%d WIDTH=%d SRCADJ=%d\n",
           channel, sad, dad, count, width, srcAdj);
    printf("ROM AT SAD: %04X %04X %04X %04X\n", bus->read16(sad),
           bus->read16(sad + 2), bus->read16(sad + 4), bus->read16(sad + 6));
  }

  // Perform Transfer
  for (int n = 0; n < run_count; n++) {
    if (width == 4) {
      uint32_t val = bus->read32(sad);
      bus->write32(dad, val);
    } else {
      uint16_t val = bus->read16(sad);
      bus->write16(dad, val);
    }

    // Update Address
    if (srcAdj == 0)
      sad += width;
    else if (srcAdj == 1)
      sad -= width;

    if (dstAdj == 0)
      dad += width;
    else if (dstAdj == 1)
      dad -= width;
    // dstAdj == 3 applies at the end of full transfer
  }

  // Save back internal state to support repeat
  dma[channel].sad = sad;
  dma[channel].dad = dad;

  if (dma[channel].count == 0) {
    if (cnt_h & 0x4000) {                  // IRQ Enable
      requestInterrupt(0x0100 << channel); // DMA0-3 IRQ uses bits 8-11
    }

    if (cnt_h & 0x0200) { // Repeat
      dma[channel].count = bus->read16(regBase + 8);
      // Sound DMA 或 dstAdj==3 时重载目标地址
      if (dstAdj == 3 || isSoundDMA) {
        dma[channel].dad = bus->read32(regBase + 4);
      }
    } else {
      // Disable DMA
      cnt_h &= ~0x8000;
      bus->write16(regBase + 0xA, cnt_h);
      dma[channel].active = false;
    }
  }
}

void GBA::loadBIOS(const std::vector<uint8_t> &data) {
  if (!data.empty()) {
    bus->loadBIOS(data);
    hasBIOS = true;
  }
}

void GBA::loadROM(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    // Load Audio Test BIOS
    std::vector<uint8_t> bios(16 * 1024);
    uint32_t *p = (uint32_t *)bios.data();

    // MOV R0, #0x04000000
    p[0] = 0xE3A00404;

    // MOV R1, #0x80
    p[1] = 0xE3A01080;

    // STRB R1, [R0, #0x84] (SOUNDCNT_X)
    p[2] = 0xE5C01084;

    // LDR R1, =0x1177 (Enable Ch1 L/R, Max Vol)
    p[3] = 0xE3A01C11; // MOV R1, #0x1100
    p[4] = 0xE2811077; // ADD R1, R1, #0x77

    // STRH R1, [R0, #0x80] (SOUNDCNT_L)
    p[5] = 0xE1C018B0;

    // MOV R1, #0
    p[6] = 0xE3A01000;
    // STRH R1, [R0, #0x60]
    p[7] = 0xE1C016B0;

    // LDR R1, =0xF080 (Duty 2, Vol 15)
    p[8] = 0xE3A01CF0; // MOV R1, #0xF000
    p[9] = 0xE2811080; // ADD R1, R1, #0x80
    // STRH R1, [R0, #0x62]
    p[10] = 0xE1C016B2;

    // LDR R1, =0x86D6 (Trigger, Freq)
    p[11] = 0xE3A0186D; // MOV R1, #0x006D0000? No. 0x86D6?
    // Let's use simpler instruction
    // MOV R1, #0x8600
    // ADD R1, R1, #0xD6
    p[11] = 0xE3A01C86; // MOV R1, #0x8600
    p[12] = 0xE28110D6; // ADD R1, R1, #0xD6

    // STRH R1, [R0, #0x64]
    p[13] = 0xE1C016B4;

    // B .
    p[14] = 0xEAFFFFFE;

    bus->loadBIOS(bios);
    return;
  }
  bus->loadROM(data);
  // 无 BIOS 时安装完整的 HLE IRQ handler
  if (!hasBIOS) {
    cpu->setHasBIOS(false);

    // === IRQ Vector (0x18): B 0x120 ===
    // offset = (0x120 - 0x20) / 4 = 0x40
    bus->write32(0x18, 0xEA000040u);

    // === 完整 HLE BIOS IRQ Handler (0x120 - 0x188) ===
    // 保存寄存器到 IRQ 栈
    bus->write32(0x120, 0xE92D500Fu); // STMFD SP!, {R0-R3, R12, LR}
    // 构造 IO 基地址 0x04000200
    bus->write32(0x124, 0xE3A00404u); // MOV R0, #0x04000000
    bus->write32(0x128, 0xE2800C02u); // ADD R0, R0, #0x200
    // 读 IE|IF (32位: low=IE, high=IF)
    bus->write32(0x12C, 0xE5901000u); // LDR R1, [R0]
    // R2 = IE & IF
    bus->write32(0x130, 0xE0012821u); // AND R2, R1, R1, LSR #16
    // 确认 IF（写1清除）
    bus->write32(0x134, 0xE1C020B2u); // STRH R2, [R0, #2]
    // 构造 0x03007FF8 (IntrWait flags 地址)
    bus->write32(0x138, 0xE3A03403u); // MOV R3, #0x03000000
    bus->write32(0x13C, 0xE2833C7Fu); // ADD R3, R3, #0x7F00
    bus->write32(0x140, 0xE28330F8u); // ADD R3, R3, #0xF8
    // 更新 IntrWait flags
    bus->write32(0x144, 0xE593C000u); // LDR R12, [R3]
    bus->write32(0x148, 0xE18CC002u); // ORR R12, R12, R2
    bus->write32(0x14C, 0xE583C000u); // STR R12, [R3]
    // 读游戏 IRQ handler 地址 [0x03007FFC]
    bus->write32(0x150, 0xE593C004u); // LDR R12, [R3, #4]
    // 切换到 System 模式 (mode=0x1F, 保持 I=1)
    bus->write32(0x154, 0xE10F0000u); // MRS R0, CPSR
    bus->write32(0x158, 0xE3C0001Fu); // BIC R0, R0, #0x1F
    bus->write32(0x15C, 0xE380001Fu); // ORR R0, R0, #0x1F
    bus->write32(0x160, 0xE121F000u); // MSR CPSR_c, R0
    // 保存 LR 到 System 栈
    bus->write32(0x164, 0xE92D4000u); // STMFD SP!, {LR}
    // 调用游戏 handler: MOV LR, PC 使 LR=0x170; BX R12 跳转
    bus->write32(0x168, 0xE1A0E00Fu); // MOV LR, PC  (LR = 0x170)
    bus->write32(0x16C, 0xE12FFF1Cu); // BX R12
    // === 游戏 handler 返回到这里 (0x170) ===
    bus->write32(0x170, 0xE8BD4000u); // LDMFD SP!, {LR}
    // 切回 IRQ 模式 (mode=0x12 | I=0x80 = 0x92)
    bus->write32(0x174, 0xE10F0000u); // MRS R0, CPSR
    bus->write32(0x178, 0xE3C0001Fu); // BIC R0, R0, #0x1F
    bus->write32(0x17C, 0xE3800092u); // ORR R0, R0, #0x92
    bus->write32(0x180, 0xE121F000u); // MSR CPSR_c, R0
    // 恢复寄存器
    bus->write32(0x184, 0xE8BD500Fu); // LDMFD SP!, {R0-R3, R12, LR}
    // 从 IRQ 返回 (恢复 CPSR 从 SPSR)
    bus->write32(0x188, 0xE25EF004u); // SUBS PC, LR, #4

    // 锁定 BIOS 区域，防止游戏覆盖 IRQ 向量（真实 GBA 上 BIOS ROM 是只读的）
    bus->lockBIOSVectorTable();

    printf("GBAEmu: Installed HLE BIOS IRQ handler at 0x120-0x188\n");
  } else {
    cpu->setHasBIOS(true);
  }
}

void GBA::updateTimers(int cycles) {
  auto checkSoundDMA = [&](int timerId) {
    for (int c = 1; c <= 2; c++) {
      if (!dma[c].active)
        continue;
      uint32_t regBase = 0x04000000 + 0xB0 + (c * 0xC);
      uint16_t cnt_h = bus->read16(regBase + 0xA);
      if (((cnt_h >> 12) & 0x3) == 3) {
        if (dma[c].dad == 0x040000A0 && apu->timerForFifoA() == timerId) {
          if (apu->fifoA_count() <= 16)
            transferDMA(c);
        } else if (dma[c].dad == 0x040000A4 &&
                   apu->timerForFifoB() == timerId) {
          if (apu->fifoB_count() <= 16)
            transferDMA(c);
        }
      }
    }
  };

  static int logTimerDmaStatus = 0;
  if (logTimerDmaStatus++ % 10000 == 0) {
    if (dma[1].active || dma[2].active) {
      printf("TIMER STATUS: DMA1 Active=%d Dad=%08X  DMA2 Active=%d Dad=%08X  "
             "FifoA cnt=%d (T%d) FifoB cnt=%d (T%d)\n",
             dma[1].active, dma[1].dad, dma[2].active, dma[2].dad,
             apu->fifoA_count(), apu->timerForFifoA(), apu->fifoB_count(),
             apu->timerForFifoB());
    }
  }

  // Basic Timer Implementation
  for (int i = 0; i < 4; i++) {
    uint32_t cnt_h_addr = 0x04000102 + (i * 4);
    uint16_t control = bus->read16(cnt_h_addr); // TMxCNT_H

    if (control & 0x80) { // Timer Enabled
      // Sync control
      timers[i].control = control;

      // Prescaler
      int prescalerSelect = control & 0x3;
      int shift = 0;
      switch (prescalerSelect) {
      case 0:
        shift = 0;
        break; // div 1
      case 1:
        shift = 6;
        break; // div 64
      case 2:
        shift = 8;
        break; // div 256
      case 3:
        shift = 10;
        break; // div 1024
      }

      // Cascade Mode (Count-up)
      bool cascade = (control & 0x4) && (i > 0);
      if (cascade)
        continue; // Handled by previous timer

      timers[i].cycles += cycles;

      int threshold = 1 << shift;
      while (timers[i].cycles >= threshold) {
        timers[i].cycles -= threshold;

        uint32_t cnt_l_addr = 0x04000100 + (i * 4);
        timers[i].counter++;

        if (timers[i].counter == 0) { // Overflow
          // Read the reload latch directly from the bus (where game writes it)
          timers[i].counter = bus->read16(cnt_l_addr);

          static int timerOverflowLog = 0;
          if (timerOverflowLog++ % 44000 == 0)
            printf("TIMER OVERFLOW: Timer %d, Reload Latch = %04X\n", i,
                   timers[i].counter);

          // DirectSound FIFO Pop
          apu->onTimerOverflow(i);
          checkSoundDMA(i);

          if (control & 0x40) { // IRQ Enable
            uint16_t irqMask = 0;
            if (i == 0)
              irqMask = 0x8;
            else if (i == 1)
              irqMask = 0x10;
            else if (i == 2)
              irqMask = 0x20;
            else if (i == 3)
              irqMask = 0x40;
            requestInterrupt(irqMask);
          }

          // Handle Cascade for next timer
          if (i < 3) {
            uint32_t next_ctrl_addr = 0x04000102 + ((i + 1) * 4);
            uint16_t next_ctrl = bus->read16(next_ctrl_addr);
            if ((next_ctrl & 0x80) && (next_ctrl & 0x04)) {
              uint32_t next_cnt_addr = 0x04000100 + ((i + 1) * 4);
              uint16_t next_val = bus->read16(next_cnt_addr);
              next_val++;
              bus->write16(next_cnt_addr, next_val);
              if (next_val == 0) {
                bus->write16(next_cnt_addr, timers[i + 1].reload);
                apu->onTimerOverflow(i + 1);
                checkSoundDMA(i + 1);
                if (next_ctrl & 0x40) {
                  uint16_t flag =
                      (i + 1 == 1) ? 0x10 : (i + 1 == 2 ? 0x20 : 0x40);
                  requestInterrupt(flag);
                }
              }
            }
          }
        }
      }
    }
  }
}

void GBA::requestInterrupt(int id) {
  bus->requestInterrupt(id);
  // printf("GBA: Request Interrupt ID=%X IF=%04X\n", id, if_reg | id);
  checkInterrupts();
}

void GBA::checkInterrupts() {
  uint16_t ie = bus->read16(0x04000200);
  uint16_t if_reg = bus->read16(0x04000202);
  uint16_t ime = bus->read16(0x04000208);

  // Prevent infinite recursive interrupting
  if ((ime & 1) && cpu->isIRQEnabled()) {
    if (ie & if_reg) {
      // printf("GBA: Firing CPU IRQ! IE&IF=%04X\n", ie & if_reg);
      cpu->irq();
    } else {
      // IME is on but no matching interrupt
      // printf("GBA: IME=1 but IE&IF=0. IE=%04X IF=%04X\n", ie, if_reg);
    }
  } else {
    // IME is off
    // if (ie & if_reg) printf("GBA: IRQ Pending but IME=0. IE=%04X IF=%04X\n",
    // ie, if_reg);
  }
}

} // namespace Core
