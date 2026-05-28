//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016–2024 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

#include "zepto8.h"
#include "pico8/cart.h"
#include "pico8/pico8.h"
#include "compat/lol_compat.h"

#include <unordered_map> // std::unordered_map
#include <cstring> // std::memchr
#include <regex>   // std::regex
#include <stack>   // std::stack
#include <vector>  // std::vector
#include <array>   // std::array

#if 0
#define TRACE(...) lol::msg::info(__VA_ARGS__)
#else
#define TRACE(...) (void)0
#endif

namespace z8::pico8
{

static std::string pxa_decompress(uint8_t const *input);
static std::string legacy_decompress(uint8_t const *input);
static std::vector<uint8_t> pxa_compress(std::string const &input, bool fast = false);
static std::vector<uint8_t> legacy_compress(std::string const &input);

// Move to front structure
struct move_to_front
{
    move_to_front()
    {
        reset();
    }

    void reset()
    {
        for (int n = 0; n < 256; ++n)
            state[n] = uint8_t(n);
    }

    // Get the nth byte and move it to front
    uint8_t get(int n)
    {
        std::rotate(state.begin(), state.begin() + n, state.begin() + n + 1);
        return state.front();
    }

    // Find index of a given byte in the structure
    int find(uint8_t ch)
    {
        auto val = std::find(state.begin(), state.end(), ch);
        return int(std::distance(state.begin(), val));
    }

    // Push a character and return its previous index, allowing the caller to compute the cost
    // of the operation. This operation can be undone by pop_op().
    int push_op(uint8_t ch)
    {
        int n = find(ch);
        get(n);
        ops.push(uint8_t(n));
        return n;
    }

