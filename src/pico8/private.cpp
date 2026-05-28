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

#include "pico8/pico8.h"
#include "pico8/vm.h"
#include "compat/lol_compat.h"

#include <string>
#include <vector>
#include <cstring>
#include <map>

#if defined(ARDUINO)
#include <Arduino.h>
#include <SD.h>
#else
#include <filesystem>
#endif

namespace z8::pico8
{

std::string_view charset::to_utf8[256];
std::u32string_view charset::to_utf32[256];

static uint8_t multibyte_start[256];
static std::map<std::string, uint8_t> to_pico8;
std::regex charset::utf8_regex = charset::static_init();

// Simple UTF-8 to UTF-32 decoder to avoid std::codecvt dependency
static std::vector<char32_t> decode_utf8(const char* s, size_t size)
{
    std::vector<char32_t> res;
    size_t i = 0;
    while (i < size)
    {
        uint32_t codepoint = 0;
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80)
        {
            codepoint = c;
            i += 1;
        }
        else if ((c & 0xe0) == 0xc0)
        {
            if (i + 1 < size) {
                codepoint = ((c & 0x1f) << 6) | ((uint8_t)s[i+1] & 0x3f);
                i += 2;
            } else { i += 1; }
        }
        else if ((c & 0xf0) == 0xe0)
        {
            if (i + 2 < size) {
                codepoint = ((c & 0x0f) << 12) | (((uint8_t)s[i+1] & 0x3f) << 6) | ((uint8_t)s[i+2] & 0x3f);
                i += 3;
            } else { i += 1; }
        }
        else if ((c & 0xf8) == 0xf0)
        {
            if (i + 3 < size) {
                codepoint = ((c & 0x07) << 18) | (((uint8_t)s[i+1] & 0x3f) << 12) | (((uint8_t)s[i+2] & 0x3f) << 6) | ((uint8_t)s[i+3] & 0x3f);
                i += 4;
            } else { i += 1; }
        }
        else
        {
            i += 1;
            continue;
        }
        res.push_back(codepoint);
    }
    return res;
}

std::regex charset::static_init()
{
    static char const utf8_chars[] =
        "\0¹²³⁴⁵⁶⁷⁸\t\nᵇᶜ\rᵉᶠ▮■□⁙⁘‖◀▶「」¥•、。゛゜"
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNO"
        "PQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~○"
        "█▒🐱⬇️░✽●♥☉웃⌂⬅️😐♪🅾️◆…➡️★⧗⬆️ˇ∧❎▤▥あいうえおか"
        "きくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよ"
        "らりるれろわをんっゃゅょアイウエオカキクケコサシスセソタチツテト"
        "ナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンッャュョ◜◝";

    auto utf32_chars = decode_utf8(utf8_chars, sizeof(utf8_chars));

    char const *p8 = utf8_chars;
    auto const *p32 = (char32_t const *)utf32_chars.data();
    std::string regex_str("(");
    for (int i = 0; i < 256; ++i)
    {
        size_t len32 = p32[1] == 0xfe0f ? 2 : 1;
        size_t len8 = ((0xe5000000 >> ((*p8 >> 3) & 0x1e)) & 3) + len32 * len32;
        to_utf8[i] = std::string_view(p8, len8);
        to_utf32[i] = std::u32string_view(p32, len32);
        to_pico8[std::string(p8, len8)] = i;

        if (len8 > 1)
        {
            multibyte_start[(uint8_t)*p8] = 1;
            regex_str += std::string(p8, len8) + '|';
        }

        p8 += len8;
        p32 += len32;
    }
    regex_str += ')';
    return std::regex(""); // Return dummy regex to save memory
}

