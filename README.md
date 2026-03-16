# M1 GBA 模拟器

一款专为 Apple Silicon (M1/M2/M3) Mac 打造的高性能 Game Boy Advance 模拟器，采用 C++ 与 Objective-C++ 混合开发。该项目依托 Apple 的 **Metal** 图形 API 与统一内存架构 (UMA)，旨在实现低延迟渲染与极致性能。

## 核心特性

### 🔧 ARM7TDMI CPU 引擎
- 完整的 ARM 与 Thumb 双指令集执行支持
- **预解码查找表 (LUT)** 加速指令分发，按 opcode 高 12 位直接映射到处理函数，消除热路径分支预测失败
- 高级模拟 (HLE) BIOS 中断拦截，无需真实 `.bin` 固件即可引导大多数游戏

### ⚡ 高性能内存总线
- **O(1) 页表寻址**：256 项指针数组直接映射 GBA 内存区域 (BIOS/WRAM/VRAM/ROM 等)，取代传统 if-else 链
- 完整的 DMA 传输引擎 (4 通道)，支持 VBlank/HBlank/Sound 触发模式
- IO 寄存器副作用处理与中断标志位的正确 ACK 逻辑

### 🎨 Metal GPU 渲染管线
- macOS 原生 MetalView 显示前端，Compute Shader 直接在 GPU 上完成 GBA PPU 像素解码
- 支持 Mode 0/1/2 平铺背景与 Mode 3/4/5 位图模式
- Sprite (OBJ) 渲染与图层优先级管理
- 仿真线程与渲染线程解耦，通过精细化锁管理避免帧率抖动

### 🔊 DirectSound 音频系统
- FIFO A/B 双通道 DirectSound 流式播放
- 矩形波方波发声器基础支持
- CoreAudio AudioQueue 输出，44.1kHz 采样率
- **NSCondition 条件变量** 精准同步音频缓冲，替代低精度的 sleep 忙等

### 🏗️ 编译优化
- `-O3` 最高优化级别 + `-flto` 链接时优化 (LTO)，支持跨编译单元内联
- `-mcpu=apple-m1` 启用 Apple Silicon 原生指令集优化
- `-arch arm64` 原生 ARM64 编译

## 编译与运行

1. **载入 ROM 游戏**: 将 `.gba` 游戏文件放置于 `rom/` 文件夹中。
2. **编译并运行**:
   ```bash
   make && ./bin/GBAEmu
   ```
   运行后可通过菜单栏 **File → Open ROM...** 动态加载游戏。

## 键盘控制

| GBA 按键 | 键盘映射 |
|----------|----------|
| 方向键 (D-Pad) | ← ↑ → ↓ |
| A | `Z` |
| B | `X` |
| L 肩键 | `A` |
| R 肩键 | `S` |
| Start | `Enter` |
| Select | `Backspace` |

## 目录结构

- `src/core/` — 核心模拟引擎 (CPU, PPU, APU, Bus, Timer, DMA)
- `src/platform/` — macOS 平台集成 (MetalView, CoreAudio, Objective-C++ 桥接)
- `src/platform/shaders/` — Metal Compute/Render Shader
- `src/test/` — 单元测试 (ARM/Thumb 指令、LZ77 解压)

## 待完善

- PPU 图层混合 (Alpha Blending) 与窗口遮罩 (Window)
- 仿射变换精度提升 (Mode 1/2 旋转缩放)
- GB 传统音源 (波形/噪音通道) 完整复刻
- 卡带存档支持 (SRAM / Flash / EEPROM)
- 手柄输入绑定与即时存档 (Save States)
