# ZPT8PC (Zepto-8 Portable for M5Cardputer)

[日本語版はこちら](README_JA.md)

> **⚠️ EXPERIMENTAL / TRIAL PROJECT**  
> This project is currently in the trial phase. It is an experimental attempt to push the M5Cardputer to its limits. **It is not a high-performance or perfect commercial product.** Expect glitches, severe slowdowns, and crashes when playing complex games.

ZPT8PC is a highly optimized PICO-8 fantasy console emulator tailored specifically for the M5Cardputer, built upon a customized Zepto-8 core. 

<img width="480" height="270" alt="Image" src="https://github.com/user-attachments/assets/10904ae2-a344-4af6-b236-2014e23407d8" />

---

## 🛑 Hardware Limitations (Crucial)
The M5Cardputer (ESP32-S3) has a strict **320KB SRAM (Heap Memory) limit**. PICO-8 was originally designed for PCs with abundant memory.
- **Out of Memory (OOM) Crashes:** Large, complex games (e.g., massive RPGs or 3D games) require too much memory to compile the Lua code on-device. They will run out of memory and crash (black screen) during the loading phase.
- **Performance Drops:** Heavy graphics will cause the frame rate to drop. We have implemented an "Auto Frameskip" feature to maintain the internal game speed, but heavy scenes will look choppy.

---

## 💾 Quick Install via M5Burner

You can easily flash ZPT8 directly onto your M5Cardputer without installing PlatformIO or compiling the source code manually!

1. Open **M5Burner** on your computer.
2. Search for the custom share code in the user-published firmware catalog:
   * **Share Code**: `fetHO26j9cpoFzcc`
3. Connect your M5Cardputer via USB, select your COM port, and click **Burn**!

---

## 📂 How to Play Games

Because of the strict memory limits, **you cannot simply throw large `.p8` files onto the SD card.** You must optimize them first.

### 1. Recommended Method: Minification via ShrinkO8
For the vast majority of games, simply minifying (shrinking) the Lua code is enough to allow the Cardputer to compile and run them on-device.

We highly recommend using **[ShrinkO8](https://thisismypassport.github.io/shrinko8/)** as your primary tool.
1. Open [ShrinkO8](https://thisismypassport.github.io/shrinko8/) in your browser.
2. Load your target `.p8` or `.p8.png` file.
3. Click **Minify** to strip out comments, spaces, and optimize the Lua code.
4. Save the minified `.p8` file and place it on your microSD card.

### 2. The Last Resort: Precompiling via `pc8c`
If a game is still too massive to run even after using ShrinkO8 (like *Unhaunters*), the on-device compiler will still OOM crash. 
As a **last resort for games you absolutely must play**, you can use the included `pc8c` tool to precompile the game into bytecode (`.pc8c`) on your PC, bypassing the Cardputer's memory limits.

*Note: This requires setting up the build environment (PlatformIO) as described in the "Build & Development Setup" section.*
```bash
# Example: Compiling a massive game on your PC
tools/pc8_compile.exe game massive_game.p8 massive_game.pc8c
```
Place the resulting `.pc8c` file on your SD card. 

---

## 💻 Build & Development Setup

Steps for building the emulator firmware and compilation utilities locally.

### Prerequisites
1. **Visual Studio Code (VSCode)**
2. **PlatformIO IDE Extension** (installed inside VSCode)
3. **M5Cardputer Device** (with a USB-C cable to connect to your PC)

### Dependent Repositories & Libraries
This project relies on the following repositories:
* **Zepto-8 Core**: [samhocevar/zepto8](https://github.com/samhocevar/zepto8) (PICO-8 emulator core)
* **M5Unified**: [m5stack/M5Unified](https://github.com/m5stack/M5Unified) (Hardware abstraction layer for M5Stack)
* **M5Cardputer**: [m5stack/M5Cardputer](https://github.com/m5stack/M5Cardputer) (M5Cardputer library)

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
Launch VSCode and open the cloned `ZPT8` directory (containing `platformio.ini`). PlatformIO will automatically initialize.

#### 3. Build and Upload Emulator to M5Cardputer
* **Using CLI**:
  ```bash
  # Compile the code
  pio run -e m5stack-stamps3
  
  # Compile and upload to the connected M5Cardputer
  pio run -e m5stack-stamps3 --target upload
  ```

#### 4. Build Cartridge Compiler (`pc8_compile`)
Compile the native desktop utility that converts `.p8` cartridges into the binary `.pc8c` format.
* **Using CLI**:
  ```bash
  # Compile for your native desktop environment
  pio run -e native_tool
  ```

---

## 🎮 Controls

The physical keyboard and side keys of the M5Cardputer are bound to PICO-8 Player 1 inputs:

| ZPT8 Key (M5Cardputer) | PICO-8 Button | In-Game Action |
| :--- | :---: | :--- |
| **Arrow Keys (↑ / ↓ / ⬅️ / ➡️)** | ⬆️ / ⬇️ / ⬅️ / ➡️ | Movement / Directional D-Pad |
| **`O` Key** or **`Z` Key** | 🅾️ (Button 4) | Jump / Confirm / Primary Action |
| **`X` Key** or **`Space` Key** | ❎ (Button 5) | Dash / Cancel / Menu Overlay |
| **Full Alphanumeric Keys** | Text Input | Typing native commands inside the PICO-8 BIOS |

### 🛠️ Integrated Boot File Selector Controls
* **`↑` / `↓` Arrow Keys**: Browse up and down through the list of available files.
* **`O` Key**: Load and automatically run (`run`) the selected cartridge.
* **`X` Key**: Cancel selection or drop back to console.

---

## 📜 License & Acknowledgments

* **Zepto-8 Core**: Copyright © 2016–2024 Sam Hocevar (Do What the Fuck You Want to Public License - WTFPL).
* **z8lua Extension**: Customized Lua 5.2 Embedded Subsystem.
* **LodePNG**: Copyright © 2005–2020 Lode Vandevenne (zlib License).
* **Custom Modifications & New Additions**: Copyright © 2026 Layer8. Licensed under the MIT License.
* **Jelpi Sample Asset**: `jelpi.pc8c` is an optimized conversion of "Jelpi Adventures", an official demo cartridge originally created by Lexaloffle Games, provided purely for hardware and performance verification purposes.
