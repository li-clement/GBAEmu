#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Core {

// GBA 卡带存档后端 — 统一接口封装 SRAM / Flash / EEPROM 三种实现
class Backup {
public:
  enum class Type {
    None,       // 无存档
    SRAM,       // 32KB SRAM，直接 8 位读写
    Flash64,    // 64KB Flash (Macronix/Sanyo)
    Flash128,   // 128KB Flash (Macronix)
    EEPROM_512, // 512B EEPROM (6 位地址)
    EEPROM_8K   // 8KB EEPROM (14 位地址)
  };

  // 扫描 ROM 数据中的标识字符串，自动检测存档类型
  static Type detectFromROM(const uint8_t *rom, size_t size);

  Backup(Type type);
  ~Backup() = default;

  // === SRAM / Flash 访问（地址空间 0x0E000000） ===
  uint8_t read8(uint32_t addr);
  void write8(uint32_t addr, uint8_t value);

  // === EEPROM 串行访问（DMA-3 通过 0x0D000000+ 触发） ===
  uint16_t eepromRead();
  void eepromWrite(uint16_t value);

  // === .sav 文件持久化 ===
  bool loadFromFile(const std::string &path);
  bool saveToFile(const std::string &path) const;

  Type getType() const { return type_; }
  bool isDirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

private:
  Type type_;
  std::vector<uint8_t> data_; // 存档数据缓冲区
  bool dirty_ = false;        // 数据是否被修改（需要写回磁盘）

  // ========== Flash 状态机 ==========
  enum class FlashState {
    Ready,       // 等待命令序列开始
    Cmd1,        // 收到 0x5555=0xAA
    Cmd2,        // 收到 0x2AAA=0x55，等待命令字节
    Erase1,      // 擦除准备：等待第二组 0xAA
    Erase2,      // 擦除准备：等待第二组 0x55
    Erase3,      // 等待具体擦除命令
    WriteReady,  // 等待写入单字节
    BankSwitch,  // 等待 Bank 编号（仅 128KB）
    IDMode       // 芯片 ID 模式
  };

  FlashState flashState_ = FlashState::Ready;
  uint8_t flashBank_ = 0;       // 当前 Bank（0 或 1，仅 128KB Flash 使用）
  uint16_t flashChipId_ = 0;    // 芯片 ID（ID 模式下读取）

  void flashWrite(uint32_t addr, uint8_t value);
  uint8_t flashRead(uint32_t addr) const;

  // ========== EEPROM 状态机 ==========
  enum class EepromState {
    Idle,        // 空闲
    ReadCmd,     // 正在接收读命令（类型+地址）
    ReadData,    // 正在按位输出 64 位数据
    WriteCmd,    // 正在接收写命令（类型+地址+数据）
    WriteEnd     // 等待结束位
  };

  EepromState eepromState_ = EepromState::Idle;
  uint64_t eepromBuffer_ = 0;  // 位移位寄存器
  int eepromBitsRead_ = 0;     // 当前已读入的位数
  int eepromBitsLeft_ = 0;     // 剩余要输出的位数
  int eepromAddress_ = 0;      // 当前操作地址（以 8 字节为单位）
  int eepromAddrBits_ = 0;     // 地址位数（6 或 14）

  void eepromProcessBit(int bit);
};

} // namespace Core
