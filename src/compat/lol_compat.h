// lol_compat.h — lolengine compatibility layer for Arduino/Cardputer
// Provides replacements for lol::ivec2, lol::u8vec4, lol::msg, lol::file, etc.

#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cassert>

// ──────────────────────────────────────────────────────────────
// Arduino / SD includes (only when building for ESP32)
// ──────────────────────────────────────────────────────────────
#if defined(ARDUINO)
#  include <Arduino.h>
#  include <SD.h>
#  include <FS.h>
#  include <SPIFFS.h>
#endif

// ──────────────────────────────────────────────────────────────
// fix32  — pulled from z8lua
// ──────────────────────────────────────────────────────────────
#include "fix32.h"

namespace lol
{

// ──────────────────────────────────────────────────────────────
// Vector types
// ──────────────────────────────────────────────────────────────

struct u8vec2
{
    uint8_t x, y;
};

struct u8vec4
{
    uint8_t r, g, b, a;
    u8vec4() = default;
    u8vec4(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_)
        : r(r_), g(g_), b(b_), a(a_) {}
    u8vec4 operator/(float f) const
    {
        return u8vec4((uint8_t)(r/f),(uint8_t)(g/f),(uint8_t)(b/f),(uint8_t)(a/f));
    }
};

struct ivec2
{
    int x, y;
    ivec2() = default;
    ivec2(int x_, int y_) : x(x_), y(y_) {}
};

struct i16vec2
{
    int16_t x, y;
};

struct vec2
{
    float x, y;
    vec2() = default;
    vec2(float x_, float y_) : x(x_), y(y_) {}
    explicit vec2(ivec2 v) : x((float)v.x), y((float)v.y) {}
};

struct vec3
{
    float x, y, z;
    vec3() = default;
    vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    vec3 rgb() const { return *this; }
};

struct vec4
{
    float r, g, b, a;
    vec4() = default;
    vec4(float r_, float g_, float b_, float a_) : r(r_), g(g_), b(b_), a(a_) {}
    explicit vec4(u8vec4 v) : r(v.r/255.f), g(v.g/255.f), b(v.b/255.f), a(v.a/255.f) {}
    vec3 rgb() const { return vec3(r, g, b); }
    vec4 operator/(float f) const
    {
        return vec4(r/f, g/f, b/f, a/f);
    }
};

// ──────────────────────────────────────────────────────────────
// Math helpers
// ──────────────────────────────────────────────────────────────

template<typename T>
inline T mix(T a, T b, float t) { return a + (b - a) * t; }

template<typename T>
inline T clamp(T v, T lo, T hi) { return std::max(lo, std::min(hi, v)); }

inline float sqlength(vec3 v) { return v.x*v.x + v.y*v.y + v.z*v.z; }

// Random float in [lo, hi)
inline float rand(float lo, float hi)
{
    return lo + (hi - lo) * (::rand() / (float)RAND_MAX);
}

static constexpr float F_TAU = 6.28318530718f;

// ──────────────────────────────────────────────────────────────
// String utilities (replaces lol::ends_with, lol::tolower, lol::split)
// ──────────────────────────────────────────────────────────────

inline bool ends_with(std::string const &str, std::string const &suffix)
{
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string tolower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

inline std::vector<std::string> split(std::string const &str, char delim)
{
    std::vector<std::string> result;
    std::string token;
    for (char c : str)
    {
        if (c == delim) { result.push_back(token); token.clear(); }
        else token += c;
    }
    if (!token.empty()) result.push_back(token);
    return result;
}

// ──────────────────────────────────────────────────────────────
// Logging (replaces lol::msg)
// ──────────────────────────────────────────────────────────────

namespace msg
{
    template<typename... Args>
    inline void error(const char *fmt, Args... args)
    {
#if defined(ARDUINO)
        Serial.printf(fmt, args...);
#else
        fprintf(stderr, fmt, args...);
#endif
    }

    template<typename... Args>
    inline void info(const char *fmt, Args... args)
    {
#if defined(ARDUINO)
        Serial.printf(fmt, args...);
#else
        printf(fmt, args...);
#endif
    }

    template<typename... Args>
    inline void warn(const char *fmt, Args... args)
    {
        info(fmt, args...);
    }
} // namespace msg

// ──────────────────────────────────────────────────────────────
// File I/O (replaces lol::file)
// ──────────────────────────────────────────────────────────────

namespace file
{
#if defined(ARDUINO)
    // Open a file from SD or SPIFFS.
    inline fs::File open(std::string const &path, const char* mode = FILE_READ)
    {
        if (path.rfind("/spiffs/", 0) == 0)
        {
            std::string subpath = path.substr(7); // remove "/spiffs" -> keep "/bios.p8"
            return SPIFFS.open(subpath.c_str(), mode);
        }
        return SD.open(path.c_str(), mode);
    }
#endif

    // Read entire file into string. Returns true on success.
    inline bool read(std::string const &path, std::string &out)
    {
#if defined(ARDUINO)
        fs::File f = open(path, FILE_READ);
        if (!f) return false;
        out.resize(f.size());
        f.read((uint8_t*)out.data(), f.size());
        f.close();
        return true;
#else
        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        out.resize(len);
        fread(out.data(), 1, len, fp);
        fclose(fp);
        return true;
#endif
    }

    // Write string to file. Returns true on success.
    inline bool write(std::string const &path, std::string const &data)
    {
#if defined(ARDUINO)
        fs::File f = open(path, FILE_WRITE);
        if (!f) return false;
        f.write((const uint8_t*)data.data(), data.size());
        f.close();
        return true;
#else
        FILE *fp = fopen(path.c_str(), "wb");
        if (!fp) return false;
        fwrite(data.data(), 1, data.size(), fp);
        fclose(fp);
        return true;
#endif
    }
} // namespace file

// ──────────────────────────────────────────────────────────────
// sys::getenv stub (not applicable on ESP32 — returns empty string)
// ──────────────────────────────────────────────────────────────
namespace sys
{
    inline std::string getenv(std::string const &) { return ""; }
}

} // namespace lol
