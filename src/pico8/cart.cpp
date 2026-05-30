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
//  ──────────────────────────────────────────────────────────────────────
//  Arduino/ESP32 port — changes from the original zepto8 cart.cpp:
//    • Replaced <lol/engine.h>, <lol/msg>, <lol/file>, <lol/utils> with
//      "compat/lol_compat.h"  (same API, SD-card backed on Arduino)
//    • Replaced lol::sys::get_data_path() with a local helper that just
//      returns the path unchanged (SD card is already rooted)
//    • Replaced PEGTL-based .p8 parser with a hand-written state-machine
//      parser (PEGTL is not available for ESP32/Arduino)
//    • Replaced std::format with snprintf-based helper fmt_hex()
//    • Removed std::filesystem (has_file_changed always returns false)
//    • Removed load_js() (QuickJS is not available on ESP32)
//    • lodepng include: "libs/lodepng/lodepng.h"
//    • All zepto8 logic preserved exactly
//  ──────────────────────────────────────────────────────────────────────

#include "compat/lol_compat.h"   // lol::msg, lol::file, lol::ends_with, …

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sstream>
#include <regex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "lodepng.h"

#include "pico8/cart.h"
#include "pico8/pico8.h"

#if defined(ARDUINO)
#include "esp_partition.h"
#endif

// ──────────────────────────────────────────────────────────────
// cart.cpp : 最上部（実体定義エリア）へ追記
// ──────────────────────────────────────────────────────────────
namespace z8::pico8
{

char* cart::s_code_buf = nullptr; 
int   cart::s_code_len = 0;

cart::~cart() {
    clear_code();
}

// バッファをリセットする処理
void cart::reset_code_buf() {
    if (!s_code_buf) {
#if defined(ARDUINO)
        // ⭕ ESP32環境（ストリームコンパイル）では、ロード時にLuaコード全体をRAMに載せる必要がないため、
        // 64KBバッファの確保を完全にバイパスして、貴重なヒープを温存します。
        s_code_buf = nullptr;
#else
        s_code_buf = new char[CODE_BUF_SIZE];
#endif
    }
    if (s_code_buf) {
        memset(s_code_buf, 0, CODE_BUF_SIZE);
    }
    s_code_len = 0;
}

void cart::clear_code() {
    s_code_len = 0;
    if (s_code_buf) {
#if defined(ARDUINO)
        free(s_code_buf);
        Serial.printf("[DEBUG] Freed s_code_buf: %p, free heap: %u\n", s_code_buf, (unsigned)ESP.getFreeHeap());
#else
        delete[] s_code_buf;
#endif
        s_code_buf = nullptr;
    }
}
}


// ──────────────────────────────────────────────────────────────────────────────
// Local helper: on the SD card the filename IS the full path already.
// This mirrors lol::sys::get_data_path(filename) which just prepends the
// data dir; on ESP32 we treat every path as absolute.
// ──────────────────────────────────────────────────────────────────────────────
static inline std::string get_data_path(std::string const &filename)
{
    return filename;
}

// ──────────────────────────────────────────────────────────────────────────────
// snprintf-based formatting helpers (replace std::format on Arduino)
// ──────────────────────────────────────────────────────────────────────────────
static std::string fmt_hex2(uint8_t v)
{
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", (unsigned)v);
    return std::string(buf);
}

static std::string fmt_hex1(uint8_t v)
{
    char buf[2];
    snprintf(buf, sizeof(buf), "%1x", (unsigned)(v & 0xf));
    return std::string(buf);
}

static std::string fmt_dec(int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    return std::string(buf);
}

