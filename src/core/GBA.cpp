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
    if (line < VISIBLE_LINES) {
      size_t pixelStride = stride / 4;
      ppu->renderScanline(line, buffer + (line * pixelStride));
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
  Debugger::getInstance().flush();
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

void GBA::checkDMA() {
  // Check all 4 channels
  for (int i = 0; i < 4; i++) {
    // Read Control Register High (Enable bit 15)
    // DMA0CNT_H is 0xBA, etc.
    // Base: 0x040000B0 + i*12 + 10 (offset to CNT_H)
    // Actually we should store state in `dma` struct when registers are
    // written, but for now, let's just read from Bus or assume registers
    // are updated. Wait, GBA class doesn't trap IO writes instantly unless
    // we hook Bus. Let's rely on polling for this step or reading from Bus
    // IORegs.

    uint32_t regBase = 0x04000000 + 0xB0 + (i * 0xC);
    uint16_t cnt_h = bus->read16(regBase + 0xA);

    if (!(cnt_h & 0x8000))
      continue; // Not enabled

    // Timing Mode (Bits 12-13)
    // 00 = Immediate
    // 01 = VBlank
    // 10 = HBlank
    // 11 = Special
    int timing = (cnt_h >> 12) & 0x3;

    bool trigger = false;
    if (timing == 0)
      trigger = true; // Immediate - should have happened on write, but
                      // polling works for test
    // TODO: Add VBlank/HBlank triggers

    if (trigger) {
      transferDMA(i);
    }
  }
}

void GBA::transferDMA(int channel) {
  uint32_t regBase = 0x04000000 + 0xB0 + (channel * 0xC);

  // Read Registers
  uint32_t sad = bus->read32(regBase);
  uint32_t dad = bus->read32(regBase + 4);
  uint16_t cnt_l = bus->read16(regBase + 8);
  uint16_t cnt_h = bus->read16(regBase + 0xA);

  int count = cnt_l;
  if (count == 0)
    count = (channel == 3) ? 0x10000 : 0x4000;

  int width = (cnt_h & 0x0400) ? 4 : 2; // 32-bit or 16-bit
  int srcAdj = (cnt_h >> 7) & 0x3; // 0=Inc, 1=Dec, 2=Fixed, 3=Inc prohibited?
  int dstAdj = (cnt_h >> 5) & 0x3; // 0=Inc, 1=Dec, 2=Fixed, 3=Reload

  // Perform Transfer
  for (int n = 0; n < count; n++) {
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
    // Fixed (2) does nothing

    if (dstAdj == 0)
      dad += width;
    else if (dstAdj == 1)
      dad -= width;
    // Fixed (2) does nothing
  }

  // Disable if not repeat (Repeat Bit 9)
  // Most transfers clear Enable bit upon completion unless Repeat is set
  // (for Vblank/Hblank/etc) Immediate always clears.
  if (!((cnt_h >> 9) & 1) || ((cnt_h >> 12) & 3) == 0) {
    // Clear Enable bit
    bus->write16(regBase + 0xA, cnt_h & ~0x8000);
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
  // 无 BIOS 时安装最小 IRQ 向量，使游戏能响应 VBlank 等中断
  if (!hasBIOS) {
    bus->write32(0x18, 0xE59FF000u); // LDR PC, [PC, #-4]
    bus->write32(0x1C, 0x00000020u); // 默认跳转到 0x20
    bus->write32(0x20, 0xE25EF004u); // SUBS PC, R14, #4  (return from IRQ)
  }
}

void GBA::updateTimers(int cycles) {
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
        uint16_t val = bus->read16(cnt_l_addr);
        val++;
        bus->write16(cnt_l_addr, val);

        if (val == 0) { // Overflow
                        // Reload (Latch)
                        // We need to fetch the reload value.
          // Since we don't have separate storage for reload in Bus,
          // and we can't easily intercept the write to store it in
          // 'timers[i].reload' without Bus modification, we will assume for
          // now that standard practice is to write reload to TMxCNT_L. But
          // wait, if we write to it, we update the counter. The GBA has an
          // internal Reload latch. When we write to TMxCNT_L, it sets the
          // Reload Latch (and the Counter?). Let's rely on our
          // 'timers[i].reload' which we likely haven't set yet! Fix: We
          // need to capture writes to TMxCNT_L.

          // For now, let's just reload with 0 to keep it running.
          bus->write16(cnt_l_addr, timers[i].reload);

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

void GBA::requestInterrupt(uint16_t flag) {
  uint32_t if_addr = 0x04000202;
  uint16_t currentIF = bus->read16(if_addr);
  bus->write16(if_addr, currentIF | flag);
}

void GBA::checkInterrupts() {
  uint16_t IE = bus->read16(0x04000200);
  uint16_t IF = bus->read16(0x04000202);
  uint16_t IME = bus->read16(0x04000208);

  // IME bit 0
  if (IME & 1) {
    if (IE & IF) {
      cpu->irq();
    }
  }
}

} // namespace Core
