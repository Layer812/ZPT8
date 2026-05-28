// pico8/memory.h — PICO-8 memory layout
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: replaced lol::u8vec2/i16vec2 with lol_compat versions

#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>

#include "zepto8.h"  // u4mat2, lol types

namespace z8::pico8
{

// ──────────────────────────────────────────────────────────────
// note_t — one note in an SFX
// ──────────────────────────────────────────────────────────────
struct note_t
{
    uint16_t key        : 6;
    uint16_t instrument : 3;
    uint16_t volume     : 3;
    uint16_t effect     : 3;
    uint16_t custom     : 1;
};

// ──────────────────────────────────────────────────────────────
// sfx_t — one SFX entry (68 bytes)
// ──────────────────────────────────────────────────────────────
struct sfx_t
{
    note_t notes[32];

    uint8_t filters;
    uint8_t speed;
    uint8_t loop_start;
    uint8_t loop_end;
};

// ──────────────────────────────────────────────────────────────
// song_t — one music pattern
// ──────────────────────────────────────────────────────────────
struct song_t
{
    union
    {
        struct
        {
            uint8_t sfx0  : 7;
            uint8_t start : 1;
            uint8_t sfx1  : 7;
            uint8_t loop  : 1;
            uint8_t sfx2  : 7;
            uint8_t stop  : 1;
            uint8_t sfx3  : 7;
            uint8_t mode  : 1;
        };
        uint8_t data[4];
    };

    uint8_t sfx(int n) const;
};

// ──────────────────────────────────────────────────────────────
// code section
// ──────────────────────────────────────────────────────────────
struct code_t : std::array<uint8_t, 0x3d00>
{
};

// ──────────────────────────────────────────────────────────────
// custom_font_t
// ──────────────────────────────────────────────────────────────
struct custom_font_t
{
    uint8_t width;
    uint8_t extended_width;
    uint8_t height;
    lol::u8vec2 offset;       // draw offset x/y
    uint8_t size_adjustments;
    uint8_t padding[2];
    uint8_t glyphs[255][8];
};

// ──────────────────────────────────────────────────────────────
// draw_state_t  (0x5f00–0x5f40)
// ──────────────────────────────────────────────────────────────
struct draw_state_t
{
    uint8_t draw_palette[16];
    uint8_t screen_palette[16];

    struct { uint8_t x1, y1, x2, y2; } clip;

    uint8_t print_start_x;
    uint8_t pen;
    lol::u8vec2 cursor;
    lol::i16vec2 camera;

    uint8_t screen_mode;

    struct
    {
        uint8_t enabled      : 1;
        uint8_t buttons      : 1;
        uint8_t locked       : 1;
        uint8_t undocumented : 5;
    } mouse_flags;

    struct
    {
        uint8_t palette       : 1;
        uint8_t undocumented1 : 4;
        uint8_t fillp         : 1;
        uint8_t undocumented2 : 2;
    } preserve_flags;

    uint8_t pause_music;
    uint8_t pause_flag;

    uint8_t fillp[2], fillp_trans, fillp_flag;
    uint8_t polyline_flag;

    struct
    {
        uint8_t multi_screen    : 1;
        uint8_t undocumented1   : 1;
        uint8_t no_newlines     : 1;
        uint8_t sprite_zero     : 1;
        uint8_t undocumented2   : 1;
        uint8_t dampen_PCM      : 1;
        uint8_t no_printscroll  : 1;
        uint8_t char_wrap       : 1;
    } misc_features;

    uint8_t disable_editor_reload;

    struct
    {
        lol::u8vec2 mask;
        lol::u8vec2 offset;
    } tline;

    lol::i16vec2 polyline;
};

// ──────────────────────────────────────────────────────────────
// print_state_t
// ──────────────────────────────────────────────────────────────
struct print_state_t
{
    uint8_t active  : 1;
    uint8_t padding : 1;
    uint8_t wide    : 1;
    uint8_t tall    : 1;
    uint8_t solid   : 1;
    uint8_t invert  : 1;
    uint8_t dotty   : 1;
    uint8_t custom  : 1;

    uint8_t char_w   : 4;
    uint8_t char_h   : 4;
    uint8_t char_w2  : 4;
    uint8_t tab_w    : 4;
    uint8_t offset_x : 4;
    uint8_t offset_y : 4;
};

// ──────────────────────────────────────────────────────────────
// hw_state_t  (0x5f40–0x5f80)
// ──────────────────────────────────────────────────────────────
struct hw_state_t
{
    uint8_t half_rate, reverb, distort, lowpass;
    struct { uint32_t a, b; } prng;
    uint8_t btn_state[8];
    uint8_t mapping_spritesheet, mapping_screen, mapping_map, mapping_map_width;
    print_state_t print_state;
    uint8_t btnp_delay, btnp_rate;
    uint8_t bit_mask;

#pragma pack(push,1)
    struct
    {
        uint8_t mode;
        uint8_t palette[16];
        std::bitset<128> bits;
    } raster;
#pragma pack(pop)
};

// ──────────────────────────────────────────────────────────────
// memory — full PICO-8 RAM (65536 bytes)
// ──────────────────────────────────────────────────────────────
struct memory
{
    union
    {
        u4mat2<128,128> gfx;  // 0x0000–0x2000

