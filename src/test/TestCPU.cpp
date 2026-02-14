#include "../core/Bus.h"
#include "../core/CPU.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

using namespace Core;

void testARMDataProcessing() {
  std::cout << "Testing ARM Data Processing..." << std::endl;
  auto bus = std::make_shared<Bus>();
  auto cpu = std::make_unique<CPU>(bus);

  // MOV R0, #42
  // 1110 001 1101 0 0000 0000 0000 00101010
  // E3A0002A
  bus->write32(0x00000000, 0xE3A0002A);

  // ADD R1, R0, #10
  // 1110 001 0100 0 0000 0001 0000 00001010
  // E280100A
  bus->write32(0x00000004, 0xE280100A);

  // Infinite Loop (B .)
  // EAFFFFFE
  bus->write32(0x00000008, 0xEAFFFFFE);

  cpu->reset();
  cpu->setPC(0x00000000);

  // Pipeline fill
  // Fetch 0, Fetch 4
  // Execute 0 (MOV)
  cpu->step();
  // Execute 4 (ADD)
  cpu->step();

  assert(cpu->getRegister(0) == 42);
  assert(cpu->getRegister(1) == 52);

  std::cout << "ARM Data Processing Passed!" << std::endl;
}

void testThumbDataProcessing() {
  std::cout << "Testing Thumb Data Processing..." << std::endl;
  auto bus = std::make_shared<Bus>();
  auto cpu = std::make_unique<CPU>(bus);

  // MOV R0, #10 (MOVS R0, #10)
  // 001 00 000 00001010
  // 200A
  bus->write16(0x00000000, 0x200A);

  // ADD R0, #5 (ADDS R0, #5)
  // 001 10 000 00000101
  // 3005
  bus->write16(0x00000002, 0x3005);

  // B .
  // E7FE
  bus->write16(0x00000004, 0xE7FE);

  cpu->reset();
  // Switch to Thumb mode
  cpu->setCPSR(cpu->getCPSR() | 0x20);
  cpu->setPC(0x00000000);

  cpu->step(); // MOV
  cpu->step(); // ADD

  assert(cpu->getRegister(0) == 15);

  std::cout << "Thumb Data Processing Passed!" << std::endl;
}

void testLZ77() {
  std::cout << "Testing LZ77 Decompression..." << std::endl;
  auto bus = std::make_shared<Bus>();
  auto cpu = std::make_unique<CPU>(bus);

  // Prepare compressed data at 0x02000000
  // "A" + 7 "A"s
  std::vector<uint8_t> data = {
      0x10, 0x08, 0x00, 0x00, // Header: LZ77, Size 8
      0x40,                   // Flags: 0 (Byte), 1 (Comp), 0...
      0x41,                   // Byte 'A'
      0x40, 0x00              // Block: Len=(4+3)=7, Disp=(0+1)=1
  };

  for (size_t i = 0; i < data.size(); i++) {
    bus->write8(0x02000000 + i, data[i]);
  }

  // SWI 0x11 (ARM)
  bus->write32(0x00000000, 0xEF110000);

  // Infinite loop to stop
  bus->write32(0x00000004, 0xEAFFFFFE);

  cpu->reset();
  cpu->setPC(0x00000000);

  // Set up registers for SWI 0x11 (LZ77UnComp)
  // R0 = Source, R1 = Dest
  cpu->setRegister(0, 0x02000000);
  cpu->setRegister(1, 0x02000100);

  // Execute SWI
  cpu->step(); // Pipeline fill 1
  cpu->step(); // Pipeline fill 2 (actually SWI executes in Execute stage)

  // Verify results
  for (int i = 0; i < 8; i++) {
    uint8_t val = bus->read8(0x02000100 + i);
    if (val != 0x41) {
      std::cerr << "LZ77 Mismatch at " << i << ": " << (int)val << std::endl;
    }
    assert(val == 0x41);
  }

  std::cout << "LZ77 Decompression Passed!" << std::endl;
}

int main() {
  try {
    testARMDataProcessing();
    testThumbDataProcessing();
    testLZ77();
    std::cout << "All Tests Passed!" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Test Failed: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
