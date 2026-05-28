// tools/pc8_compile.cpp
// PC-side PICO-8 Lua precompiler using z8lua
//
// Usage:
//   pc8_compile bios  <input.p8>  <output.pc8c>
//   pc8_compile game  <input.p8>  <output.pc8c>
//
// Output format (PC8C):
//   [0]  4 bytes  magic "PC8C"
//   [4]  4 bytes  uint32_t rom_size (= 17408)
//   [8]  17408 b  ROM data (GFX, MAP, GFF, MUSIC, SFX)
//   [17416] 4 b   uint32_t bytecode_size
//   [17420] N b   z8lua bytecode (from lua_dump)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>

// ── z8lua headers ──────────────────────────────────────────────
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// Dummy PICO-8 RAM (required by lua_setpico8memory)
static uint8_t g_pico8_ram[32768] = {};

static std::vector<uint8_t> g_bytecode;

static int lua_dump_writer(lua_State* /*L*/, const void* p, size_t sz, void* /*ud*/)
{
    const uint8_t* src = (const uint8_t*)p;
    g_bytecode.insert(g_bytecode.end(), src, src + sz);
    return 0;
}

// PICO-8 ROM layout sizes
static const size_t ROM_SIZE = 17408; // 0x4400

struct P8Data {
    std::string lua_code;
    uint8_t     rom[ROM_SIZE];
};

static bool parse_p8(const char* filename, P8Data& out)
{
    memset(out.rom, 0, sizeof(out.rom));

    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", filename);
        return false;
    }

    enum class Section { NONE, LUA, GFX, MAP, GFF, MUSIC, SFX, OTHER };
    Section sec = Section::NONE;
    bool header_done = false;

    // Track write offsets for each section
    size_t gfx_offset   = 0;      // 0x0000 (size 0x2000)
    size_t map_offset   = 0x2000; // 0x2000 (size 0x1000)
    size_t gff_offset   = 0x3000; // 0x3000 (size 0x100)
    size_t music_offset = 0x3100; // 0x3100 (size 0x100)
    size_t sfx_offset   = 0x3200; // 0x3200 (size 0x1200)

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing CR/LF
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (!header_done) {
            if (strncmp(line, "version ", 8) == 0) header_done = true;
            continue;
        }

        // Section header detection
        if (strcmp(line, "__lua__")   == 0) { sec = Section::LUA; continue; }
        if (strcmp(line, "__gfx__")   == 0) { sec = Section::GFX; continue; }
        if (strcmp(line, "__map__")   == 0) { sec = Section::MAP; continue; }
        if (strcmp(line, "__gff__")   == 0) { sec = Section::GFF; continue; }
        if (strcmp(line, "__music__") == 0) { sec = Section::MUSIC; continue; }
        if (strcmp(line, "__sfx__")   == 0) { sec = Section::SFX; continue; }
        if (strncmp(line, "__", 2) == 0 && len >= 4 && line[len-1] == '_' && line[len-2] == '_' && !strchr(line, ' ') && !strchr(line, '=')) {
            sec = Section::OTHER;
            continue;
        }

        if (sec == Section::LUA) {
            out.lua_code += line;
            out.lua_code += '\n';
        }
        else if (sec == Section::GFX || sec == Section::MAP || sec == Section::GFF || sec == Section::MUSIC || sec == Section::SFX) {
            // Determine target offset, maximum size, and whether nibbles are swapped
            size_t* p_offset = nullptr;
            size_t  max_offset = 0;
            bool    swap_nibbles = false;

            if (sec == Section::GFX) {
                p_offset = &gfx_offset;
                max_offset = 0x2000;
                swap_nibbles = true;
            } else if (sec == Section::MAP) {
                p_offset = &map_offset;
                max_offset = 0x3000;
            } else if (sec == Section::GFF) {
                p_offset = &gff_offset;
                max_offset = 0x3100;
            } else if (sec == Section::SFX) {
                p_offset = &sfx_offset;
                max_offset = 0x4400;
            }

            if (sec == Section::MUSIC) {
                const char* p = line;
                int pat_hi = hexval(p[0]);
                int pat_lo = hexval(p[1]);
                if (pat_hi >= 0 && pat_lo >= 0) {
                    int pattern_id = (pat_hi << 4) | pat_lo;
                    p += 2;
                    while (*p && hexval(*p) < 0) p++;
                    size_t target_off = 0x3100 + 4 * pattern_id;
                    for (int ch = 0; ch < 4; ++ch) {
                        if (!p[0] || !p[1]) break;
                        int hi = hexval(p[0]);
                        int lo = hexval(p[1]);
                        if (hi >= 0 && lo >= 0) {
                            if (target_off < 0x3200) {
                                out.rom[target_off++] = (uint8_t)((hi << 4) | lo);
                            }
                        }
                        p += 2;
                    }
                }
            } else if (p_offset) {
                const char* p = line;
                auto hexval = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };

                while (p[0] && p[1]) {
                    if (*p_offset >= max_offset) break;
                    int hi = hexval(p[0]);
                    int lo = hexval(p[1]);
                    if (hi < 0 || lo < 0) { p++; continue; }

                    if (swap_nibbles) {
                        // GFX: Swap nibbles (low pixel is low nibble in memory)
                        out.rom[*p_offset] = (uint8_t)((lo << 4) | hi);
                    } else {
                        // Standard: High nibble first
                        out.rom[*p_offset] = (uint8_t)((hi << 4) | lo);
                    }
                    (*p_offset)++;
                    p += 2;
                }
            }
        }
    }
    fclose(f);

    if (out.lua_code.empty()) {
        fprintf(stderr, "Error: no __lua__ section found in %s\n", filename);
        return false;
    }

    printf("Parsed %s: lua=%zu bytes, GFX=%zu/8192, MAP=%zu/4096, SFX=%zu/4608\n",
           filename, out.lua_code.size(), gfx_offset, map_offset - 0x2000, sfx_offset - 0x3200);
    return true;
}

