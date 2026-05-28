// bios.h — PICO-8 BIOS loader
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: memory-optimized binary bios partition/file loader
//          PC8C format (precompiled bytecode) support

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace z8::pico8
{

class bios
{
public:
    bios();

    const uint8_t* get_gfx()       const { return m_gfx_ptr; }
    const char*    get_code()      const { return m_code_ptr; }
    size_t         get_code_len()  const { return m_code_len; }
    bool           is_bytecode()   const { return m_is_bytecode; }

private:
    const char*    m_code_ptr    = nullptr;
    size_t         m_code_len    = 0;
    const uint8_t* m_gfx_ptr    = nullptr;
    bool           m_is_bytecode = false; // true = PC8C compiled bytecode

    // Allocated fallback data only if not mapped from partition
    std::string          m_fallback_code;
    std::vector<uint8_t> m_fallback_gfx;
};

} // namespace z8::pico8