    // Undo an push_op() operation
    void pop_op()
    {
        std::rotate(state.begin(), state.begin() + 1, state.begin() + ops.top() + 1);
        ops.pop();
    }

private:
    std::array<uint8_t, 256> state;
    std::stack<uint8_t> ops;
};

std::string code::decompress(uint8_t const *input)
{
    if (input[0] == '\0' && input[1] == 'p' && input[2] == 'x' && input[3] == 'a')
        return pxa_decompress(input);

    if (input[0] == ':' && input[1] == 'c' && input[2] == ':' && input[3] == '\0')
        return legacy_decompress(input);

    auto end = (uint8_t const *)std::memchr(input, '\0', sizeof(pico8::memory::code));
    auto len = end ? size_t(end - input) : sizeof(pico8::memory::code);
    return std::string((char const *)input, len);
}

std::vector<uint8_t> code::compress(std::string const &input,
                                    format fmt /* = format::pxa */)
{
    switch (fmt)
    {
        case format::old: return legacy_compress(input);
        case format::pxa: return pxa_compress(input);
        case format::pxa_fast: return pxa_compress(input, true);
        case format::best:
        default:
        {
            auto ret = legacy_compress(input);
            auto b = pxa_compress(input);
            if (b.size() < ret.size())
                ret = b;
            if (ret.size() <= input.length())
                return ret;
            [[fallthrough]];
        }
        case format::store:
        {
            std::vector<uint8_t> ret(input.begin(), input.end());
            ret.push_back(0);
            return ret;
        }
    }
}

static std::string printable(char ch)
{
    char buf[32];
    if (ch >= 0x20 && ch < 0x7f)
        std::snprintf(buf, sizeof(buf), "$%d '%c'", uint8_t(ch), ch);
    else
        std::snprintf(buf, sizeof(buf), "$%d", uint8_t(ch));
    return std::string(buf);
}

static uint8_t const *compress_lut = nullptr;
static char const *decompress_lut = "\n 0123456789abcdefghijklmnopqrstuvwxyz!#%(){}[]<>+=/*:;.,~_";

static std::string pxa_decompress(uint8_t const *input)
{
    size_t length = input[4] * 256 + input[5];
    size_t compressed = input[6] * 256 + input[7];

    size_t pos = size_t(8) * 8; // stream position in bits
    auto get_bits = [&](size_t count) -> uint32_t
    {
        uint32_t n = 0;
        for (size_t i = 0; i < count && pos < compressed * 8; ++i, ++pos)
            n |= ((input[pos >> 3] >> (pos & 0x7)) & 0x1) << i;
        return n;
    };

    move_to_front mtf;
    std::string ret;

    TRACE("# Size: %d (%04x)\n", int(compressed), int(compressed));

    while (ret.size() < length && pos < compressed * 8)
    {
        auto oldpos = pos; (void)oldpos;

        if (get_bits(1))
        {
            int nbits = 4;
            while (get_bits(1))
                ++nbits;
            int n = get_bits(nbits) + (1 << nbits) - 16;
            uint8_t ch = mtf.get(n);
            if (!ch)
                break;
            TRACE("%04x [%d] %s\n", int(ret.size()), int(pos-oldpos), printable(ch).c_str());
            ret.push_back(char(ch));
        }
        else
        {
            int nbits = get_bits(1) ? get_bits(1) ? 5 : 10 : 15;
            int offset = get_bits(nbits) + 1;

            if (nbits == 10 && offset == 1)
            {
                uint8_t ch = get_bits(8);
                while (ch)
                {
                    ret.push_back(char(ch));
                    ch = get_bits(8);
                }
                TRACE("%04x [%d] #%d\n", int(ret.size()), int(pos-oldpos), int(pos-oldpos-21) / 8);
            }
            else
            {
                int n, len = 3;
                do
                    len += (n = get_bits(3));
                while (n == 7);

                TRACE("%04x [%d] %d@-%d\n", int(ret.size()), int(pos-oldpos), len, offset);
                for (int i = 0; i < len; ++i)
                    ret.push_back(ret[ret.size() - offset]);
            }
        }
    }

    return ret;
}

static std::string legacy_decompress(uint8_t const *input)
{
    std::string ret;

    // Expected data length (including trailing zero)
    size_t length = input[4] * 256 + input[5];

    ret.resize(0);
    for (size_t i = 8; i < sizeof(code_t) && ret.length() < length; ++i)
    {
        if (input[i] >= 0x3c)
        {
            size_t a = (input[i] - 0x3c) * 16 + (input[i + 1] & 0xf);
            size_t b = input[i + 1] / 16 + 2;
            if (ret.length() >= a)
                while (b--)
                    ret += ret[ret.length() - a];
            ++i;
        }
        else
        {
            ret += input[i] ? decompress_lut[input[i] - 1]
                            : input[++i];
        }
    }

    if (length != ret.length())
        lol::msg::warn("expected %d code bytes, got %d\n", int(length), int(ret.length()));

    // Remove possible trailing zeroes
    ret.resize(strlen(ret.c_str()));

    // Some old PNG carts have a “if(_update60)_update…” code snippet added by PICO-8 for backwards
    // compatibility. But some buggy versions apparently miss a carriage return or space, leading
    // to syntax errors. Remove it.
    static std::regex junk("if(_update60)_update=function()_update60([)_update_buttons(]*)_update60()end$");
    return std::regex_replace(ret, junk, "");
}

// Stub out suffix-array based PXA compress, as writing it on target device is rarely needed.
// Under standard situations, cartridges are read-only.
static std::vector<uint8_t> pxa_compress(std::string const& input, bool fast)
{
    (void)fast;
    // Fallback: use legacy compress or uncompressed store
    return legacy_compress(input);
}

static std::vector<uint8_t> legacy_compress(std::string const &input)
{
    std::vector<uint8_t> ret;

    ret.insert(ret.end(),
    {
        ':', 'c', ':', '\0',
        (uint8_t)(input.length() >> 8), (uint8_t)input.length(),
        0, 0
    });

    // Ensure the compression LUT is initialised
    if (!compress_lut)
    {
        static uint8_t tmp[256] = { 0 };
        for (int i = 0; i < 0x3b; ++i)
            tmp[(uint8_t)decompress_lut[i]] = i + 1;
        compress_lut = tmp;
    }

    for (int i = 0; i < (int)input.length(); ++i)
    {
        // Look behind for possible patterns
        int best_j = 0, best_len = 0;
        for (int j = std::max(i - 3135, 0); j < i; ++j)
        {
            int k = 0, end = std::min((int)input.length() - j, 17);
            end = std::min(end, i - j);

            while (k < end && input[j + k] == input[i + k])
                ++k;

            if (k >= best_len)
            {
                best_j = j;
                best_len = k;
            }
        }

        uint8_t byte = (uint8_t)input[i];

        if (compress_lut[byte] && best_len <= 2)
        {
            ret.push_back(compress_lut[byte]);
        }
        else if (best_len >= 2)
        {
            uint8_t a = 0x3c + (i - best_j) / 16;
            uint8_t b = ((i - best_j) & 0xf) + (best_len - 2) * 16;
            ret.insert(ret.end(), { a, b });
            i += best_len - 1;
        }
        else
        {
            ret.insert(ret.end(), { '\0', byte });
        }
    }

    return ret;
}

} // namespace z8::pico8
