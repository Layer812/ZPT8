//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016—2020 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//
// Changes:
//   PC8C format support: precompiled z8lua bytecode
//   Format: "PC8C" + uint32(gfx_size=8192) + gfx[8192] + uint32(bc_size) + bytecode
//   Legacy PC8B format (Lua source text) still supported as fallback.

#include "compat/lol_compat.h"
#include "bios.h"

#if defined(ARDUINO)
#include <M5Cardputer.h>
#include "esp_partition.h"
#endif

namespace z8::pico8
{

bios::bios()
{
    bool loaded = false;

#if defined(ARDUINO)
    // 1. Try to load from memory-mapped partition named "bios"
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bios");

    if (partition != nullptr)
    {
        Serial.printf("Found 'bios' partition at 0x%08X, size=%u. mmap-ing...\n",
                      (unsigned int)partition->address,
                      (unsigned int)partition->size);
        spi_flash_mmap_handle_t map_handle;
        const void *map_ptr = nullptr;
        esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                           SPI_FLASH_MMAP_DATA, &map_ptr, &map_handle);
        if (err == ESP_OK)
        {
            const char* base = (const char*)map_ptr;

            Serial.print("First 8 bytes: ");
            for (int i = 0; i < 8; i++) Serial.printf("%02X ", (uint8_t)base[i]);
            Serial.println();

            // PC8C: precompiled bytecode format
            // New format: magic(4) + name(32) + rom_size(4) + ROM + bc_size(4) + bytecode
            // Old format: magic(4) + rom_size(4) + ROM + bc_size(4) + bytecode
            if (strncmp(base, "PC8C", 4) == 0)
            {
                // New format detection: byte[4] is printable ASCII (first char of filename)
                bool new_fmt = (base[4] >= 0x20 && base[4] < 0x7f);
                uint32_t rom_size, bc_size;
                const uint8_t* rom_ptr;
                const char*    bc_ptr;
                if (new_fmt) {
                    // New: +4=name(32), +36=rom_size, +40=ROM, +40+rom_size=bc_size
                    rom_size = *(const uint32_t*)(base + 36);
                    rom_ptr  = (const uint8_t*)(base + 40);
                    bc_size  = *(const uint32_t*)(base + 40 + rom_size);
                    bc_ptr   = base + 40 + rom_size + 4;
                } else {
                    // Old: +4=rom_size, +8=ROM, +8+rom_size=bc_size
                    rom_size = *(const uint32_t*)(base + 4);
                    rom_ptr  = (const uint8_t*)(base + 8);
                    bc_size  = *(const uint32_t*)(base + 8 + rom_size);
                    bc_ptr   = base + 8 + rom_size + 4;
                }
                m_gfx_ptr    = rom_ptr;
                m_code_ptr   = bc_ptr;
                m_code_len   = bc_size;
                m_is_bytecode = true;
                Serial.printf("PC8C BIOS (%s): rom=%u bc=%u bytes\n",
                              new_fmt ? "new" : "old", rom_size, bc_size);
                loaded = true;
            }
            // PC8B: legacy Lua source text format
            else if (strncmp(base, "PC8B", 4) == 0)
            {
                uint32_t lua_size = *(const uint32_t*)(base + 4);
                m_gfx_ptr    = (const uint8_t*)(base + 8);
                m_code_ptr   = base + 8 + 8192;
                m_code_len   = lua_size;
                m_is_bytecode = false;
                Serial.printf("PC8B BIOS: gfx=8192 lua=%u bytes (source mode)\n",
                              lua_size);
                loaded = true;
            }
            else
            {
                Serial.println("Invalid magic in 'bios' partition.");
            }
        }
        else
        {
            Serial.printf("mmap failed: 0x%x\n", err);
        }
    }
    else
    {
        Serial.println("'bios' partition not found. Falling back to SPIFFS.");
    }
#endif

    if (!loaded)
    {
        // Fallback: load from SPIFFS file
        // Try PC8C first, then PC8B
        const char* filenames[] = { "/spiffs/bios.pc8c", "/spiffs/bios.bin" };

        for (const char* filename : filenames)
        {
#if defined(ARDUINO)
            fs::File f = lol::file::open(filename, FILE_READ);
            if (!f) continue;

            char magic[4];
            if (f.readBytes(magic, 4) != 4) { f.close(); continue; }

            if (strncmp(magic, "PC8C", 4) == 0)
            {
                uint32_t gfx_size = 0, bc_size = 0;
                if (f.readBytes((char*)&gfx_size, 4) != 4) { f.close(); continue; }
                m_fallback_gfx.resize(gfx_size);
                if (f.readBytes((char*)m_fallback_gfx.data(), gfx_size) != (int)gfx_size) { f.close(); continue; }
                if (f.readBytes((char*)&bc_size, 4) != 4) { f.close(); continue; }
                m_fallback_code.resize(bc_size);
                if (f.readBytes((char*)m_fallback_code.data(), bc_size) != (int)bc_size) { f.close(); continue; }
                f.close();

                m_gfx_ptr    = m_fallback_gfx.data();
                m_code_ptr   = m_fallback_code.data();
                m_code_len   = bc_size;
                m_is_bytecode = true;
                Serial.printf("PC8C BIOS from SPIFFS: gfx=%u bc=%u bytes\n", gfx_size, bc_size);
                loaded = true;
                break;
            }
            else if (strncmp(magic, "PC8B", 4) == 0)
            {
                uint32_t lua_size = 0;
                if (f.readBytes((char*)&lua_size, 4) != 4) { f.close(); continue; }
                m_fallback_gfx.resize(8192);
                if (f.readBytes((char*)m_fallback_gfx.data(), 8192) != 8192) { f.close(); continue; }
                m_fallback_code.resize(lua_size);
                if (f.readBytes((char*)m_fallback_code.data(), lua_size) != (int)lua_size) { f.close(); continue; }
                f.close();

                m_gfx_ptr    = m_fallback_gfx.data();
                m_code_ptr   = m_fallback_code.data();
                m_code_len   = lua_size;
                m_is_bytecode = false;
                Serial.printf("PC8B BIOS from SPIFFS: gfx=8192 lua=%u bytes\n", lua_size);
                loaded = true;
                break;
            }
            f.close();
#endif
        }

        if (!loaded)
        {
            lol::msg::error("unable to load BIOS from partition or SPIFFS\n");
#if defined(ARDUINO)
            M5Cardputer.Display.setTextColor(TFT_RED);
            M5Cardputer.Display.drawString("BIOS LOAD FAILED!", 10, 30);
            M5Cardputer.Display.drawString("Flash bios.pc8c to 'bios' partition", 10, 50);
#endif
            while (1) { delay(100); }
        }
    }
}

} // namespace z8::pico8