        struct
        {
            uint8_t padding[0x1000];

            uint8_t map2[0x1000];

            struct
            {
                inline uint8_t &operator[](int n)
                {
                    memory &mem = *(memory*)(b - 0x2000);
                    if (mem.hw_state.mapping_map >= 0x80)
                    {
                        assert(n >= 0 && n < (0x100 - mem.hw_state.mapping_map) << 8);
                        int offset = mem.hw_state.mapping_map << 8;
                        return mem[offset + n];
                    }
                    assert(n >= 0 && n < (int)(sizeof(memory::map) + sizeof(memory::map2)));
                    return b[(n ^ 0x1000) - 0x1000];
                }

                inline uint8_t const &operator[](int n) const
                {
                    memory &mem = *(memory*)(b - 0x2000);
                    if (mem.hw_state.mapping_map >= 0x80)
                    {
                        assert(n >= 0 && n < (0x100 - mem.hw_state.mapping_map) << 8);
                        int offset = mem.hw_state.mapping_map << 8;
                        return mem[offset + n];
                    }
                    assert(n >= 0 && n < (int)(sizeof(memory::map) + sizeof(memory::map2)));
                    return b[(n ^ 0x1000) - 0x1000];
                }

            private:
                uint8_t b[0x1000];
            } map;
        };
    };

    uint8_t gfx_flags[0x100];  // 0x3000
    song_t  song[0x40];        // 0x3100
    sfx_t   sfx[0x40];         // 0x3200

    union
    {
        struct
        {
            inline auto  operator()()       { return reinterpret_cast<code_t &>(*this); }
            inline auto &operator()() const { return reinterpret_cast<code_t const &>(*this); }
        } code;

        struct
        {
            uint8_t         user_data[0x1300];  // 0x4300
            custom_font_t   custom_font;         // 0x5600
        };
    };

    uint8_t     persistent[0x100];   // 0x5e00
    draw_state_t draw_state;          // 0x5f00
    hw_state_t   hw_state;            // 0x5f40
    uint8_t      gpio_pins[0x80];     // 0x5f80
    u4mat2<128,128> screen;           // 0x6000
    // uint8_t      extended_map[4][0x2000]; // 0x8000 (Removed to save 32KB RAM per memory instance)

    inline uint8_t &operator[](int n)
    {
        n &= 0xffff;
        if (n >= 0x8000)
        {
            static uint8_t dummy = 0;
            return dummy;
        }
        assert(n >= 0 && n < (int)sizeof(memory));
        return ((uint8_t *)this)[n];
    }

    inline uint8_t const &operator[](int n) const
    {
        n &= 0xffff;
        if (n >= 0x8000)
        {
            static uint8_t const dummy = 0;
            return dummy;
        }
        assert(n >= 0 && n < (int)sizeof(memory));
        return ((uint8_t const *)this)[n];
    }

    inline u4mat2<128,128> const &get_gfx() const
    {
        return hw_state.mapping_spritesheet == 0x60 ? screen : gfx;
    }

    inline u4mat2<128,128> &get_gfx()
    {
        return const_cast<u4mat2<128,128> &>(std::as_const(*this).get_gfx());
    }

    ~memory() = default;
};

// Static size checks
static_assert(sizeof(sfx_t)  == 68,     "pico8::sfx has incorrect size");
static_assert(sizeof(code_t) == 0x3d00, "pico8::code_t has incorrect size");

#define static_check_section(name, offset, size) \
    static_assert(offsetof(memory, name) == offset, \
                  "pico8::memory::"#name" should have offset "#offset); \
    static_assert((sizeof(memory::name) == size) || (size == -1), \
                  "pico8::memory::"#name" should have size "#size);

static_check_section(gfx,        0x0000, 0x2000);
static_check_section(map2,       0x1000, 0x1000);
static_check_section(map,        0x2000, 0x1000);
static_check_section(gfx_flags,  0x3000,  0x100);
static_check_section(song,       0x3100,  0x100);
static_check_section(sfx,        0x3200, 0x1100);
static_check_section(code,       0x4300,     -1);
static_check_section(user_data,  0x4300, 0x1300);
static_check_section(custom_font,0x5600,  0x800);
static_check_section(persistent, 0x5e00,  0x100);
static_check_section(draw_state, 0x5f00,   0x40);
static_check_section(hw_state,   0x5f40,   0x40);
static_check_section(gpio_pins,  0x5f80,   0x80);
static_check_section(screen,     0x6000, 0x2000);
#undef static_check_section

static_assert(sizeof(memory) == 0x8000, "pico8::memory should have size 0x8000");

} // namespace z8::pico8
