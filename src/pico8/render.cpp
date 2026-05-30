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
// Adapted for Cardputer/ESP32 by PC8 project.
// Changes:
//   - Replaced #include <lol/vector> with #include "compat/lol_compat.h"
//   - Removed #if HAVE_CONFIG_H block

#include "compat/lol_compat.h" // lol::u8vec4

#include "pico8/vm.h"
#include "pico8/pico8.h"

namespace z8::pico8
{

void vm::private_end_render()
{
    if (m_in_pause) return;

    // Double buffering disabled: do not copy to m_front_buffer
    m_front_draw_state = m_ram.draw_state;
    m_front_hw_state = m_ram.hw_state;
}

void vm::render(lol::u8vec4 *screen) const
{
    // Cannot use a 256-value LUT because data access will be
    // very random due to rotation, flip, stretch etc.
    lol::u8vec4 lut[128 + 16];
    for (int c = 0; c < 16; ++c)
    {
        lut[c] = palette::get8(c);
        lut[128 + c] = palette::get8(16 + c);
    }

    // Multiscreen disabled for memory savings; always render single screen
    for (int y = 0; y < 128; ++y)
    {
        for (int x = 0; x < 128; ++x)
            *screen++ = lut[pixel(x, y, get_front_screen())];
    }
}


// Hardware pixel accessor
uint8_t vm::pixel(int x, int y, u4mat2<128, 128> const& screen) const
{
    // TODO: cache all state
    auto &draw_state = m_front_draw_state;
    auto &hw_state = m_front_hw_state;

    // Get screen mode
    uint8_t const& mode = draw_state.screen_mode;

    // Apply screen mode (rotation, mirror, flip…)
    if ((mode & 0xbc) == 0x84)
    {
        // Rotation modes (0x84 to 0x87)
        if (mode & 1)
            std::swap(x, y);
        x = mode & 2 ? 127 - x : x;
        y = ((mode + 1) & 2) ? 127 - y : y;
    }
    else
    {
        // Other modes
        x = (mode & 0xbd) == 0x05 ? std::min(x, 127 - x) // mirror
            : (mode & 0xbd) == 0x01 ? x / 2                // stretch
            : (mode & 0xbd) == 0x81 ? 127 - x : x;         // flip
        y = (mode & 0xbe) == 0x06 ? std::min(y, 127 - y) // mirror
            : (mode & 0xbe) == 0x02 ? y / 2                // stretch
            : (mode & 0xbe) == 0x82 ? 127 - y : y;         // flip
    }

    int c = screen.get(x, y);

    // Apply raster mode
    if (hw_state.raster.mode == 0x10)
    {
        // Raster mode: alternate palette
        if (hw_state.raster.bits[y])
            return hw_state.raster.palette[c];
    }
    else if ((hw_state.raster.mode & 0x30) == 0x30)
    {
        // Raster mode: gradient
        if ((hw_state.raster.mode & 0x0f) == c)
        {
            int c2 = (y / 8 + (hw_state.raster.bits[y] ? 1 : 0)) % 16;
            return hw_state.raster.palette[c2];
        }
    }

    // Apply screen palette
    return draw_state.screen_palette[c];
}

int vm::get_ansi_color(uint8_t c) const
{
    static int const ansi_palette[] =
    {
         16, // 000000 → 000000
         17, // 1d2b53 → 00005f
         89, // 7e2553 → 87005f
         29, // 008751 → 00875f
        131, // ab5236 → ab5236
        240, // 5f574f → 5f5f5f
        251, // c2c3c7 → c6c6c6
        230, // fff1e8 → ffffdf
        197, // ff004d → ff005f
        214, // ffa300 → ffaf00
        220, // ffec27 → ffdf00
         47, // 00e436 → 00ff5f
         39, // 29adff → 00afff
        103, // 83769c → 8787af
        211, // ff77a8 → f787af
        223, // ffccaa → ffdfaf
    };

    // FIXME: support the extended palette!
    return ansi_palette[m_front_draw_state.screen_palette[c & 0xf] & 0xf];
}

bool vm::render_fast(uint16_t *dest) const
{
    if (m_in_pause) return false;

    auto &draw_state = m_front_draw_state;
    auto &hw_state = m_front_hw_state;

    // Build the RGB565 look-up table for all 128+16 palette colors
    // Pre-calculate shifted and byte-swapped RGB565 values
    uint16_t palette_rgb565[128 + 16];
    for (int c = 0; c < 16; ++c)
    {
        lol::u8vec4 col = palette::get8(c);
        uint16_t rgb = ((col.r >> 3) << 11) | ((col.g >> 2) << 5) | (col.b >> 3);
        palette_rgb565[c] = (rgb >> 8) | (rgb << 8);
        lol::u8vec4 col2 = palette::get8(16 + c);
        uint16_t rgb2 = ((col2.r >> 3) << 11) | ((col2.g >> 2) << 5) | (col2.b >> 3);
        palette_rgb565[128 + c] = (rgb2 >> 8) | (rgb2 << 8);
    }

    uint8_t const& mode = draw_state.screen_mode;
    uint8_t const raster_mode = hw_state.raster.mode;
    bool has_raster = (raster_mode == 0x10) || ((raster_mode & 0x30) == 0x30);

    // Fast path: standard mode, no raster (multiscreen always disabled)
    bool fast_mode = (mode == 0 && !has_raster);

    if (fast_mode)
    {
        // Build screen palette LUT (16 entries)
        uint16_t screen_pal[16];
        for (int i = 0; i < 16; ++i)
            screen_pal[i] = palette_rgb565[draw_state.screen_palette[i]];

        const uint8_t *src = &get_front_screen().data[0][0];
        for (int i = 0; i < 8192; ++i)
        {
            uint8_t val = src[i];
            *dest++ = screen_pal[val & 0x0f];
            *dest++ = screen_pal[val >> 4];
        }
    }
    else
    {
        // Slow path: handles rotation, mirror, stretch, flip, raster, multi-screen
        // Writes directly to uint16_t* dest (no intermediate RGBA buffer)
        auto get_pixel_rgb565 = [&](int x, int y, const u4mat2<128,128>& screen) {
            // Apply screen mode transformations
            int tx = x, ty = y;
            if ((mode & 0xbc) == 0x84)
            {
                if (mode & 1) std::swap(tx, ty);
                tx = mode & 2 ? 127 - tx : tx;
                ty = ((mode + 1) & 2) ? 127 - ty : ty;
            }
            else
            {
                tx = (mode & 0xbd) == 0x05 ? std::min(tx, 127 - tx)
                    : (mode & 0xbd) == 0x01 ? tx / 2
                    : (mode & 0xbd) == 0x81 ? 127 - tx : tx;
                ty = (mode & 0xbe) == 0x06 ? std::min(ty, 127 - ty)
                    : (mode & 0xbe) == 0x02 ? ty / 2
                    : (mode & 0xbe) == 0x82 ? 127 - ty : ty;
            }

            int c = screen.get(tx, ty);

            // Apply raster mode
            if (raster_mode == 0x10)
            {
                if (hw_state.raster.bits[ty])
                    c = hw_state.raster.palette[c];
            }
            else if ((raster_mode & 0x30) == 0x30)
            {
                if ((raster_mode & 0x0f) == c)
                {
                    int c2 = (ty / 8 + (hw_state.raster.bits[ty] ? 1 : 0)) % 16;
                    c = hw_state.raster.palette[c2];
                }
            }

            // Apply screen palette
            c = draw_state.screen_palette[c];
            return palette_rgb565[c];
        };

        // Single screen only (multiscreen disabled for memory savings)
        for (int y = 0; y < 128; ++y)
        {
            for (int x = 0; x < 128; ++x)
                *dest++ = get_pixel_rgb565(x, y, get_front_screen());
        }
    }

    return true;
}

} // namespace z8::pico8
