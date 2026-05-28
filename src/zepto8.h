// zepto8.h — Core types for Cardputer PICO-8 player
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: removed lolengine dependency, uses lol_compat.h

#pragma once

// Pull in compatibility types (replaces <lol/vector>, <lol/msg>, etc.)
#include "compat/lol_compat.h"

#include <any>
#include <string>
#include <tuple>
#include <functional>
#include <cassert>
#include <cstddef>
#include <memory>

// ──────────────────────────────────────────────────────────────
// Re-export frequently used lol types into the global scope
// so that code using "lol::u8vec4" etc. still compiles.
// ──────────────────────────────────────────────────────────────
// (Already in namespace lol — callers can use lol:: prefix.)

namespace z8
{

namespace pico8
{
    class bios; // forward declaration
}

// ──────────────────────────────────────────────────────────────
// 4-bit 2D array  (128×128 nibbles → 8 192 bytes)
// ──────────────────────────────────────────────────────────────
template<int W, int H>
class u4mat2
{
public:
    inline uint8_t safe_get(int x, int y) const
    {
        return (x >= 0 && y >= 0 && x < W && y < H) ? get(x, y) : 0;
    }

    inline void safe_set(int x, int y, uint8_t c)
    {
        if (x >= 0 && y >= 0 && x < W && y < H)
            set(x, y, c);
    }

    inline uint8_t get(int x, int y) const
    {
        assert(x >= 0 && x < W && y >= 0 && y < H);
        uint8_t const p = data[y][x / 2];
        return x & 1 ? p >> 4 : p & 0xf;
    }

    inline void set(int x, int y, uint8_t c)
    {
        assert(x >= 0 && x < W && y >= 0 && y < H);
        uint8_t &p = data[y][x / 2];
        p = (p & (x & 1 ? 0x0f : 0xf0)) | (x & 1 ? c << 4 : c & 0x0f);
    }

    uint8_t data[H][W / 2];
};

// ──────────────────────────────────────────────────────────────
// Generic VM interface
// ──────────────────────────────────────────────────────────────
class vm_base
{
public:
    vm_base() = default;
    virtual ~vm_base() = default;

    virtual void load(std::string const &name) = 0;
    virtual void run()  = 0;
    virtual void reset() = 0;
    virtual bool step(float seconds) = 0;
    virtual float getTime() = 0;

    // Rendering
    virtual void render(lol::u8vec4 *screen) const = 0;
    virtual u4mat2<128,128> const &get_front_screen() const = 0;
    virtual lol::ivec2 get_screen_resolution() const = 0;

    virtual int get_ansi_color(uint8_t c) const = 0;

    // Code
    virtual std::string const &get_code() const = 0;
    virtual std::string &get_mutable_code() = 0;

    // Audio streaming
    virtual void get_audio(void *buffer, size_t frames) = 0;

    // IO
    virtual void button(int player, int index, int state) = 0;
    virtual void mouse(lol::ivec2 coords, lol::ivec2 relative, int buttons, int scroll) = 0;
    virtual void text(char ch) = 0;
    virtual void sixaxis(lol::vec3 angle) = 0;
    virtual void axis(int player, float valueX, float valueY) = 0;

    // Memory
    virtual std::tuple<uint8_t *, size_t> ram() = 0;
    virtual std::tuple<uint8_t *, size_t> rom() = 0;

    virtual void request_exit() = 0;
    virtual bool is_running() = 0;

    virtual int  get_filter_index() = 0;
    virtual int  get_fullscreen()   = 0;
    virtual void set_fullscreen(int value, bool save = true, bool runCallback = true) = 0;
    virtual void set_config_dir(std::string new_path_config_dir) = 0;
    virtual void use_default_carts_dir() = 0;

    // Callbacks (stubs — not used on Cardputer)
    std::function<int(int)>          setfilter_callback;
    std::function<std::string(int)>  getfiltername_callback;
    std::function<void(int)>         setfullscreen_callback;
    std::function<std::string()>     getfullscreen_callback;
    std::function<void(bool)>        pointerLock_callback;

    void registerSetFilterCallback(std::function<int(int)> f)              { setfilter_callback     = std::move(f); }
    void registerGetFilterNameCallback(std::function<std::string(int)> f)  { getfiltername_callback = std::move(f); }
    void registerSetFullscreenCallback(std::function<void(int)> f)         { setfullscreen_callback = std::move(f); }
    void registerGetFullscreenCallback(std::function<std::string()> f)     { getfullscreen_callback = std::move(f); }
    void registerPointerLockCallback(std::function<void(bool)> f)          { pointerLock_callback   = std::move(f); }

    // Extension commands / stats
    virtual void add_extcmd(std::string const &, std::function<void(std::string const &)>) = 0;
    virtual void add_stat(int16_t, std::function<std::any()>) = 0;

protected:
    std::unique_ptr<pico8::bios> m_bios;
};

enum
{
    SCREEN_WIDTH  = 128,
    SCREEN_HEIGHT = 128,
};

} // namespace z8