static bool write_pc8c(const char* outfile, const char* orig_filename,
                       const uint8_t* rom, uint32_t rom_size,
                       const uint8_t* bytecode, uint32_t bc_size)
{
    FILE* f = fopen(outfile, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create %s\n", outfile);
        return false;
    }

    std::string base_name = orig_filename;
    size_t last_slash = base_name.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        base_name = base_name.substr(last_slash + 1);
    }
    char name_buf[32];
    memset(name_buf, 0, sizeof(name_buf));
    strncpy(name_buf, base_name.c_str(), 31);

    fwrite("PC8C", 1, 4, f);
    fwrite(name_buf, 1, 32, f);
    fwrite(&rom_size, 4, 1, f);
    fwrite(rom, 1, rom_size, f);
    fwrite(&bc_size, 4, 1, f);
    fwrite(bytecode, 1, bc_size, f);
    fclose(f);

    printf("Written %s: header=40 rom=%u bytecode=%u total=%u bytes\n",
           outfile, rom_size, bc_size, 40 + rom_size + 4 + bc_size);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "PC8 Lua Precompiler (z8lua)\n"
            "Usage: %s <bios|game> <input.p8> <output.pc8c>\n",
            argv[0]);
        return 1;
    }

    const char* mode    = argv[1];
    const char* infile  = argv[2];
    const char* outfile = argv[3];

    bool is_bios = (strcmp(mode, "bios") == 0);
    bool is_game = (strcmp(mode, "game") == 0);
    if (!is_bios && !is_game) {
        fprintf(stderr, "Error: mode must be 'bios' or 'game'\n");
        return 1;
    }

    P8Data p8;
    if (!parse_p8(infile, p8)) return 1;

    lua_State* L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "Error: cannot create Lua state\n");
        return 1;
    }
    lua_setpico8memory(L, g_pico8_ram);
    luaL_openlibs(L);

    printf("Compiling %s Lua code (%zu bytes)...\n", mode, p8.lua_code.size());

    int status = luaL_loadbuffer(L,
                                 p8.lua_code.c_str(),
                                 p8.lua_code.size(),
                                 (std::string("@") + infile).c_str());
    if (status != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        fprintf(stderr, "Compile error: %s\n", msg ? msg : "(unknown)");
        lua_close(L);
        return 1;
    }

    g_bytecode.clear();
    status = lua_dump(L, lua_dump_writer, nullptr);
    if (status != 0) {
        fprintf(stderr, "Error: lua_dump failed with status %d\n", status);
        lua_close(L);
        return 1;
    }
    lua_close(L);

    if (g_bytecode.empty()) {
        fprintf(stderr, "Error: bytecode is empty after dump\n");
        return 1;
    }

    printf("Bytecode size: %zu bytes (%.1f%% of source)\n",
           g_bytecode.size(),
           100.0 * g_bytecode.size() / p8.lua_code.size());

    uint32_t rom_size = ROM_SIZE;
    uint32_t bc_size  = (uint32_t)g_bytecode.size();

    if (!write_pc8c(outfile, infile, p8.rom, rom_size, g_bytecode.data(), bc_size))
        return 1;

    printf("Done.\n");
    return 0;
}
