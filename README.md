# ZPT8 (Zepto-8 Portable for M5Cardputer)

[日本語版はこちら](README_JA.md)

ZPT8 is a highly optimized PICO-8 fantasy console emulator tailored specifically for the M5Cardputer, built upon a customized Zepto-8 core. 

By employing aggressive bare-metal memory hacks—including static buffer reuse, custom zero-overhead Lua memory allocators, and real-time aggressive Garbage Collection (GC)—ZPT8 shatters the restrictive 320KB RAM barrier of the ESP32-S3. This allows it to successfully boot standalone system BIOS and run heavy, complex PICO-8 cartridges converted into the optimized `.pc8c` format right in the palm of your hand.<br>
<img width="480" height="270" alt="Image" src="https://github.com/user-attachments/assets/10904ae2-a344-4af6-b236-2014e23407d8" />

---

## 💾 Quick Install via M5Burner

You can easily flash ZPT8 directly onto your M5Cardputer without installing PlatformIO or compiling the source code manually!

1. Open **M5Burner** on your computer.
2. Search for the custom share code in the user-published firmware catalog:
   * **Share Code**: `Uv0jV9Mo8hxCK7Gf`
3. Connect your M5Cardputer via USB, select your COM port, and click **Burn**!

---

## ✨ Features

* **Display Optimization**: Maps the native 128x128 PICO-8 canvas directly onto the center of the M5Cardputer screen with dedicated fast-path rendering utilities.
* **Extreme Memory Footprint Reduction**: 
  * Reuses a fixed 64KB static code buffer (BSS section) to push heap allocation overhead during cartridge swapping down to exactly **0 bytes**.
  * Eliminates transient heap retention by stripping out standard C++ `std::string` copies.
* **Bare-Metal Lua Allocator**: Overrides the internal quota restrictions of `z8lua`, opening up 100% of the ESP32's raw remaining free heap directly to the Lua state.
* **Aggressive Garbage Collection**: Forces the Lua engine into a high-frequency recycling mode (`LUA_GCSETPAUSE` at 100, `LUA_GCSETSTEPMUL` at 500) to safely execute volatile processes within tight 30KB–90KB operational margins.
* **Dedicated Audio Pipeline**: Offloads sound synthesis to Core 0 via FreeRTOS tasks, driving steady dual-buffered 11025Hz audio down-sampling without choking the main frame loop.

---

## 📂 SD Card File Layout

Structure your microSD card root directory as follows. All cartridges must be pre-compiled into the `.pc8c` binary format to allow direct flash memory mapping and ultra-low RAM overhead:

```text
SD Card Root/
├── bios.pc8c          # System BIOS for the ZPT8 console environment
├── jelpi.pc8c         # Default fallback/demo verification game (Recommended)
├── 31991.pc8c         # "El Dorado" optimized binary cartridge
└── any_other_game.pc8c # Standard games reside directly on the storage base
```

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

### Optional Tools
* **Shrinko8**: A PICO-8 optimizer and minifier. Although not required to compile ZPT8 itself, if you need to optimize and compress your Lua code size before converting to `.pc8c`, you can clone and use Shrinko8 from the official repository:
  ```bash
  git clone https://github.com/thisistherong/shrinko8.git
  ```

### Building and Uploading Firmware

#### 1. Clone the Repository (with Submodules)
Clone this repository recursively into a directory named `ZPT8` to fetch the source code along with all required submodules (such as `zepto8`):
```bash
git clone --recursive https://github.com/Layer812/ZPT8.git ZPT8
```
If you have already cloned the repository without submodules, navigate into the directory and initialize them:
```bash
cd ZPT8
git submodule update --init --recursive
```

#### 2. Import Project
Launch VSCode and open the cloned `ZPT8` directory (containing `platformio.ini`). PlatformIO will automatically initialize.

#### 3. Build and Upload Emulator to M5Cardputer
This project targets the M5Cardputer (internally driven by M5Stack StampS3).
* **Using VSCode GUI**:
  1. Click the PlatformIO sidebar icon (the ant icon).
  2. Under **Project Tasks**, navigate to `env:m5stack-stamps3` ➔ **General** ➔ **Upload** to build and flash.
  3. Start **Monitor** under the same section to inspect debug output.
* **Using CLI**:
  ```bash
  # Compile the code
  pio run -e m5stack-stamps3
  
  # Compile and upload to the connected M5Cardputer
  pio run -e m5stack-stamps3 --target upload
  
  # Start serial monitor
  pio run -e m5stack-stamps3 --target monitor
  ```

#### 4. Build Cartridge Compiler (`pc8_compile`)
Compile the native desktop utility that converts `.p8` cartridges into the binary `.pc8c` format.
* **Using CLI**:
  ```bash
  # Compile for your native desktop environment
  pio run -e native_tool
  ```
  Once compiled, the executable binary will be generated. On Windows, locate the executable and move it to `tools/pc8_compile.exe` for convenience.

---

## 🛠️ How to Compile Cartridges (`.p8.png` ➔ `.pc8c`)
ZPT8 runs highly optimized `.pc8c` format cartridges from the SD card.
Follow these steps to convert standard `.p8.png` cartridges:

### Prerequisites
* Python 3.x installed.
* `Pillow` image library installed:
  ```bash
  pip install Pillow
  ```

### Step 1: Decode `.p8.png` to `.p8` Text
Run the decoder tool `p28.py` to extract the Lua code and graphics into a plain `.p8` cartridge text file:
```bash
python p28.py <input_cart.p8.png> <output_cart.p8>
```
* **Example**:
  ```bash
  python p28.py jelpi.p8.png jelpi.p8
  ```

### Step 2: Compile `.p8` to `.pc8c` Binary
Use the compiled `pc8_compile` executable to optimize and pack the code/ROM:

#### Command syntax:
```bash
tools/pc8_compile.exe <mode> <input_cart.p8> <output_cart.pc8c>
```
* `<mode>`: Set to `game` for game cartridges, or `bios` for system bios cartridges.

#### Example (Game):
```bash
tools/pc8_compile.exe game jelpi.p8 jelpi.pc8c
```
#### Example (BIOS):
```bash
tools/pc8_compile.exe bios bios.p8 bios.pc8c
```

Copy the generated `.pc8c` files onto the root directory of your microSD card.

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

## 🔧 Troubleshooting

#### Q. A cartridge crashes with an error or triggers a sudden device reset.
A. Because the ESP32-S3 operates under highly constrained memory limits (320KB RAM), complex or resource-heavy cartridges might run out of memory. If you encounter a cartridge that crashes or resets the device, please let us know by opening a GitHub Issue (kindly and gently)! We appreciate your support in making ZPT8 better.

---

## 📜 License & Acknowledgments

* **Zepto-8 Core**: Copyright © 2016–2024 Sam Hocevar (Do What the Fuck You Want to Public License - WTFPL).
* **z8lua Extension**: Customized Lua 5.2 Embedded Subsystem.
* **LodePNG**: Copyright © 2005–2020 Lode Vandevenne (zlib License).
* **Custom Modifications & New Additions**: Copyright © 2026 Layer8. Licensed under the MIT License.
* **Jelpi Sample Asset**: `jelpi.pc8c` is an optimized conversion of "Jelpi Adventures", an official demo cartridge originally created by Lexaloffle Games, provided purely for hardware and performance verification purposes.
