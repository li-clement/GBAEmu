# GBA Emulator (M1 Mac)

GBA 模拟器，支持运行 `rom` 目录下的 .gba 游戏。

## 运行游戏

1. **放入 ROM**：将 `.gba` 文件放到项目根目录下的 `rom/` 文件夹中。
2. **编译并运行**：
   ```bash
   make && ./bin/GBAEmu
   ```
   或从 Xcode/终端直接运行 `bin/GBAEmu`（需在项目根目录执行，以便正确找到 `rom/`）。

3. **BIOS（推荐）**：将 GBA BIOS（16KB）放入 `rom/` 并命名为 `gba_bios.bin` 或 `bios.bin`，模拟器会自动加载，可提升兼容性与画面表现。

## 按键

- 方向键：十字键  
- Z / X：A / B  
- Enter：Start  
- Backspace：Select  
- A / S：L / R  

## 打开其他 ROM

菜单 **File → Open ROM...** 可随时选择其他 .gba 或 .bin 文件加载。
