#include "Bus.h"
#include "APU.h"
#include "CPU.h"
#include "Debugger.h"

namespace Core {

Bus::Bus() {
  bios.resize(16 * 1024);
  wram_board.resize(256 * 1024);
  wram_chip.resize(32 * 1024);
  io_regs.resize(0x1000); // 扩展至覆盖 0x04000800 (WAITCNT) 等
  // KEYINPUT (0x04000130) 初始化为 0x03FF (所有按键抬起)
  io_regs[0x130] = 0xFF;
  io_regs[0x131] = 0x03;
  palette.resize(1024);
  vram.resize(96 * 1024);
  oam.resize(1024);
  rom.resize(32 * 1024 * 1024); // Pre-allocate max size for simplicity
}

Bus::~Bus() {}

void Bus::loadBIOS(const std::vector<uint8_t> &data) {
  if (data.size() <= bios.size()) {
    std::copy(data.begin(), data.end(), bios.begin());
    vectorTableWritable_ = false; // 真实 BIOS 的向量表只读
  }
}

void Bus::loadROM(const std::vector<uint8_t> &data) {
  if (data.size() <= rom.size()) {
    std::copy(data.begin(), data.end(), rom.begin());
  }
}

uint8_t Bus::read8(uint32_t addr) {
  // Simple memory map implementation
  if (addr < 0x00004000) {
    return bios[addr];
  } else if (addr >= 0x02000000 && addr < 0x03000000) {
    // 256KB On-board WRAM mirrored
    return wram_board[(addr - 0x02000000) & 0x3FFFF];
  } else if (addr >= 0x03000000 && addr < 0x04000000) {
    // 32KB On-chip WRAM mirrored
    return wram_chip[(addr - 0x03000000) & 0x7FFF];
  } else if (addr >= 0x04000000 && addr < 0x04001000) {
    return io_regs[addr - 0x04000000];
  } else if (addr >= 0x05000000 && addr < 0x05000400) {
    return palette[addr - 0x05000000];
  } else if (addr >= 0x06000000 && addr < 0x06018000) {
    return vram[addr - 0x06000000];
  } else if (addr >= 0x07000000 && addr < 0x07000400) {
    return oam[addr - 0x07000000];
  } else if (addr >= 0x08000000) {
    // ROM Mirroring handled simplistically
    if (addr - 0x08000000 < rom.size())
      return rom[addr - 0x08000000];
  }

  // Unmapped
  Debugger::getInstance().logBusRead(addr, 0, 8);
  return 0;
}

uint16_t Bus::read16(uint32_t addr) {
  // 快速路径：BIOS 访问 (0x00xxxxxx)
  if (addr < 0x3FFF) {
    return *(uint16_t *)&bios[addr & ~1];
  }
  // 快速路径：IWRAM (0x03xxxxxx)
  if ((addr >> 24) == 0x03) {
    return *(uint16_t *)&wram_chip[addr & 0x7FFE];
  }
  // 快速路径：EWRAM (0x02xxxxxx)
  if ((addr >> 24) == 0x02) {
    return *(uint16_t *)&wram_board[addr & 0x3FFFE];
  }
  // 快速路径：ROM (0x08)
  if ((addr >> 24) == 0x08) {
    if ((addr & 0x00FFFFFF) < rom.size() - 1)
      return *(uint16_t *)&rom[(addr & 0x00FFFFFF) & ~1];
    return 0; // Out of bounds ROM read
  }
  return read8(addr) | (read8(addr + 1) << 8);
}

uint32_t Bus::read32(uint32_t addr) {
  // 快速路径：BIOS
  if (addr < 0x3FFC) {
    return *(uint32_t *)&bios[addr & ~3];
  }
  // 快速路径：IWRAM
  if ((addr >> 24) == 0x03) {
    return *(uint32_t *)&wram_chip[addr & 0x7FFC];
  }
  // 快速路径：EWRAM
  if ((addr >> 24) == 0x02) {
    return *(uint32_t *)&wram_board[addr & 0x3FFFC];
  }
  // 快速路径：ROM (0x08)
  if ((addr >> 24) == 0x08) {
    if ((addr & 0x00FFFFFF) < rom.size() - 3)
      return *(uint32_t *)&rom[(addr & 0x00FFFFFF) & ~3];
    return 0; // Out of bounds ROM read
  }
  return read16(addr) | (read16(addr + 2) << 16);
}

void Bus::write8(uint32_t addr, uint8_t value) {
  // BIOS 区域写保护：加载真实 BIOS 后或 lockBIOSVectorTable 后整个区域只读
  // vectorTableWritable_=true 时允许写入（供 HLE handler 安装）
  if (addr < 0x00004000) {
    if (vectorTableWritable_) {
      bios[addr] = value;
    }
    return;
  }

  if (addr >= 0x02000000 && addr < 0x03000000) {
    // 256KB On-board WRAM mirrored
    wram_board[(addr - 0x02000000) % 0x40000] = value;
  } else if (addr >= 0x03000000 && addr < 0x04000000) {
    // 32KB On-chip WRAM mirrored
    wram_chip[(addr - 0x03000000) % 0x8000] = value;
  } else if (addr >= 0x04000000 && addr < 0x04000400) {
    uint32_t offset = addr - 0x04000000;
    if (apu && offset >= 0x60 && offset <= 0xA8) {
      apu->write8(addr, value);
    }
    if (offset == 0x202 || offset == 0x203) {
      // IF register clear by writing 1
      io_regs[offset] &= ~value;
    } else {
      io_regs[offset] = value;
    }
  } else if (addr >= 0x05000000 && addr < 0x05000400) {
    palette[addr - 0x05000000] = value;
  } else if (addr >= 0x06000000 && addr < 0x06018000) {
    vram[addr - 0x06000000] = value;
  } else if (addr >= 0x07000000 && addr < 0x07000400) {
    oam[addr - 0x07000000] = value;
  }
}

void Bus::write16(uint32_t addr, uint16_t value) {
  // Write to IO
  if ((addr >> 24) == 0x04) {
    uint32_t offset = addr & 0x3FF;
    if (apu && offset >= 0x60 && offset <= 0xA8) {
      apu->write16(addr, value);
    }
    if (offset >= 0xB0 && offset <= 0xDF) {
      static int dmaWriteLog = 0;
      if (dmaWriteLog++ < 100) {
        printf("BUS DMA WRITE16: addr=%08X offset=%04X value=%04X\n", addr,
               offset, value);
      }
    }
    if (offset == 0x202) {
      // REG_IF is acknowledge-by-writing-1
      uint16_t currentIF = *(uint16_t *)&io_regs[0x202];
      *(uint16_t *)&io_regs[0x202] = currentIF & ~value;
    } else {
      *(uint16_t *)&io_regs[offset] = value;
    }
    return;
  }
  // 快速路径：IWRAM
  if ((addr >> 24) == 0x03) {
    *(uint16_t *)&wram_chip[addr & 0x7FFE] = value;
    return;
  }
  // 快速路径：EWRAM
  if ((addr >> 24) == 0x02) {
    *(uint16_t *)&wram_board[addr & 0x3FFFE] = value;
    return;
  }
  // Palette (0x05)
  if ((addr >> 24) == 0x05) {
    *(uint16_t *)&palette[(addr - 0x05000000) & 0x3FE] = value;
    return;
  }
  // VRAM (0x06)
  if ((addr >> 24) == 0x06) {
    *(uint16_t *)&vram[(addr - 0x06000000) % 0x18000] = value;
    return;
  }
  // OAM (0x07)
  if ((addr >> 24) == 0x07) {
    *(uint16_t *)&oam[(addr - 0x07000000) & 0x3FE] = value;
    return;
  }
  write8(addr, value & 0xFF);
  write8(addr + 1, (value >> 8) & 0xFF);
}

void Bus::write32(uint32_t addr, uint32_t value) {
  // IO Registers
  if ((addr >> 24) == 0x04) {
    uint32_t offset = addr & 0x3FF;
    // APU FIFO 必须整字写入
    if (apu && offset >= 0x60 && offset <= 0xA8) {
      apu->write32(addr, value);
      return;
    }
    // DMA SAD/DAD 等 32 位寄存器直接写（不含 CNT_H 等需要副作用的地址）
    if (offset >= 0xB0 && offset <= 0xDF && (offset & 3) != 0xA) {
      *(uint32_t *)&io_regs[offset] = value;
      return;
    }
    // 其他 IO 通过 write16 拆分，保留副作用
    write16(addr, value & 0xFFFF);
    write16(addr + 2, (value >> 16) & 0xFFFF);
    return;
  }
  // 快速路径：IWRAM
  if ((addr >> 24) == 0x03) {
    *(uint32_t *)&wram_chip[addr & 0x7FFC] = value;
    return;
  }
  // 快速路径：EWRAM
  if ((addr >> 24) == 0x02) {
    *(uint32_t *)&wram_board[addr & 0x3FFFC] = value;
    return;
  }
  // Palette (0x05)
  if ((addr >> 24) == 0x05) {
    *(uint32_t *)&palette[(addr - 0x05000000) & 0x3FC] = value;
    return;
  }
  // VRAM (0x06)
  if ((addr >> 24) == 0x06) {
    *(uint32_t *)&vram[(addr - 0x06000000) % 0x18000] = value;
    return;
  }
  // OAM (0x07)
  if ((addr >> 24) == 0x07) {
    *(uint32_t *)&oam[(addr - 0x07000000) & 0x3FC] = value;
    return;
  }
  write16(addr, value & 0xFFFF);
  write16(addr + 2, (value >> 16) & 0xFFFF);
}

void Bus::setKeyInput(uint16_t value) {
  // 0x4000130 - 0x04000000 = 0x130
  uint32_t offset = 0x130;
  if (offset < io_regs.size()) {
    io_regs[offset] = value & 0xFF;
    io_regs[offset + 1] = (value >> 8) & 0xFF;
  }
}

void Bus::requestInterrupt(uint16_t flag) {
  uint16_t currentIF = *(uint16_t *)&io_regs[0x202];
  *(uint16_t *)&io_regs[0x202] = currentIF | flag;
}

// setAPU moved to header inline

} // namespace Core