std::string charset::utf8_to_pico8(std::string const &str)
{
    std::string ret;
    ret.reserve(str.size());
    for (size_t i = 0; i < str.size(); )
    {
        uint8_t c = (uint8_t)str[i];
        if (multibyte_start[c])
        {
            bool found = false;
            size_t max_len = std::min<size_t>(6, str.size() - i);
            for (size_t len = max_len; len >= 2; --len)
            {
                std::string sub = str.substr(i, len);
                auto it = to_pico8.find(sub);
                if (it != to_pico8.end())
                {
                    ret += (char)it->second;
                    i += len;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                ret += str[i++];
            }
        }
        else
        {
            ret += str[i++];
        }
    }

    return ret;
}

void charset::utf8_to_pico8_inplace(std::string &str)
{
    size_t write_idx = 0;
    for (size_t i = 0; i < str.size(); )
    {
        uint8_t c = (uint8_t)str[i];
        if (multibyte_start[c])
        {
            bool found = false;
            size_t max_len = std::min<size_t>(6, str.size() - i);
            for (size_t len = max_len; len >= 2; --len)
            {
                std::string sub = str.substr(i, len);
                auto it = to_pico8.find(sub);
                if (it != to_pico8.end())
                {
                    str[write_idx++] = (char)it->second;
                    i += len;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                str[write_idx++] = str[i++];
            }
        }
        else
        {
            str[write_idx++] = str[i++];
        }
    }
    str.resize(write_idx);
}

// C-style version: operates directly on raw char buffer to avoid heap allocation
void charset::utf8_to_pico8_inplace(char* buf, int& len)
{
    int write_idx = 0;
    for (int i = 0; i < len; )
    {
        uint8_t c = (uint8_t)buf[i];
        if (multibyte_start[c])
        {
            bool found = false;
            int max_len = 6;
            if (max_len > len - i) max_len = len - i;
            for (int l = max_len; l >= 2; --l)
            {
                // Build a small fixed buffer to avoid heap allocation
                char sub[7];
                std::memcpy(sub, buf + i, l);
                sub[l] = '\0';
                auto it = to_pico8.find(std::string(sub, l));
                if (it != to_pico8.end())
                {
                    buf[write_idx++] = (char)it->second;
                    i += l;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                buf[write_idx++] = buf[i++];
            }
        }
        else
        {
            buf[write_idx++] = buf[i++];
        }
    }
    len = write_idx;
}

std::string charset::pico8_to_utf8(std::string const &str)
{
    std::string ret;
    for (uint8_t ch : str)
        ret += std::string(to_utf8[ch]);
    return ret;
}

void vm::private_stub(std::string str)
{
    lol::msg::info("z8:stub:%s\n", str.c_str());
}

bool vm::private_is_api(std::string str)
{
    if (api::functions.find(str) != api::functions.end())
        return true;

    if (str.size() == 1 && uint8_t(str[0]) >= 0x80 && uint8_t(str[0]) < 0x80 + 26)
        return true;

    return false;
}

opt<bool> vm::private_cartdata(opt<std::string> str)
{
    if (!str)
        return m_cartdata.size() > 0;

    if (!str->size())
    {
        m_cartdata = "";
        return std::nullopt;
    }

    m_cartdata = *str;
    
    char buf[64];
    std::snprintf(buf, sizeof(buf), "cartdata(\"%s\")", m_cartdata.c_str());
    private_stub(std::string(buf));

    return load_cartdata();
}

std::vector<std::string> vm::private_dir(opt<std::string> target_dir)
{
    std::vector<std::string> files;
    std::string path = get_path_active_dir();
    if (target_dir) path = path + *target_dir;

#if defined(ARDUINO)
    File dir = SD.open(path.c_str());
    if (dir && dir.isDirectory())
    {
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry) break;
            std::string name = entry.name();
            size_t pos = name.find_last_of('/');
            if (pos != std::string::npos) name = name.substr(pos + 1);

            if (entry.isDirectory())
                files.push_back(name + "/");
            else
                files.push_back(name);
            entry.close();
        }
    }
    if (dir) dir.close();
#else
    #if defined(_MSC_VER) || __cplusplus >= 201703L
    namespace fs = std::filesystem;
    if (fs::exists(path))
    {
        for (const auto& entry : fs::directory_iterator(path))
        {
            if (entry.is_directory())
                files.push_back(entry.path().filename().string() + "/");
            else
                files.push_back(entry.path().filename().string());
        }
    }
    #endif
#endif

    if (files.size() > 32)
    {
        files.resize(32);
    }
    return files;
}

void vm::private_set_pause(bool pause)
{
    m_in_pause = pause;
}

} // namespace z8::pico8
