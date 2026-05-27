# ZPT8 (Zepto-8 Portable for M5Cardputer)

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

## 🛠️ How to Compile Cartridges (`.p8.png` ➔ `.pc8c`)

ZPT8 includes a powerful automation tool `png2pc8c.py` that streamlines the process of shrinking your original PICO-8 PNG cartridges and packing them into the optimized `.pc8c` format using `shrinko8` and `pc8_compile`.

### Prerequisites
1. **Python 3.x** installed on your host system.
2. **shrinko8**: The popular PICO-8 code optimizer (`shrinko8.py` or `shrinko8.exe`).
3. **pc8_compile**: The native compiler tool for your binary layout.

Put either `shrinko8` or `pc8_compile` inside a `tools/` folder under your script directory, or keep them in the same folder as `png2pc8c.py`. The script will automatically auto-detect them.

### Usage
Run the script from your terminal, passing the target `.p8.png` cartridge file as the argument:

```bash
python png2pc8c.py <input.p8.png> [output.pc8c]

# Example: Converted to 31991.pc8c automatically
python png2pc8c.py 31991.p8.png
```

### What the Script Does Under the Hood:
1. **Minification**: Invokes `shrinko8` to safely strip comments, eliminate whitespace, and compress the Lua code to its minimal textual token layout.
2. **Binary Synthesis**: Pipes the minified source into `pc8_compile` to wrap graphic banks, sound/music definitions, and code segments into the high-speed structural `.pc8c` output file.
3. **Clean Up**: Safely purges temporary script components from the workspace once generation completes successfully.

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

## 🚀 Source Build Guide (Alternative)

If you wish to compile the firmware from scratch, the main project is configured using **PlatformIO**.

### 1. Production `platformio.ini` Example
```ini
[env:m5stack-stamps3]
platform = espressif32
board = m5stack-stamps3
framework = arduino
monitor_speed = 115200
build_flags = 
    -Os
    -DCORE_DEBUG_LEVEL=0
```

### 2. Deployment Shortcuts
Disconnect any existing serial connections, then invoke the following PlatformIO keybinds:
* **Build & Flash Binary**: `Ctrl + U` (or `pio run --target upload`)
* **Launch Device Diagnostics Monitor**: `Ctrl + Alt + M` (or `pio device monitor`)

---

## 🔧 Troubleshooting

#### Q. Cartridge loads but the screen stays frozen or black.
A. Verify your pipeline routing inside `main.cpp`'s `loop()`. If a cartridge flips the system away from the optimal fast-path rendering matrix (`g_vm->render_fast() == false`), ensure the fallback pixel array is properly copied, byte-swapped to Big-Endian RGB565, and pushed via `g_hal.pushScreenBuffer()`.

#### Q. Real-time loop triggers `*** BIOS LUA ERROR 4: not enough memory`.
A. Double check `src/pico8/vm.cpp`. Ensure the `vm::vm()` constructor explicitly bypasses standard Lua memory configurations by invoking `lua_newstate(baremetal_lua_alloc, nullptr)` and configuring aggressive GC steps before running core initialization routines or bindings.

---

## 📜 License & Acknowledgments

* **ZPT8 Engine & Hardware Abstraction Layer**: MIT License.
* **Zepto-8 Core**: Copyright © 2016–2024 Sam Hocevar (Do What the Fuck You Want to Public License - WTFPL).
* **z8lua Extension**: Customized Lua 5.2 Embedded Subsystem.
* **Jelpi Sample Asset**: `jelpi.pc8c` is an optimized conversion of "Jelpi Adventures", an official demo cartridge originally created by Lexaloffle Games, provided purely for hardware and performance verification purposes.
```
