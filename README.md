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
ZPT8 runs highly optimized .pc8c format cartridges from the SD card.
The compiler tool chain (including python scripts and binaries to convert .p8.png to .pc8c) is currently being prepared and will be released on GitHub soon!

For now, please enjoy the pre-installed system and default verification games (jelpi).

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
