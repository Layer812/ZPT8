// pico8/pico8.h — PICO-8 definitions (palette, charset, code utilities)
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: removed lol/vector dependency (uses lol_compat.h via zepto8.h)

#pragma once

#include <map>
#include <unordered_set>
#include <string_view>
#include <regex>
#include <cfloat>

#include "zepto8.h"  // pulls in lol_compat.h

namespace z8::pico8
{

enum
{
    PICO8_VERSION = 25,
};

enum
{
    LABEL_WIDTH  = 128,
    LABEL_HEIGHT = 128,
    LABEL_X      = 16,
    LABEL_Y      = 24,
};

struct api
{
    static std::unordered_set<std::string> functions;
    static std::unordered_set<std::string> keywords;
};

struct charset
{
    static std::string utf8_to_pico8(std::string const &str);
    static void utf8_to_pico8_inplace(std::string &str);
    // C-style version operating directly on raw buffers (avoids heap allocation)
    static void utf8_to_pico8_inplace(char* buf, int& len);
    static std::string pico8_to_utf8(std::string const &str);

    static std::u32string_view to_utf32[256];
    static std::string_view   to_utf8[256];

private:
    static std::regex static_init();
    static std::regex utf8_regex;
};

struct code
{
    enum class format
    {
        best, pxa, pxa_fast, old, store,
    };

    static std::string decompress(uint8_t const *input);
    static std::vector<uint8_t> compress(std::string const &input,
                                         format fmt = format::pxa);

    static int count_tokens(std::string const &s);
    static std::string ast(std::string const &s);
};

struct palette
{
    enum
    {
        black = 0, dark_blue, dark_purple, dark_green,
        brown, dark_gray, light_gray, white,
        red, orange, yellow, green,
        blue, indigo, pink, peach,
    };

    static lol::vec4 get(int n)
    {
        return lol::vec4(get8(n)) / 255.f;  // uses vec4(u8vec4)
    }

    static lol::u8vec4 get8(int n)
    {
        static lol::u8vec4 const pal[] =
        {
            lol::u8vec4(0x00, 0x00, 0x00, 0xff), // black
            lol::u8vec4(0x1d, 0x2b, 0x53, 0xff), // dark_blue
            lol::u8vec4(0x7e, 0x25, 0x53, 0xff), // dark_purple
            lol::u8vec4(0x00, 0x87, 0x51, 0xff), // dark_green
            lol::u8vec4(0xab, 0x52, 0x36, 0xff), // brown
            lol::u8vec4(0x5f, 0x57, 0x4f, 0xff), // dark_gray
            lol::u8vec4(0xc2, 0xc3, 0xc7, 0xff), // light_gray
            lol::u8vec4(0xff, 0xf1, 0xe8, 0xff), // white
            lol::u8vec4(0xff, 0x00, 0x4d, 0xff), // red
            lol::u8vec4(0xff, 0xa3, 0x00, 0xff), // orange
            lol::u8vec4(0xff, 0xec, 0x27, 0xff), // yellow
            lol::u8vec4(0x00, 0xe4, 0x36, 0xff), // green
            lol::u8vec4(0x29, 0xad, 0xff, 0xff), // blue
            lol::u8vec4(0x83, 0x76, 0x9c, 0xff), // indigo
            lol::u8vec4(0xff, 0x77, 0xa8, 0xff), // pink
            lol::u8vec4(0xff, 0xcc, 0xaa, 0xff), // peach

            // Secret palette (indices 16–31)
            lol::u8vec4(0x29, 0x18, 0x14, 0xff),
            lol::u8vec4(0x11, 0x1d, 0x35, 0xff),
            lol::u8vec4(0x42, 0x21, 0x36, 0xff),
            lol::u8vec4(0x12, 0x53, 0x59, 0xff),
            lol::u8vec4(0x74, 0x2f, 0x29, 0xff),
            lol::u8vec4(0x49, 0x33, 0x3b, 0xff),
            lol::u8vec4(0xa2, 0x88, 0x79, 0xff),
            lol::u8vec4(0xf3, 0xef, 0x7d, 0xff),
            lol::u8vec4(0xbe, 0x12, 0x50, 0xff),
            lol::u8vec4(0xff, 0x6c, 0x24, 0xff),
            lol::u8vec4(0xa8, 0xe7, 0x2e, 0xff),
            lol::u8vec4(0x00, 0xb5, 0x43, 0xff),
            lol::u8vec4(0x06, 0x5a, 0xb5, 0xff),
            lol::u8vec4(0x75, 0x46, 0x65, 0xff),
            lol::u8vec4(0xff, 0x6e, 0x59, 0xff),
            lol::u8vec4(0xff, 0x9d, 0x81, 0xff),
        };
        return pal[n & 0x1f];
    }

    static int best(lol::vec4 c, int count = 16)
    {
        int ret = 0;
        float dist = FLT_MAX;
        for (int n = 0; n < count; ++n)
        {
            lol::vec3 delta;
            lol::vec4 pn = get(n);
            delta.x = c.r - pn.r;
            delta.y = c.g - pn.g;
            delta.z = c.b - pn.b;
            float newdist = lol::sqlength(delta);
            if (newdist < dist) { dist = newdist; ret = n; }
        }
        return ret;
    }

    static int best(lol::u8vec4 c, int count = 16)
    {
        return best(lol::vec4(c) / 255.f, count);
    }
};

} // namespace z8::pico8
