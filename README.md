# ZPT8PC (Zepto-8 Portable for M5Cardputer)

[日本語版はこちら](README_JA.md)

> **⚠️ EXPERIMENTAL / TRIAL PROJECT**  
> This project is currently in the trial phase. It is an experimental attempt to push the M5Cardputer to its limits. **It is not a high-performance or perfect commercial product.** Expect glitches, severe slowdowns, and crashes due to memory shortages when playing heavy games.

ZPT8 is a highly optimized PICO-8 fantasy console emulator tailored specifically for the M5Cardputer, built upon a customized Zepto-8 core.

<img width="480" height="270" alt="Image" src="https://github.com/user-attachments/assets/10904ae2-a344-4af6-b236-2014e23407d8" />

---

## 🛑 Hardware Limitations (Crucial)
The M5Cardputer (ESP32-S3) has a strict **320KB SRAM (Heap Memory) limit**. PICO-8 was originally designed for PCs with abundant memory.
- **Out of Memory (OOM) Crashes:** Super massive games (e.g., massive RPGs or 3D games) require too much memory to compile the source code on-device. They will run out of memory and crash (or freeze with a black screen) during the loading phase.
- **Performance Drops:** Heavy 3D graphics or games with a massive amount of sprites will cause the frame rate to drop. We have implemented an "Auto Frameskip" feature to maintain the internal game speed, but heavy scenes will look choppy.

---

## 💾 Quick Install via M5Burner

You can easily flash ZPT8 directly onto your M5Cardputer without installing PlatformIO or compiling the source code manually.

1. Open **M5Burner** on your computer.
2. Search for the custom share code in the user-published firmware catalog:
   * **Share Code**: `fetHO26j9cpoFzcc`
3. Connect your M5Cardputer via USB, select your COM port, and click **Burn**!

---

## 📂 How to Play Games

For small games, you can simply place the `.p8` file directly on the SD card and run it.

### 1. Recommended: Create p8 files using the ShrinkO8 Webapp
For most games, following these steps will allow them to run within the limited memory of the actual device.

1. Open [ShrinkO8](https://thisismypassport.github.io/shrinko8/) in your browser.
2. Load the `.p8` or `.p8.png` file you want to play.
3. Execute **Minify** to strip out comments and spaces, compressing the code.
4. Place the saved `.p8` file onto your microSD card and launch the program.

### 2. The Last Resort: Precompiling via `pc8c`
If a blockbuster game (like *Unhaunters*) still crashes due to memory shortage even after extreme compression with ShrinkO8, compiling on-device is impossible.
As a **last resort for games you absolutely must play**, you can use the included `pc8c` tool to precompile the game into bytecode (`.pc8c`) on your PC, bypassing the memory limit.

*Note: To use this method, you need to set up the PlatformIO environment on your PC following the "Build & Development Setup" section.*
```bash
# Example: Compiling a massive game on your PC
tools/pc8_compile.exe game massive_game.p8 massive_game.pc8c
```
Place the generated `.pc8c` file onto your microSD card.

---

## 💻 Build & Development Setup

Steps for building the emulator firmware and cartridge compiler.

### Prerequisites
1. **Visual Studio Code (VSCode)**
2. **PlatformIO IDE Extension** (installed inside VSCode)
3. **M5Cardputer Device** (with a USB-C cable to connect to your PC)

### Dependent Repositories & Libraries
This project relies on the following major external repositories:
* **Zepto-8 Core**: [samhocevar/zepto8](https://github.com/samhocevar/zepto8) (PICO-8 emulator core)
* **M5Unified**: [m5stack/M5Unified](https://github.com/m5stack/M5Unified) (Hardware abstraction layer for M5Stack)
* **M5Cardputer**: [m5stack/M5Cardputer](https://github.com/m5stack/M5Cardputer) (M5Cardputer wrapper library)

### Building and Uploading Firmware

#### 1. Clone the Repository & Dependencies
Clone this repository into a directory named `ZPT8`, then manually clone the required `zepto8` core recursively inside it:
```bash
# Clone ZPT8
git clone https://github.com/Layer812/ZPT8.git ZPT8
cd ZPT8

# Clone the dependent zepto8 core recursively into the ZPT8 folder
git clone --recursive https://github.com/samhocevar/zepto8.git zepto8
```

#### 2. Import Project
Launch VSCode and open the cloned `ZPT8` directory. PlatformIO will automatically recognize the project.

#### 3. Build and Upload Emulator to M5Cardputer
* **Using CLI**:
  ```bash
  # Build only
  pio run -e m5stack-stamps3
  
  # Build & upload to M5Cardputer
  pio run -e m5stack-stamps3 --target upload
  ```

#### 4. Build Cartridge Compiler (`pc8_compile`)
Build the tool to convert `.p8` to `.pc8c` on PC.
* **Using CLI**:
  ```bash
  # Build for native PC
  pio run -e native_tool
  ```

---

## 🎮 Controls

The physical keyboard and side keys of the M5Cardputer are bound to PICO-8 Player 1 inputs.

| ZPT8 Key (M5Cardputer) | PICO-8 Button | In-Game Action |
| :--- | :---: | :--- |
| **Arrow Keys (↑ / ↓ / ⬅️ / ➡️)** | ⬆️ / ⬇️ / ⬅️ / ➡️ | Movement / Directional D-Pad |
| **`O` Key** or **`Z` Key** | 🅾️ (Button 4) | Jump / Confirm / Primary Action |
| **`X` Key** or **`Space` Key** | ❎ (Button 5) | Dash / Cancel / Menu Overlay |
| **Full Alphanumeric Keys** | Text Input | Typing native commands inside the PICO-8 BIOS |

### 🛠️ Integrated Boot File Selector Controls
* **`↑` / `↓` Arrow Keys**: Browse up and down through the available file list.
* **`O` Key**: Load and automatically run (`run`) the selected cartridge.
* **`X` Key**: Cancel selection or return to the console.

---

## 🔧 Troubleshooting

#### Q. A cartridge crashes with an error or the device suddenly resets (reboots).
A. Because the ESP32-S3 operates under a strictly limited memory capacity (320KB RAM), complex or highly memory-consuming cartridges may cause an OutOfMemory error. If you find a cartridge that causes crashes or forced resets, we would appreciate it if you could gently let us know via GitHub Issues! Thank you for your cooperation in making ZPT8 better.

---

## 📜 License & Acknowledgments

* **Zepto-8 Core**: Copyright © 2016–2024 Sam Hocevar (Do What the Fuck You Want to Public License - WTFPL).
* **z8lua Extension**: Customized Lua 5.2 Embedded Subsystem.
* **LodePNG**: Copyright © 2005–2020 Lode Vandevenne (zlib License).
* **Custom Modifications & New Additions**: Copyright © 2026 Layer8. Licensed under the MIT License.
* **Jelpi Sample Asset**: `jelpi.pc8c` is an optimized conversion of "Jelpi Adventures", an official demo cartridge originally created by Lexaloffle Games, provided purely for hardware and performance verification purposes.
