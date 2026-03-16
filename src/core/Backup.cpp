#include "Backup.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <cstdio>

namespace Core {

// ============================================================
// ROM 标识字符串扫描 — 自动检测存档类型
// ============================================================
Backup::Type Backup::detectFromROM(const uint8_t *rom, size_t size) {
  if (!rom || size == 0)
    return Type::None;

  // 将 ROM 视为字符串进行搜索
  auto contains = [&](const char *needle) -> bool {
    size_t needleLen = strlen(needle);
    if (needleLen > size)
      return false;
    // 在整个 ROM 中搜索标识字符串
    for (size_t i = 0; i <= size - needleLen; i++) {
      if (memcmp(rom + i, needle, needleLen) == 0)
        return true;
    }
    return false;
  };

  // 按优先级检测（更具体的先匹配）
  if (contains("EEPROM_V"))
    return Type::EEPROM_8K; // 默认 8K，某些游戏需要 512B，后续可细化
  if (contains("FLASH1M_V"))
    return Type::Flash128;
  if (contains("FLASH512_V"))
    return Type::Flash64;
  if (contains("FLASH_V"))
    return Type::Flash64;
  if (contains("SRAM_F_V"))
    return Type::SRAM;
  if (contains("SRAM_V"))
    return Type::SRAM;

  return Type::None;
}

// ============================================================
// 构造函数 — 根据类型初始化数据缓冲区
// ============================================================
Backup::Backup(Type type) : type_(type) {
  switch (type_) {
  case Type::SRAM:
    data_.resize(32 * 1024, 0xFF); // 32KB，默认 0xFF（未写入状态）
    break;
  case Type::Flash64:
    data_.resize(64 * 1024, 0xFF); // 64KB
    flashChipId_ = 0x1CC2;        // Macronix MX29L512 (常见)
    break;
  case Type::Flash128:
    data_.resize(128 * 1024, 0xFF); // 128KB
    flashChipId_ = 0x09C2;         // Macronix MX29L010 (常见)
    break;
  case Type::EEPROM_512:
    data_.resize(512, 0xFF); // 512 字节
    eepromAddrBits_ = 6;     // 6 位地址 → 64 个 8 字节块
    break;
  case Type::EEPROM_8K:
    data_.resize(8 * 1024, 0xFF); // 8KB
    eepromAddrBits_ = 14;         // 14 位地址 → 1024 个 8 字节块
    break;
  case Type::None:
  default:
    break;
  }
}

// ============================================================
// 通用读写入口
// ============================================================
uint8_t Backup::read8(uint32_t addr) {
  switch (type_) {
  case Type::SRAM:
    return data_[addr & 0x7FFF]; // 32KB 镜像
  case Type::Flash64:
  case Type::Flash128:
    return flashRead(addr);
  default:
    return 0xFF;
  }
}

void Backup::write8(uint32_t addr, uint8_t value) {
  switch (type_) {
  case Type::SRAM:
    if (data_[addr & 0x7FFF] != value) {
      data_[addr & 0x7FFF] = value;
      dirty_ = true;
    }
    break;
  case Type::Flash64:
  case Type::Flash128:
    flashWrite(addr, value);
    break;
  default:
    break;
  }
}

// ============================================================
// Flash 读取
// ============================================================
uint8_t Backup::flashRead(uint32_t addr) const {
  uint16_t offset = addr & 0xFFFF;

  // ID 模式下特殊返回
  if (flashState_ == FlashState::IDMode) {
    if (offset == 0x0000)
      return flashChipId_ & 0xFF;        // 制造商 ID
    if (offset == 0x0001)
      return (flashChipId_ >> 8) & 0xFF; // 设备 ID
    return 0;
  }

  // 正常读取：Bank偏移
  uint32_t bankOffset = (uint32_t)flashBank_ * 0x10000 + offset;
  if (bankOffset < data_.size())
    return data_[bankOffset];

  return 0xFF;
}

// ============================================================
// Flash 命令状态机
// ============================================================
void Backup::flashWrite(uint32_t addr, uint8_t value) {
  uint16_t offset = addr & 0xFFFF;

  switch (flashState_) {
  case FlashState::Ready:
    if (offset == 0x5555 && value == 0xAA) {
      flashState_ = FlashState::Cmd1;
    }
    break;

  case FlashState::Cmd1:
    if (offset == 0x2AAA && value == 0x55) {
      flashState_ = FlashState::Cmd2;
    } else {
      flashState_ = FlashState::Ready; // 序列中断
    }
    break;

  case FlashState::Cmd2:
    // 第三步命令字节（写入 0x5555）
    if (offset == 0x5555) {
      switch (value) {
      case 0x90: // 进入 ID 模式
        flashState_ = FlashState::IDMode;
        break;
      case 0xF0: // 退出 ID 模式 / 软复位
        flashState_ = FlashState::Ready;
        break;
      case 0x80: // 擦除准备
        flashState_ = FlashState::Erase1;
        break;
      case 0xA0: // 写入准备（单字节编程）
        flashState_ = FlashState::WriteReady;
        break;
      case 0xB0: // Bank 切换（仅 128KB）
        if (type_ == Type::Flash128) {
          flashState_ = FlashState::BankSwitch;
        } else {
          flashState_ = FlashState::Ready;
        }
        break;
      default:
        flashState_ = FlashState::Ready;
        break;
      }
    } else {
      flashState_ = FlashState::Ready;
    }
    break;

  case FlashState::Erase1:
    if (offset == 0x5555 && value == 0xAA) {
      flashState_ = FlashState::Erase2;
    } else {
      flashState_ = FlashState::Ready;
    }
    break;

  case FlashState::Erase2:
    if (offset == 0x2AAA && value == 0x55) {
      flashState_ = FlashState::Erase3;
    } else {
      flashState_ = FlashState::Ready;
    }
    break;

  case FlashState::Erase3:
    if (offset == 0x5555 && value == 0x10) {
      // 全片擦除
      std::fill(data_.begin(), data_.end(), 0xFF);
      dirty_ = true;
    } else if (value == 0x30) {
      // 扇区擦除 (4KB) — offset 高4位确定扇区
      uint32_t sectorStart =
          (uint32_t)flashBank_ * 0x10000 + (offset & 0xF000);
      uint32_t sectorEnd = sectorStart + 0x1000;
      if (sectorEnd <= data_.size()) {
        std::fill(data_.begin() + sectorStart, data_.begin() + sectorEnd,
                  0xFF);
        dirty_ = true;
      }
    }
    flashState_ = FlashState::Ready;
    break;

  case FlashState::WriteReady: {
    // 写入单字节
    uint32_t bankOffset = (uint32_t)flashBank_ * 0x10000 + offset;
    if (bankOffset < data_.size()) {
      if (data_[bankOffset] != (data_[bankOffset] & value)) {
        data_[bankOffset] &= value;
        dirty_ = true;
      }
    }
    flashState_ = FlashState::Ready;
    break;
  }

  case FlashState::BankSwitch:
    if (offset == 0x0000) {
      flashBank_ = value & 0x01; // Bank 0 或 1
      printf("Backup: Flash Bank 切换到 %d\n", flashBank_);
    }
    flashState_ = FlashState::Ready;
    break;

  case FlashState::IDMode:
    if (value == 0xF0) {
      flashState_ = FlashState::Ready; // 退出 ID 模式
    } else if (offset == 0x5555 && value == 0xAA) {
      flashState_ = FlashState::Cmd1; // 允许在 ID 模式下发起新命令
    }
    break;
  }
}

// ============================================================
// EEPROM 串行读
// ============================================================
uint16_t Backup::eepromRead() {
  if (type_ != Type::EEPROM_512 && type_ != Type::EEPROM_8K)
    return 1; // 未就绪时返回 1

  if (eepromState_ == EepromState::ReadData) {
    if (eepromBitsLeft_ > 0) {
      // 从高位到低位逐位输出
      int bitIndex = eepromBitsLeft_ - 1;
      uint16_t bit = (eepromBuffer_ >> bitIndex) & 1;
      eepromBitsLeft_--;

      if (eepromBitsLeft_ == 0) {
        eepromState_ = EepromState::Idle;
      }
      return bit;
    }
    eepromState_ = EepromState::Idle;
  }

  return 1; // 就绪信号（线空闲时为高）
}

// ============================================================
// EEPROM 串行写
// ============================================================
void Backup::eepromWrite(uint16_t value) {
  if (type_ != Type::EEPROM_512 && type_ != Type::EEPROM_8K)
    return;

  eepromProcessBit(value & 1);
}

// ============================================================
// EEPROM 位处理状态机
// ============================================================
void Backup::eepromProcessBit(int bit) {
  switch (eepromState_) {
  case EepromState::Idle:
    // 开始新的传输：第一个位是请求类型的高位
    eepromBuffer_ = bit;
    eepromBitsRead_ = 1;
    eepromState_ = EepromState::ReadCmd; // 暂时当作读命令，后续根据类型位区分
    break;

  case EepromState::ReadCmd: {
    eepromBuffer_ = (eepromBuffer_ << 1) | bit;
    eepromBitsRead_++;

    // 前 2 位是请求类型 (11=读, 10=写)
    // 后续的 eepromAddrBits_ 位是地址
    int totalBitsNeeded = 2 + eepromAddrBits_;

    if (eepromBitsRead_ >= totalBitsNeeded) {
      int requestType = (eepromBuffer_ >> eepromAddrBits_) & 0x3;
      int addrMask = (1 << eepromAddrBits_) - 1;
      eepromAddress_ = eepromBuffer_ & addrMask;

      if (requestType == 0x3) {
        // 读请求：准备输出 64 位数据 (前面有 4 个 0 位)
        uint32_t byteOffset = eepromAddress_ * 8;
        eepromBuffer_ = 0;
        if (byteOffset + 8 <= data_.size()) {
          for (int i = 0; i < 8; i++) {
            eepromBuffer_ = (eepromBuffer_ << 8) | data_[byteOffset + i];
          }
        }
        eepromBitsLeft_ = 68; // 4 个填充 0 + 64 位数据
        // 将 64 位数据放在低 64 位，高 4 位补 0
        eepromState_ = EepromState::ReadData;
      } else if (requestType == 0x2) {
        // 写请求：继续接收 64 位数据
        eepromBuffer_ = 0;
        eepromBitsRead_ = 0;
        eepromState_ = EepromState::WriteCmd;
      } else {
        // 未知类型，回到空闲
        eepromState_ = EepromState::Idle;
      }
    }
    break;
  }

  case EepromState::WriteCmd: {
    eepromBuffer_ = (eepromBuffer_ << 1) | bit;
    eepromBitsRead_++;

    if (eepromBitsRead_ >= 64) {
      // 收到 64 位数据，写入到 EEPROM
      uint32_t byteOffset = eepromAddress_ * 8;
      if (byteOffset + 8 <= data_.size()) {
        for (int i = 7; i >= 0; i--) {
          data_[byteOffset + i] = eepromBuffer_ & 0xFF;
          eepromBuffer_ >>= 8;
        }
        dirty_ = true;
      }
      eepromState_ = EepromState::WriteEnd;
    }
    break;
  }

  case EepromState::WriteEnd:
    // 结束位（应为 0），写操作完成
    eepromState_ = EepromState::Idle;
    break;

  case EepromState::ReadData:
    // 读模式下不应有写入，忽略
    break;
  }
}

// ============================================================
// .sav 文件读写
// ============================================================
bool Backup::loadFromFile(const std::string &path) {
  if (type_ == Type::None || data_.empty())
    return false;

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open())
    return false;

  // 获取文件大小
  file.seekg(0, std::ios::end);
  size_t fileSize = file.tellg();
  file.seekg(0, std::ios::beg);

  if (fileSize == 0)
    return false;

  // 读取数据（取文件大小和缓冲区大小的较小值）
  size_t readSize = std::min(fileSize, data_.size());
  file.read(reinterpret_cast<char *>(data_.data()), readSize);
  file.close();

  printf("Backup: 已加载存档 %s (%zu 字节)\n", path.c_str(), readSize);
  dirty_ = false;
  return true;
}

bool Backup::saveToFile(const std::string &path) const {
  if (type_ == Type::None || data_.empty())
    return false;

  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) {
    printf("Backup: 无法写入存档文件 %s\n", path.c_str());
    return false;
  }

  file.write(reinterpret_cast<const char *>(data_.data()), data_.size());
  file.close();

  printf("Backup: 已保存存档 %s (%zu 字节)\n", path.c_str(), data_.size());
  return true;
}

} // namespace Core