namespace z8::pico8
{


using lol::ivec2;
namespace msg = lol::msg;
using lol::u8vec4;

void refresh_code_view(cart* c)
{
    return;
//    c->m_code_view.assign(cart::s_code_buf, cart::s_code_len);
}


// ──────────────────────────────────────────────────────────────────────────────
// cart::load — dispatch by extension
// ──────────────────────────────────────────────────────────────────────────────
bool cart::load(std::string const &filename)
{
    msg::info("loading file %s\n", filename.c_str());

    bool success = false;
    if (lol::ends_with(lol::tolower(filename), ".p8") && load_p8(filename))
        success = true;
    else if (lol::ends_with(lol::tolower(filename), ".lua") && load_lua(filename))
        success = true;
    else if (lol::ends_with(lol::tolower(filename), ".png") && load_png(filename))
        success = true;

    if (success)
    {
#if defined(ARDUINO)
        // Code is now in static buffer — no heap to free
        msg::info("Code loaded: %d bytes (static buffer)\n", cart::s_code_len);
#endif
        return true;
    }

    // .js (QuickJS) not available on ESP32 — skip
    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::load_png
// ──────────────────────────────────────────────────────────────────────────────
bool cart::load_png(std::string const &filename)
{
    init_filename(filename);

    // Open cartridge as PNG image
    std::vector<uint8_t> image;
    unsigned int width, height;
    unsigned int error = lodepng::decode(image, width, height, get_data_path(filename));

    if (error)
        return false;

    if (width * height != 160 * 205)
        return false;

    u8vec4 const *pixels = (u8vec4 const *)image.data();

    // Retrieve cartridge data from lower image bits
    std::vector<uint8_t> bytes(sizeof(m_rom) + 5);
    for (int n = 0; n < (int)bytes.size(); ++n)
    {
        u8vec4 p(pixels[n].r * 64, pixels[n].g * 64, pixels[n].b * 64, pixels[n].a * 64);
        bytes[n] = (p.a & 0xc0) + ((p.r >> 2) & 0x30) + ((p.g >> 4) & 0x0c) + ((p.b >> 6) & 0x03);
    }

    // Retrieve label from image pixels (disabled to save RAM)
    /*
    if (width >= LABEL_WIDTH + LABEL_X && height >= LABEL_HEIGHT + LABEL_Y)
    {
        m_label.resize(LABEL_WIDTH * LABEL_HEIGHT);
        for (int y = 0; y < LABEL_HEIGHT; ++y)
        for (int x = 0; x < LABEL_WIDTH; ++x)
        {
            lol::u8vec4 p = pixels[(y + LABEL_Y) * width + (x + LABEL_X)];
            m_label[y * LABEL_WIDTH + x] = palette::best(p, 32);
        }
    }
    */

    set_bin(bytes);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::load_lua
// ──────────────────────────────────────────────────────────────────────────────
bool cart::load_lua(std::string const &filename)
{
    init_filename(filename);

    // Reset static code buffer
    reset_code_buf();

    // Stream read .lua file in 4KB chunks (avoid loading entire file into memory)
    FILE* f = fopen(get_data_path(filename).c_str(), "r");
    if (!f) return false;

    // Remove CRLF for internal consistency without using std::regex (saves heavy RAM)
    static uint8_t chunk[4096];
    size_t bytes_read;

    while ((bytes_read = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        for (size_t i = 0; i < bytes_read; ++i)
        {
            char c = (char)chunk[i];
            // Skip \r before \n
            if (c == '\r') continue;
            append_code_char(c);
        }
    }
    fclose(f);

    // PICO-8 saves some symbols in the .lua file as Emoji/Unicode characters
    // but the runtime expects 8-bit characters instead.
    // Apply charset fix directly on static buffer (no heap allocation)
    charset::utf8_to_pico8_inplace(s_code_buf, s_code_len);

    refresh_code_view(this);
    init_title();
    memset(m_rom.data(), 0, sizeof(m_rom));
    init_rom();
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::set_bin  — copy raw binary data into ROM, decompress code section
// ──────────────────────────────────────────────────────────────────────────────
void cart::set_bin(std::vector<uint8_t> const &bytes)
{
    memcpy(m_rom.data(), bytes.data(), sizeof(m_rom));
    uint8_t const *vbytes = bytes.data() + sizeof(m_rom);
    int version = vbytes[0];
    int minor = (vbytes[1] << 24) | (vbytes[2] << 16) | (vbytes[3] << 8) | vbytes[4];

    // Retrieve code, with optional decompression → write to static buffer
    std::string decompressed = code::decompress(m_rom.data() + 0x4300);
    reset_code_buf();
    for (auto c : decompressed) append_code_char(c);

    refresh_code_view(this);
    init_title();

    msg::info("version: %d.%d code: %d chars\n", version, minor, s_code_len);

    // Invalidate code cache
    m_lua.resize(0);
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::init_rom  — reset music/sfx slots to disabled state
// ──────────────────────────────────────────────────────────────────────────────
void cart::init_rom()
{
    // init music sfx to be disabled
    for (int i = 0; i < 64; ++i)
    {
        m_rom[0x3100 + i * 4 + 0] = 65;
        m_rom[0x3100 + i * 4 + 1] = 66;
        m_rom[0x3100 + i * 4 + 2] = 67;
        m_rom[0x3100 + i * 4 + 3] = 68;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::init_title  — extract title/author from first two comment lines
// ──────────────────────────────────────────────────────────────────────────────
void cart::init_title()
{
    m_title.clear();
    m_author.clear();

    if (s_code_len < 2) return;
    const char* code = (const char*)s_code_buf;
    if (code[0] != '-' || code[1] != '-') return;
    size_t first = 0;
    for (size_t i = 0; i < (size_t)s_code_len; i++) {
        if (s_code_buf[i] == '\n') { first = i; break; }
    }
    if (first == 0) return;
    m_title.assign(code + 2, first - 2);

    if (first + 3 >= (size_t)s_code_len) return;
    if (code[first + 1] != '-' || code[first + 2] != '-') return;
    size_t second = 0;
    for (size_t i = first + 3; i < (size_t)s_code_len; i++) {
        if (s_code_buf[i] == '\n') { second = i; break; }
    }
    if (second == 0) return;
    m_author.assign(code + first + 3, second - first - 3);
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::init_filename
// ──────────────────────────────────────────────────────────────────────────────
void cart::init_filename(std::string filename)
{
    m_filename = filename;
    // No std::filesystem on ESP32 — has_file_changed() always returns false.
}

// ──────────────────────────────────────────────────────────────────────────────
// Hand-written .p8 parser
// Replaces the PEGTL-based p8_reader (PEGTL not available on Arduino).
//
// .p8 file format:
//   pico-8 cartridge // http://www.pico-8.com
//   version N
//   __lua__
//   <lua code lines>
//   __gfx__
//   <hex lines>
//   __gff__
//   <hex lines>
//   __map__
//   <hex lines>
//   __sfx__
//   <hex lines>
//   __music__
//   <hex lines>
//   __label__
//   <base32 lines>
// ──────────────────────────────────────────────────────────────────────────────

struct p8_reader
{
    enum class section : int8_t
    {
        error  = -1,
        header = 0,
        lua,
        gfx,
        gff,
        map,
        sfx,
        mus,
        lab,
    };

    int     m_version = -1;
    section m_current_section = section::header;

    // Direct write destinations (can be nullptr)
    cart::rom_t* m_rom = nullptr;
    std::vector<uint8_t>* m_label = nullptr;

    // Writing offsets and small record buffers
    size_t m_gfx_written = 0;
    size_t m_gff_written = 0;
    size_t m_map_written = 0;

    uint8_t m_current_sfx[84] = {};
    size_t m_current_sfx_size = 0;
    size_t m_sfx_index = 0;

    uint8_t m_current_mus[5] = {};
    size_t m_current_mus_size = 0;
    size_t m_mus_index = 0;

    size_t m_lab_written = 0;

    cart* m_cart = nullptr;  // writes code via append_code_char()
    bool    header_done = false;

    std::string* m_title_ptr = nullptr;
    std::string* m_author_ptr = nullptr;
    int m_lua_lines_written = 0;

    p8_reader(cart::rom_t* rom, std::vector<uint8_t>* label, cart* cart,
              std::string* title = nullptr, std::string* author = nullptr)
        : m_rom(rom), m_label(label), m_cart(cart),
          m_title_ptr(title), m_author_ptr(author)
    {}

    void flush_current_sfx()
    {
        if (!m_rom || m_sfx_index >= 64) return;
        if (m_current_sfx_size < 84) return;

        for (int j = 0; j < 32; ++j)
        {
            uint32_t ins = ((uint32_t)m_current_sfx[4 + j * 5 / 2 + 0] << 16)
                         | ((uint32_t)m_current_sfx[4 + j * 5 / 2 + 1] << 8)
                         | ((uint32_t)m_current_sfx[4 + j * 5 / 2 + 2]);
            ins = (j & 1) ? ins & 0xfffff : ins >> 4;

            uint16_t key        = (ins & 0x3f000) >> 12;
            uint16_t instrument = (ins & 0x700)   >> 8;
            uint16_t volume     = (ins & 0x70)    >> 4;
            uint16_t effect     =  ins & 0x7;
            uint16_t custom     = (ins & 0x800)   >> 11;

            uint16_t note_val = key | (instrument << 6) | (volume << 9) | (effect << 12) | (custom << 15);
            m_rom->data()[0x3200 + m_sfx_index * 68 + j * 2 + 0] = note_val & 0xff;
            m_rom->data()[0x3200 + m_sfx_index * 68 + j * 2 + 1] = (note_val >> 8) & 0xff;
        }

        m_rom->data()[0x3200 + m_sfx_index * 68 + 64] = m_current_sfx[0];
        m_rom->data()[0x3200 + m_sfx_index * 68 + 65] = m_current_sfx[1];
        m_rom->data()[0x3200 + m_sfx_index * 68 + 66] = m_current_sfx[2];
        m_rom->data()[0x3200 + m_sfx_index * 68 + 67] = m_current_sfx[3];

        m_sfx_index++;
        m_current_sfx_size = 0;
    }

    void flush_current_mus()
    {
        if (!m_rom || m_mus_index >= 64) return;
        if (m_current_mus_size < 5) return;

        m_rom->data()[0x3100 + m_mus_index * 4 + 0] = m_current_mus[1] | ((m_current_mus[0] << 7) & 0x80);
        m_rom->data()[0x3100 + m_mus_index * 4 + 1] = m_current_mus[2] | ((m_current_mus[0] << 6) & 0x80);
        m_rom->data()[0x3100 + m_mus_index * 4 + 2] = m_current_mus[3] | ((m_current_mus[0] << 5) & 0x80);
        m_rom->data()[0x3100 + m_mus_index * 4 + 3] = m_current_mus[4] | ((m_current_mus[0] << 4) & 0x80);

        m_mus_index++;
        m_current_mus_size = 0;
    }

    void parse_line(const char* line)
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        {
            len--;
        }

        if (!header_done)
        {
            if (strstr(line, "pico-8 cartridge") != nullptr)
                return;
            if (strncmp(line, "version ", 8) == 0)
            {
                m_version = std::atoi(line + 8);
                header_done = true;
                return;
            }
            return;
        }

        if (len >= 7 && line[0] == '_' && line[1] == '_')
        {
            char section_name[16];
            size_t name_len = 0;
            for (size_t i = 0; i < len && name_len < 15; i++)
            {
                if (line[i] != ' ' && line[i] != '\t')
                {
                    section_name[name_len++] = line[i];
                }
            }
            section_name[name_len] = '\0';

            if (m_current_section == section::sfx) flush_current_sfx();
            if (m_current_section == section::mus) flush_current_mus();

            if      (strcmp(section_name, "__lua__") == 0)   { m_current_section = section::lua; return; }
            else if (strcmp(section_name, "__gfx__") == 0)   { m_current_section = section::gfx; return; }
            else if (strcmp(section_name, "__gff__") == 0)   { m_current_section = section::gff; return; }
            else if (strcmp(section_name, "__map__") == 0)   { m_current_section = section::map; return; }
            else if (strcmp(section_name, "__sfx__") == 0)   { m_current_section = section::sfx; return; }
            else if (strcmp(section_name, "__music__") == 0) { m_current_section = section::mus; return; }
            else if (strcmp(section_name, "__label__") == 0) { m_current_section = section::lab; return; }
        }

        if (m_current_section == section::lua)
        {
            // Track #include flag via cart method
            if (m_cart && !m_cart->has_includes() && strstr(line, "#include ") != nullptr)
            {
                m_cart->set_has_includes(true);
            }

            bool in_string = false;
            char string_char = 0;
            size_t comment_start = len;
            for (size_t i = 0; i < len; i++)
            {
                char c = line[i];
                if (!in_string)
                {
                    if (c == '"' || c == '\'')
                    {
                        in_string = true;
                        string_char = c;
                    }
                    else if (c == '-' && i + 1 < len && line[i+1] == '-')
                    {
                        comment_start = i;
                        break;
                    }
                }
                else
                {
                    if (c == string_char)
                    {
                        int slashes = 0;
                        for (int j = (int)i - 1; j >= 0 && line[j] == '\\'; j--) slashes++;
                        if (slashes % 2 == 0) in_string = false;
                    }
                }
            }

            size_t end_pos = comment_start;
            while (end_pos > 0 && (line[end_pos - 1] == ' ' || line[end_pos - 1] == '\t'))
            {
                end_pos--;
            }

            size_t start_pos = 0;
            while (start_pos < end_pos && (line[start_pos] == ' ' || line[start_pos] == '\t'))
            {
                start_pos++;
            }

            if (start_pos < end_pos)
            {
                // Extract title/author from first two comment lines
                if (m_lua_lines_written < 2)
                {
                    if (line[start_pos] == '-' && start_pos + 1 < end_pos && line[start_pos + 1] == '-')
                    {
                        size_t vstart = start_pos + 2;
                        while (vstart < end_pos && (line[vstart] == ' ' || line[vstart] == '\t')) vstart++;
                        size_t vend = end_pos;
                        while (vend > vstart && (line[vend - 1] == ' ' || line[vend - 1] == '\t')) vend--;

                        if (m_lua_lines_written == 0 && m_title_ptr)
                            m_title_ptr->assign(line + vstart, vend - vstart);
                        else if (m_lua_lines_written == 1 && m_author_ptr)
                            m_author_ptr->assign(line + vstart, vend - vstart);
                    }
                    m_lua_lines_written++;
                }

#if !defined(ARDUINO)
                // Write code line directly to static buffer via cart
                if (m_cart)
                {
                    for (size_t i = start_pos; i < end_pos; i++)
                        m_cart->append_code_char(line[i]);
                    m_cart->append_code_char('\n');
                }
#endif
            }
        }
        else if (m_current_section != section::error &&
                 m_current_section != section::header)
        {
            bool const is_swapped = (m_current_section == section::gfx);
            bool const is_base32  = (m_current_section == section::lab);

            const uint8_t *p   = (const uint8_t *)line;
            const uint8_t *end = p + len;

            if (is_base32)
            {
                for (; p < end; ++p)
                {
                    uint8_t ch = *p;
                    int8_t  b  = (ch >= '0' && ch <= '9') ? (int8_t)(ch - '0')
                               : (ch >= 'a' && ch <= 'v') ? (int8_t)(ch - 'a' + 10)
                               : (ch >= 'A' && ch <= 'V') ? (int8_t)(ch - 'A' + 10)
                               : -1;
                    if (b >= 0)
                    {
                        if (m_label && m_lab_written < m_label->size())
                        {
                            (*m_label)[m_lab_written++] = (uint8_t)b;
                        }
                    }
                }
            }
            else
            {
                while (p + 1 < end)
                {
                    uint8_t c0 = p[is_swapped ? 1 : 0];
                    uint8_t c1 = p[is_swapped ? 0 : 1];
                    bool c0ok = (c0 >= '0' && c0 <= '9') || (c0 >= 'a' && c0 <= 'f') || (c0 >= 'A' && c0 <= 'F');
                    bool c1ok = (c1 >= '0' && c1 <= '9') || (c1 >= 'a' && c1 <= 'f') || (c1 >= 'A' && c1 <= 'F');
                    if (!c0ok || !c1ok) { ++p; continue; }
                    char tmp[3] = { (char)c0, (char)c1, '\0' };
                    uint8_t val = (uint8_t)strtoul(tmp, nullptr, 16);
                    p += 2;

                    switch (m_current_section)
                    {
                        case section::gfx:
                        {
                            if (m_rom && m_gfx_written < 0x2000)
                            {
                                m_rom->data()[m_gfx_written++] = val;
                            }
                            break;
                        }
                        case section::gff:
                        {
                            if (m_rom && m_gff_written < 0x100)
                            {
                                m_rom->data()[0x3000 + m_gff_written++] = val;
                            }
                            break;
                        }
                        case section::map:
                        {
                            if (m_rom)
                            {
                                if (m_map_written < 4096)
                                {
                                    m_rom->data()[0x2000 + m_map_written++] = val;
                                }
                                else if (m_map_written < 8192)
                                {
                                    size_t idx = m_map_written - 4096;
                                    m_rom->data()[0x1000 + idx] |= val;
                                    m_map_written++;
                                }
                            }
                            break;
                        }
                        case section::sfx:
                        {
                            if (m_current_sfx_size < sizeof(m_current_sfx))
                            {
                                m_current_sfx[m_current_sfx_size++] = val;
                                if (m_current_sfx_size == 84)
                                    flush_current_sfx();
                            }
                            break;
                        }
                        case section::mus:
                        {
                            if (m_current_mus_size < sizeof(m_current_mus))
                            {
                                m_current_mus[m_current_mus_size++] = val;
                                if (m_current_mus_size == 5)
                                    flush_current_mus();
                            }
                            break;
                        }
                        default: break;
                    }
                }
            }
        }
    }

    void parse(std::string const &str)
    {
        std::istringstream iss(str);
        std::string line;
        while (std::getline(iss, line))
        {
            parse_line(line.c_str());
        }
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// cart::load_p8_mem
// ──────────────────────────────────────────────────────────────────────────────
bool cart::load_p8_mem(const char* data, size_t size)
{
    init_filename("memory");

    // Reset static code buffer
    reset_code_buf();

    memset(&m_rom, 0, sizeof(m_rom));
    init_rom();

    auto reader = std::make_unique<p8_reader>(&m_rom, &m_label, this, &m_title, &m_author);

    const char* p = data;
    const char* end = data + size;
    char line_buf[512];
    size_t line_len = 0;

    while (p < end)
    {
        char c = *p++;
        if (c == '\0') // stop on null bytes (padding in flash partition)
            break;

        if (c == '\n')
        {
            line_buf[line_len] = '\0';
            reader->parse_line(line_buf);
            line_len = 0;
        }
        else if (c != '\r')
        {
            if (line_len < sizeof(line_buf) - 1)
            {
                line_buf[line_len++] = c;
            }
        }
    }
    if (line_len > 0)
    {
        line_buf[line_len] = '\0';
        reader->parse_line(line_buf);
    }

    // Flush any remaining records at EOF
    reader->flush_current_sfx();
    reader->flush_current_mus();

    m_version = reader->m_version;

    // Apply charset fix directly on static buffer (no heap allocation)
    charset::utf8_to_pico8_inplace(s_code_buf, s_code_len);

    refresh_code_view(this);
    init_title();

    msg::info("version: %d code: %d (static buf)\n", m_version, s_code_len);

    // Invalidate code cache
    m_lua.resize(0);

    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::load_p8
// ──────────────────────────────────────────────────────────────────────────────
#if defined(ARDUINO)
#include <SD.h>
#else
#include <fstream>
#endif

bool cart::load_p8(std::string const &filename)
{
    init_filename(filename);

    std::string path = get_data_path(filename);
#if defined(ARDUINO)
    File f = lol::file::open(path, FILE_READ);
    if (!f)
        return false;

    msg::info("loading file %s (stream mode)\n", filename.c_str());

    // Reset static code buffer
    reset_code_buf();

    memset(&m_rom, 0, sizeof(m_rom));
    init_rom();

    auto reader = std::make_unique<p8_reader>(&m_rom, &m_label, this, &m_title, &m_author);

    char line_buf[512];
    size_t line_len = 0;

    while (f.available())
    {
        int c = f.read();
        if (c < 0) break;

        if (c == '\n')
        {
            line_buf[line_len] = '\0';
            reader->parse_line(line_buf);
            line_len = 0;
        }
        else if (c != '\r')
        {
            if (line_len < sizeof(line_buf) - 1)
            {
                line_buf[line_len++] = (char)c;
            }
        }
    }
    if (line_len > 0)
    {
        line_buf[line_len] = '\0';
        reader->parse_line(line_buf);
    }
    f.close();
#else
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return false;

    msg::info("loading file %s\n", filename.c_str());

    // Reset static code buffer
    reset_code_buf();

    memset(&m_rom, 0, sizeof(m_rom));
    init_rom();

    auto reader = std::make_unique<p8_reader>(&m_rom, &m_label, this, &m_title, &m_author);

    std::string line;
    while (std::getline(ifs, line))
    {
        reader->parse_line(line);
    }
#endif

    // Flush any remaining records at EOF
    reader->flush_current_sfx();
    reader->flush_current_mus();

    msg::info("loaded file %s\n", filename.c_str());

    if (reader->m_version < 0)
        return false;

    // Apply charset fix directly on static buffer (no heap allocation)
    charset::utf8_to_pico8_inplace(s_code_buf, s_code_len);

    refresh_code_view(this);
    init_title();

    // Actual sizes actually loaded
    msg::info("version: %d code: %d gfx: %d gff: %d map: %d sfx: %d mus: %d lab: %d\n",
              reader->m_version, s_code_len,
              (int)reader->m_gfx_written,
              (int)reader->m_gff_written,
              (int)reader->m_map_written,
              (int)reader->m_sfx_index,
              (int)reader->m_mus_index,
              (int)reader->m_lab_written);

    // Invalidate code cache
    m_lua.resize(0);

    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::preprocess_code
// Handles #include directives in the Lua code.
// ──────────────────────────────────────────────────────────────────────────────
std::string cart::preprocess_code() const
{
    // fast path if no include is found in the file
    size_t found_hashtag = get_code().find("#include ");
    if (found_hashtag == std::string::npos)
    {
        return get_code();
    }

    // get file base path
    size_t found_path = m_filename.find_last_of("/\\");
    std::string include_path = m_filename.substr(0, found_path);

    // TODO: optimize by finding all include points, and only stitch code
    //       there instead of each line
    // TODO: skip #include in multiline comment and multiline string

    // handle include of files
    std::string final_code;
    std::istringstream iss(get_code());
    for (std::string line; std::getline(iss, line); )
    {
        size_t found_include = line.find("#include ");
        if (   found_include != std::string::npos
            && line.find_first_of("\"'") == std::string::npos
            && line.find("--") == std::string::npos) // verify include is not in string or comment
        {
            // local file name
            std::string incfile = line.substr(found_include + 9);
            std::string include_name = include_path + "/" + incfile;

            std::string s;
            if (!lol::file::read(get_data_path(include_name), s))
            {
                lol::msg::error("cannot load include cart: %s\n", include_name.c_str());
                continue;
            }

            msg::info("loaded include file %s\n", include_name.c_str());

            if (lol::ends_with(lol::tolower(incfile), ".p8"))
            {
                // Parse .p8 include — save current static buf, parse into it, restore
                int saved_len = cart::s_code_len;
                char* saved_buf = new char[saved_len];
                memcpy(saved_buf, cart::s_code_buf, saved_len);
                cart::s_code_len = 0;  // reset (static, so no const issue)
                auto reader = std::make_unique<p8_reader>(nullptr, nullptr, const_cast<cart*>(this), nullptr, nullptr);
                reader->parse(s);
                std::string inc_code((const char*)cart::s_code_buf, cart::s_code_len);
                // Restore saved buffer
                cart::s_code_len = 0;
                for (int i = 0; i < saved_len; i++)
                    cart::s_code_buf[cart::s_code_len++] = (uint8_t)saved_buf[i];
                delete[] saved_buf;
                final_code += charset::utf8_to_pico8(inc_code) + "\n";
            }
            else
            {
                final_code += charset::utf8_to_pico8(s) + "\n";
            }
        }
        else
        {
            final_code += line + "\n";
        }
    }

    return final_code;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::save — dispatch by extension
// ──────────────────────────────────────────────────────────────────────────────
bool cart::save(std::string const &filename) const
{
    msg::info("saving file %s\n", filename.c_str());

    if (lol::ends_with(lol::tolower(filename), ".p8") && save_p8(filename))
        return true;

    if (lol::ends_with(lol::tolower(filename), ".png") && save_png(filename))
        return true;

    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::save_png
// ──────────────────────────────────────────────────────────────────────────────
bool cart::save_png(std::string const &filename) const
{
    // Open blank cartridge template
    std::vector<uint8_t> image;
    unsigned int width, height;
    unsigned int error = lodepng::decode(image, width, height,
                                         get_data_path("data/blank.png"));
    if (error != 0)
    {
        lol::msg::error("cannot load blank cart: %s\n", lodepng_error_text(error));
        return false;
    }

    u8vec4 *pixels = (u8vec4 *)image.data();

    // Apply label
    if (m_label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        for (int y = 0; y < LABEL_HEIGHT; ++y)
        for (int x = 0; x < LABEL_WIDTH; ++x)
        {
            uint8_t col = m_label[y * LABEL_WIDTH + x] & 0x1f;
            pixels[(y + LABEL_Y) * width + (x + LABEL_X)] = palette::get8(col);
        }
    }

    // Create ROM data
    std::vector<uint8_t> const &rom = get_bin();

    // Write ROM to lower image bits
    for (size_t n = 0; n < rom.size(); ++n)
    {
        u8vec4 &px = pixels[n];
        uint8_t  b = rom[n];
        // Encode 8 bits across RGBA channel LSBs:
        //   bits[7:6] → alpha bits[7:6]
        //   bits[5:4] → red   bits[5:4]
        //   bits[3:2] → green bits[3:2]
        //   bits[1:0] → blue  bits[1:0]
        px.r = (px.r & 0xfc) | ((b >> 4) & 0x03);
        px.g = (px.g & 0xfc) | ((b >> 2) & 0x03);
        px.b = (px.b & 0xfc) | ( b       & 0x03);
        px.a = (px.a & 0xfc) | ((b >> 6) & 0x03);
    }

    error = lodepng::encode(filename, image, width, height);
    if (error != 0)
    {
        lol::msg::error("cannot save cart: %s\n", lodepng_error_text(error));
        return false;
    }

    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::set_from_ram  — copy RAM data back into the cart ROM
// ──────────────────────────────────────────────────────────────────────────────
void cart::set_from_ram(uint8_t const *ram, int in_dst, int in_src, int in_size)
{
    // If writing after the cart, nothing to do
    if (in_dst > 0x4300)
    {
        return;
    }

    // Now copy possibly legal data
    int amount = std::min(in_size, 0x4300 - in_dst);

    if (amount <= 0)
    {
        return;
    }

    ::memcpy(&m_rom[in_dst], &ram[in_src], amount);
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::get_compressed_code
// ──────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> cart::get_compressed_code() const
{
    return code::compress(std::string((const char*)s_code_buf, s_code_len));
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::get_bin — assemble full binary ROM (data + compressed code)
// ──────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> cart::get_bin() const
{
    int const data_size = 0x4300;

    // Create ROM image
    std::vector<uint8_t> ret;

    // Copy non-code data to ROM
    ret.resize(data_size);
    memcpy(ret.data(), m_rom.data(), data_size);

    // Compress and append code
    auto compressed = code::compress(std::string((const char*)s_code_buf, s_code_len));
    ret.insert(ret.end(), compressed.begin(), compressed.end());

    msg::info("compressed code length: %d/%d\n",
              (int)compressed.size(), (int)sizeof(memory::code));
    ret.resize(sizeof(memory));

    ret.push_back(PICO8_VERSION);

    return ret;
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::save_p8 — serialise cart to .p8 text format
// ──────────────────────────────────────────────────────────────────────────────
bool cart::save_p8(std::string const &filename) const
{
    std::string ret = "pico-8 cartridge // http://www.pico-8.com\n";
    ret += "version " + fmt_dec(int(PICO8_VERSION)) + "\n";

    ret += "__lua__\n";
    ret += z8::pico8::charset::pico8_to_utf8(get_code());
    if (ret.back() != '\n')
        ret += '\n';

    // Export gfx section
    int gfx_lines = 0;
    for (int i = 0; i < 0x2000; ++i)
        if (m_rom[i] != 0)
            gfx_lines = 1 + i / 64;

    for (int line = 0; line < gfx_lines; ++line)
    {
        if (line == 0)
            ret += "__gfx__\n";

        for (int i = 0; i < 64; ++i)
        {
            uint8_t val = m_rom[line * 64 + i];
            ret += fmt_hex2(uint8_t(val * 0x101 / 0x10));
        }
        ret += '\n';
    }

    // Export label
    if (m_label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        ret += "__label__\n";
        for (int i = 0; i < LABEL_WIDTH * LABEL_HEIGHT; ++i)
        {
            uint8_t col = m_label.data()[i];
            ret += "0123456789abcdefghijklmnopqrstuv"[col & 0x1f];
            if ((i + 1) % LABEL_WIDTH == 0)
                ret += '\n';
        }
        ret += '\n';
    }

    // Export gff section
    int gff_lines = 0;
    for (int i = 0; i < 0x100; ++i)
        if (m_rom[0x3000 + i] != 0)
            gff_lines = 1 + i / 128;

    for (int line = 0; line < gff_lines; ++line)
    {
        if (line == 0)
            ret += "__gff__\n";

        for (int i = 0; i < 128; ++i)
            ret += fmt_hex2(m_rom[0x3000 + 128 * line + i]);

        ret += '\n';
    }

    // Export map section
    int map_lines = 0;
    for (int i = 0; i < 4096; ++i)
        if (m_rom[0x2000 + i] != 0)
            map_lines = 1 + i / 128;

    for (int line = 0; line < map_lines; ++line)
    {
        if (line == 0)
            ret += "__map__\n";

        for (int i = 0; i < 128; ++i)
            ret += fmt_hex2(m_rom[0x2000 + 128 * line + i]);

        ret += '\n';
    }

    // Export sfx section
    int sfx_lines = 0;
    for (int i = 0; i < 4352; ++i)
        if (m_rom[0x3200 + i] != 0)
            sfx_lines = 1 + i / 68;

    for (int line = 0; line < sfx_lines; ++line)
    {
        if (line == 0)
            ret += "__sfx__\n";

        const uint8_t *data = m_rom.data() + 0x3200 + line * 68;
        ret += fmt_hex2(data[64]);
        ret += fmt_hex2(data[65]);
        ret += fmt_hex2(data[66]);
        ret += fmt_hex2(data[67]);
        for (int j = 0; j < 64; j += 2)
        {
            int pitch      =  data[j]       & 0x3f;
            int instrument = ((data[j + 1] << 2) & 0x4) | (data[j] >> 6);
            int volume     = (data[j + 1] >> 1) & 0x7;
            int effect     = (data[j + 1] >> 4) & 0xf;
            ret += fmt_hex2((uint8_t)pitch);
            ret += fmt_hex1((uint8_t)instrument);
            ret += fmt_hex1((uint8_t)volume);
            ret += fmt_hex1((uint8_t)effect);
        }

        ret += '\n';
    }

    // Export music section
    int music_lines = 0;
    for (int i = 0; i < 256; ++i)
        if (m_rom[0x3100 + i] != 0)
            music_lines = 1 + i / 4;

    for (int line = 0; line < music_lines; ++line)
    {
        if (line == 0)
            ret += "__music__\n";

        uint8_t const *sdata = m_rom.data() + 0x3100 + line * 4;
        int flags = (sdata[0] >> 7) | ((sdata[1] >> 7) << 1) | ((sdata[2] >> 7) << 2) | ((sdata[3] >> 7) << 3);

        ret += fmt_hex2((uint8_t)flags);
        ret += ' ';
        ret += fmt_hex2(sdata[0] & 0x7f);
        ret += fmt_hex2(sdata[1] & 0x7f);
        ret += fmt_hex2(sdata[2] & 0x7f);
        ret += fmt_hex2(sdata[3] & 0x7f);
        ret += '\n';
    }

    ret += '\n';

    return lol::file::write(filename, ret);
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::load_from_partition — mmap "game" partition, parse cached .p8 data
// ──────────────────────────────────────────────────────────────────────────────
bool cart::load_from_partition(std::string const &filename)
{
#if defined(ARDUINO)
    init_filename(filename);
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "game");
    if (!partition) {
        msg::info("game partition not found\n");
        return false;
    }

    spi_flash_mmap_handle_t map_handle;
    const void *map_ptr = nullptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       SPI_FLASH_MMAP_DATA, &map_ptr, &map_handle);
    if (err != ESP_OK) {
        msg::error("game partition mmap failed: 0x%x\n", err);
        return false;
    }

    const char* base = (const char*)map_ptr;

    // Magic: "PC8G" = raw .p8 cache (runtime), "PC8C" = bytecode (flash script)
    // If PC8C, let api_run() handle it — don't overwrite with raw cache
    if (strncmp(base, "PC8C", 4) == 0) {
        char cached_name[33];
        memcpy(cached_name, base + 4, 32);
        cached_name[32] = '\0';

        std::string req_base = filename;
        size_t last_slash = req_base.find_last_of('/');
        if (last_slash != std::string::npos) {
            req_base = req_base.substr(last_slash + 1);
        }

        if (req_base == cached_name) {
            msg::info("game partition has matching PC8C bytecode (%s), bypass file loading\n", cached_name);
            (void)map_handle;
            return true;
        } else {
            msg::info("game partition has PC8C but filename mismatch: '%s' != '%s', falling back to SD load\n",
                      cached_name, req_base.c_str());
        }
    }
    if (strncmp(base, "PC8G", 4) != 0) {
        msg::info("game partition cache miss (invalid magic)\n");
        (void)map_handle;
        return false;
    }

    uint32_t data_size = *(const uint32_t*)(base + 4);
    // Header: "PC8G"(4) + data_size(4) + filename(32) = 40 bytes total
    constexpr int header_size = 40;
    if (data_size > partition->size - header_size) {
        msg::error("game partition data_size too large: %u\n", data_size);
        (void)map_handle;
        return false;
    }

    // Check cached filename matches the requested file
    char cached_name[33];
    memcpy(cached_name, base + 8, 32);
    cached_name[32] = '\0';
    
    if (m_filename.empty() || strcmp(cached_name, m_filename.c_str()) != 0) {
        msg::info("game partition cache miss (filename mismatch: '%s' != '%s')\n", 
                  cached_name, m_filename.c_str());
        (void)map_handle;
        return false;
    }

    const char* p8_data = base + header_size;
    msg::info("Loaded game from flash partition (%u bytes)\n", data_size);

    // Parse the .p8 data from mmap'd memory
    return load_p8_mem(p8_data, data_size);
#else
    return false;
#endif
}

// ──────────────────────────────────────────────────────────────────────────────
// cart::save_to_partition — stream .p8 data to "game" partition (4KB chunks)
// Called from vm::load_cart after successful SD load
// ──────────────────────────────────────────────────────────────────────────────
bool cart::save_to_partition() const
{
#if defined(ARDUINO)
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "game");
    if (!partition) {
        msg::info("game partition not found, skipping cache\n");
        return false;
    }

    // Don't overwrite PC8C bytecode written by flash script
    uint8_t magic_buf[4];
    esp_err_t read_err = esp_partition_read(partition, 0, magic_buf, 4);
    if (read_err == ESP_OK && memcmp(magic_buf, "PC8C", 4) == 0) {
        msg::info("Game partition has PC8C bytecode, skipping raw cache\n");
        return true;
    }

    // Open the .p8 file from SD for streaming read (4KB chunks)
    FILE* f = fopen(get_data_path(m_filename).c_str(), "rb");
    if (!f) {
        msg::error("Failed to open %s for partition cache\n", m_filename.c_str());
        return false;
    }

    // Get file size by seeking
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t data_size = (uint32_t)file_size;
    constexpr int header_size = 40; // "PC8G"(4) + size(4) + filename(32)
    uint32_t total_size = header_size + data_size;
    if (total_size > partition->size) {
        msg::error("Game too large for partition: %u > %u\n",
                   total_size, partition->size);
        fclose(f);
        return false;
    }

    msg::info("Caching game to flash partition (%u bytes, streaming)\n", total_size);

    // Erase partition first
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        msg::error("Partition erase failed: 0x%x\n", err);
        fclose(f);
        return false;
    }

    // Write header: "PC8G"(4) + size(4) + filename(32) = 40 bytes
    uint8_t header[40];
    memset(header, 0, sizeof(header));
    memcpy(header, "PC8G", 4);
    memcpy(header + 4, &data_size, 4);
    if (!m_filename.empty()) {
        size_t copy_len = std::min(m_filename.size(), (size_t)32);
        memcpy(header + 8, m_filename.c_str(), copy_len);
    }

    err = esp_partition_write(partition, 0, header, sizeof(header));
    if (err != ESP_OK) {
        msg::error("Partition header write failed: 0x%x\n", err);
        fclose(f);
        return false;
    }

    // Stream file data in 4KB chunks directly to flash partition
    static uint8_t chunk[4096];
    uint32_t write_offset = header_size; // After header
    size_t bytes_read;

    while ((bytes_read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        err = esp_partition_write(partition, write_offset, chunk, bytes_read);
        if (err != ESP_OK) {
            msg::error("Partition write failed at offset %u: 0x%x\n", write_offset, err);
            fclose(f);
            return false;
        }
        write_offset += (uint32_t)bytes_read;
    }

    fclose(f);

    // Verify we wrote all data
    if (write_offset != total_size) {
        msg::error("Partition size mismatch: wrote %u, expected %u\n", write_offset, total_size);
        return false;
    }

    msg::info("Game cached to flash partition successfully (streaming)\n");
    return true;
#else
    return false;
#endif
}

} // namespace z8::pico8
