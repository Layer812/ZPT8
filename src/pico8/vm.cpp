// pico8/vm.cpp — PICO-8 VM core implementation
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes:
//   - Removed lol/engine.h, lol/file.h, lol/utils.h → lol_compat.h
//   - Replaced std::chrono with millis() for ESP32/Arduino timing
//   - Removed network download, filesystem watch, NX/SCE ifdefs
//   - File paths now rooted at /saves/ and /config/ on SD card
//   - std::format → snprintf/+ where needed (ESP32 has limited <format>)

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>
#include <ctime>
#include <any>

#include "compat/lol_compat.h"

#include "pico8/pico8.h"
#include "pico8/vm.h"
#include "bindings/lua.h"
#include "bios.h"

#include "lauxlib.h"
#include "lualib.h"

#if defined(ARDUINO)
#include "esp_partition.h"
#include "esp_spi_flash.h"
#endif
namespace z8::pico8 {

static lua_State* g_active_lua = nullptr;

static void* baremetal_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; 
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return nullptr;
    }
    void* result = realloc(ptr, nsize);
    if (result == nullptr && g_active_lua != nullptr) {
#if defined(ARDUINO)
        Serial.printf("[WARN] OOM in baremetal_lua_alloc (nsize=%d)! Free Heap: %u, MaxAlloc: %u. Running full GC...\n",
                      (int)nsize, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        Serial.flush();
        
        // Dump Lua callstack for diagnostic purposes
        lua_Debug ar;
        int level = 0;
        Serial.println("=== LUA OOM CALLSTACK DUMP ===");
        while (lua_getstack(g_active_lua, level, &ar)) {
            lua_getinfo(g_active_lua, "nSl", &ar);
            Serial.printf("  [%d] %s:%d in function '%s' (type: %s)\n",
                          level,
                          ar.short_src ? ar.short_src : "?",
                          ar.currentline,
                          ar.name ? ar.name : "?",
                          ar.namewhat ? ar.namewhat : "?");
            level++;
        }
        Serial.println("==============================");
        Serial.flush();
#endif
        lua_gc(g_active_lua, LUA_GCCOLLECT, 0);
        result = realloc(ptr, nsize);
#if defined(ARDUINO)
        if (result != nullptr) {
            Serial.println("[INFO] GC recovered memory successfully!");
            Serial.flush();
        } else {
            Serial.println("[ERROR] GC could not recover enough memory!");
            Serial.flush();
        }
#endif
    }
    return result;
}


const uint8_t* g_bios_gfx_ptr = nullptr;

static const char glue_code[] = R"GLUE_CODE(
if (_init) _init()
if _update or _update60 or _draw then
    while true do
        if _update60 then
            _update_buttons()
            _mainloop=_update60
            _set_mainloop_exists(true)
            _update60()
            _mainloop=nil
            _set_mainloop_exists(false)
        else
            yield()
            _update_buttons()
            if _update then
                _mainloop=_update
                _set_mainloop_exists(true)
                _update()
                _mainloop=nil
                _set_mainloop_exists(false)
            end
        end
        if _draw then
            holdframe()
            _mainloop=_draw
            _set_mainloop_exists(true)
            _draw()
            _mainloop=nil
            _set_mainloop_exists(false)
            flip()
        else
            yield()
        end
    end
end
)GLUE_CODE";
}

// Binding specialisation for rich_string
template<> void z8::bindings::lua_get(lua_State *l, int n,
                                      z8::pico8::rich_string &arg)
{
    if (lua_isnone(l, n))
        arg.assign("[no value]");
    else if (lua_isnil(l, n))
        arg.assign("[nil]");
    else if (lua_type(l, n) == LUA_TSTRING)
    {
        size_t len;
        char const *s = lua_tolstring(l, n, &len);
        arg.assign(s, len);
    }
    else if (lua_isnumber(l, n))
    {
        char buffer[20];
        fix32 x = lua_tonumber(l, n);
        int i = sprintf(buffer, "%.4f", (double)x);
        while (i > 2 && buffer[i-1] == '0' && ::isdigit(buffer[i-2]))
            buffer[--i] = '\0';
        if (i > 2 && buffer[i-1] == '0' && buffer[i-2] == '.')
            buffer[i -= 2] = '\0';
        arg.assign(buffer);
    }
    else if (lua_istable(l, n))   arg.assign("[table]");
    else if (lua_isthread(l, n))  arg.assign("[thread]");
    else if (lua_isfunction(l, n)) arg.assign("[function]");
    else arg.assign(lua_toboolean(l, n) ? "true" : "false");
}

#define HAVE_LUA_GETEXTRASPACE 0

namespace z8::pico8
{

// ──────────────────────────────────────────────────────────────
// Constructor / destructor
// ──────────────────────────────────────────────────────────────

struct FileReaderData {
#if defined(ARDUINO)
    fs::File file;
#else
    FILE* file = nullptr;
#endif
    // Pre-loaded chunk: uses static buffer to avoid heap allocation
    const char* chunk_data = nullptr;
    int chunk_len = 0;
    int chunk_pos = 0;
    char buffer[512];
    bool in_lua = false;
};

// Preload all Lua code from a .p8 file into data->chunk, then close the file.
// This avoids keeping the file handle and line buffers during lua_load.
static bool preload_lua_code(FileReaderData *data)
{
    // Use static buffer - no heap allocation at all
    static char buf[16384];
    int pos = 0;
    int line_num = 0;

    while (true)
    {
#if defined(ARDUINO)
        if (!data->file || !data->file.available()) break;
        char ch;
        char line_buf[256];
        int line_len = 0;
        while (data->file.available() && line_len < 255)
        {
            ch = data->file.read();
            if (ch == '\n') break;
            if (ch == '\r') continue;
            line_buf[line_len++] = ch;
        }
        line_buf[line_len] = '\0';
#else
        if (!data->file || feof(data->file)) break;
        char line_buf[256];
        if (!fgets(line_buf, sizeof(line_buf), data->file)) break;
        int line_len = (int)strlen(line_buf);
        while (line_len > 0 && (line_buf[line_len-1] == '\r' || line_buf[line_len-1] == '\n'))
            line_buf[--line_len] = '\0';
#endif
        line_num++;

        if (line_num <= 5)
            lol::msg::info("preload line %d: [%s]\n", line_num, line_buf);

        const char *t = line_buf;
        while (*t == ' ' || *t == '\t') t++;

        if (strncmp(t, "__lua__", 7) == 0 && t[7] == '\0')
        {
            lol::msg::info("FOUND __lua__ marker at line %d\n", line_num);
            data->in_lua = true;
            continue;
        }
        if (data->in_lua)
        {
            const char *markers[] = {"__gfx__", "__gff__", "__map__", "__sfx__", "__mus__", "__label__", "__hud__"};
            for (const auto &m : markers)
            {
                size_t mlen = strlen(m);
                if (strncmp(t, m, mlen) == 0 && t[mlen] == '\0')
                {
#if defined(ARDUINO)
                    data->file.close();
#else
                    fclose(data->file);
#endif
                    // Point directly at static buffer - NO heap allocation
                    data->chunk_data = buf;
                    data->chunk_len = pos;
                    data->chunk_pos = 0;
                    lol::msg::info("FOUND end marker %s at line %d, len=%d\n", m, line_num, pos);
                    return true;
                }
            }
        }

        if (!data->in_lua) continue;

        if (pos + line_len + 1 >= (int)sizeof(buf)) {
            lol::msg::error("preload buffer overflow at line %d\n", line_num);
            data->chunk_data = buf;
            data->chunk_len = pos;
            data->chunk_pos = 0;
#if defined(ARDUINO)
            data->file.close();
#else
            fclose(data->file);
#endif
            return true;
        }
        memcpy(buf + pos, line_buf, line_len);
        pos += line_len;
        buf[pos++] = '\n';
    }

    lol::msg::error("preload ended without end marker, len=%d\n", pos);
    data->chunk_data = buf;
    data->chunk_len = pos;
    data->chunk_pos = 0;
#if defined(ARDUINO)
    data->file.close();
#else
    fclose(data->file);
#endif
    return true;
}

static const char* chunk_reader(lua_State *L, void *ud, size_t *size)
{
    FileReaderData *data = (FileReaderData *)ud;
    if (data->chunk_pos >= data->chunk_len)
    {
        *size = 0;
        return nullptr;
    }
    size_t available = data->chunk_len - data->chunk_pos;
    size_t toread = std::min(available, (size_t)sizeof(data->buffer));
    memcpy(data->buffer, data->chunk_data + data->chunk_pos, toread);
    data->chunk_pos += toread;
    *size = toread;
    return data->buffer;
}

vm::vm()
{
    load_config();

    m_bios = std::make_unique<bios>();

// ⭕ 1. 制限つきアロケータを完全にバイパスし、無制限の realloc ラッパーを名実ともに直結！
    m_lua = lua_newstate(baremetal_lua_alloc, nullptr);
    g_active_lua = m_lua;

    if (!m_lua) {
        Serial.println("[ERROR] Failed to create baremetal Lua state!");
    }

    // ⭕ 2. 【最重要：ERROR 4を粉砕する特効薬】
    // わずか30KB〜90KBの過酷なヒープ環境でLuaを動かすため、
    // ガベージコレクション(GC)の「ゴミ拾い頻度」を最大（アグレッシブモード）に強制設定します。
    // これにより、BIOS読み込み中に一時発生するメモリがその場で即座に解放され、容量上限を絶対に超えなくなります。
    lua_gc(m_lua, LUA_GCSETPAUSE, 100);      // 次のGCまでのウェイトを0にする
    lua_gc(m_lua, LUA_GCSETSTEPMUL, 500);   // ゴミを拾う速度を通常の5倍に引き上げる

    // Only load essential Lua libraries to save RAM (skip io, os, package)
    static const struct luaL_Reg lib[] = {
        {"_G", luaopen_base},
        {"coroutine", luaopen_coroutine},  // Required by BIOS: cocreate()
        {"string", luaopen_string},
        {"table", luaopen_table},
        {"math", luaopen_math},
        // {"debug", luaopen_debug},       // Disabled for RAM optimization
        {NULL, NULL}
    };
    for (const struct luaL_Reg *l = lib; l->func != NULL; l++) {
        luaL_requiref(m_lua, l->name, l->func, 1);
        lua_pop(m_lua, 1);
    }

    // Register dummy debug table to satisfy BIOS requirements without full debug library overhead
    luaL_dostring(m_lua, "debug = { getinfo = function() return {} end, traceback = function() return '' end, sethook = function() end, getlocal = function() return nil end }");

    // ⭕ コンパイラの指定通り正しい関数名に
    lua_setpico8memory(m_lua, (const unsigned char*)&m_ram);

    bindings::lua::init(m_lua, this);

#if !defined(ARDUINO)
    lua_sethook(m_lua, &vm::instruction_hook, LUA_MASKCOUNT, 1000);
#endif

    private_init_ram();

    ::memset(m_state.buttons, 0, sizeof(m_state.buttons));
    ::memset(&m_state.mouse,  0, sizeof(m_state.mouse));

    // Compile the BIOS code from memory-mapped flash or file using piece-by-piece reader
    // This is critical for ESP32: luaL_loadbuffer() needs 2-3x the buffer size in RAM
    // while lua_load() with a chunk reader only needs ~128 bytes at a time.
    struct EmbeddedReaderData {
        const char* data;
        size_t pos;
        size_t len;
    };
    
    EmbeddedReaderData embedded_data;
    embedded_data.data = m_bios->get_code();
    embedded_data.pos = 0;
    embedded_data.len = m_bios->get_code_len();
    
    // Use a lambda for the chunk reader to capture embedded_data by reference
    auto embedded_chunk_reader = [](lua_State* L, void* ud, size_t* size) -> const char* {
        EmbeddedReaderData* data = static_cast<EmbeddedReaderData*>(ud);
        (void)L;
        if (data->pos >= data->len)
            return nullptr;
        size_t chunk = 128;
        if (data->len - data->pos < chunk)
            chunk = data->len - data->pos;
        const char* result = data->data + data->pos;
        data->pos += chunk;
        *size = chunk;
        return result;
    };

    lol::msg::info("compiling/loading BIOS (%d bytes, %s)...\n",
                   (int)embedded_data.len,
                   m_bios->is_bytecode() ? "bytecode" : "source");
    // バイトコードなら "b"、Luaソーステキストなら "t" モード
    const char* bios_mode = m_bios->is_bytecode() ? "b" : "t";
    int status = lua_load(m_lua, embedded_chunk_reader, &embedded_data, "@bios", bios_mode);
    if (status == LUA_OK)
    {
        lol::msg::info("BIOS compiled successfully, executing...\n");
        status = lua_pcall(m_lua, 0, LUA_MULTRET, 0);
    }

    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("error %d loading bios: %s\n", status, message);
#if defined(ARDUINO)
        // Dump diagnostic info to help debug "incompatible precompiled chunk"
        Serial.printf("\n=== BIOS LOAD DIAGNOSTICS ===\n");
        Serial.printf("Error: %d, Message: %s\n", status, message ? message : "(null)");
        Serial.printf("BIOS code_len: %d, is_bytecode: %d\n", (int)embedded_data.len, m_bios->is_bytecode());
        
        // Dump the first 20 bytes of the BIOS data (raw)
        Serial.printf("Raw BIOS header dump (first 20 bytes):\n");
        int dump_len = std::min((int)embedded_data.len, 20);
        for (int i = 0; i < dump_len; i++) {
            Serial.printf(" %02X", (uint8_t)embedded_data.data[i]);
            if ((i + 1) % 16 == 0) Serial.println();
        }
        Serial.println();
        
        // Show what the runtime expects as header
        Serial.printf("Runtime expected header:\n");
        Serial.printf(" LUA_SIGNATURE: ");
        for (int i = 0; i < 4; i++) Serial.printf(" %02X", (uint8_t)LUA_SIGNATURE[i]);
        Serial.println();
        Serial.printf(" VERSION byte:  %02X (Lua %s.%s = 0x%02X)\n",
                       0x52, LUA_VERSION_MAJOR, LUA_VERSION_MINOR, 0x52);
        Serial.printf(" FORMAT byte:   %02X\n", 0x00);
        Serial.printf(" LUAC_TAIL:     ");
        const char tail[] = "\x19\x93\r\n\x1a\n";
        for (int i = 0; i < 5; i++) Serial.printf(" %02X", (uint8_t)tail[i]);
        Serial.println();
        
        // If bytecode, parse and display the header fields from the file
        if (m_bios->is_bytecode() && embedded_data.len >= 12) {
            const uint8_t* h = (const uint8_t*)embedded_data.data;
            Serial.printf("\nBytecode header fields from file:\n");
            Serial.printf(" [0..3]  Signature: ");
            for (int i = 0; i < 4; i++) Serial.printf(" %02X('%c')", h[i], (h[i] >= 32 && h[i] < 127) ? h[i] : '.');
            Serial.println();
            Serial.printf(" [4]     Version:   %02X (expected %02X) %s\n", h[4], 0x52, h[4] == 0x52 ? "OK" : "MISMATCH!");
            Serial.printf(" [5]     Format:    %02X (expected %02X) %s\n", h[5], 0x00, h[5] == 0x00 ? "OK" : "MISMATCH!");
            Serial.printf(" [6]     Endian:    %02X (1=little, 0=big)\n", h[6]);
            Serial.printf(" [7]     SzInt:     %u\n", h[7]);
            Serial.printf(" [8]     SzSize:    %u\n", h[8]);
            Serial.printf(" [9]     SzInstruction: %u\n", h[9]);
            Serial.printf(" [10]    IsNumberInt: %u\n", h[10]);
            Serial.printf(" [11]    SzNumber:  %u\n", h[11]);
            Serial.printf(" [12..16] Tail:     ");
            for (int i = 12; i < 18 && i < (int)embedded_data.len; i++) Serial.printf(" %02X", h[i]);
            Serial.println();
        }
        
        Serial.printf("=== END DIAGNOSTICS ===\n\n");
        Serial.flush();
        
        while (1) {
            Serial.printf("*** BIOS LUA ERROR %d: %s\n", status, message ? message : "(null)");
            Serial.flush();
            delay(1000);
        }
#else
        assert(false);
#endif
        if (status != LUA_ERRFILE)
            lua_pop(m_lua, 1);
    }

    // Debug: check if __z8_run_cart was defined
    lua_getglobal(m_lua, "__z8_run_cart");
    if (lua_isfunction(m_lua, -1))
        lol::msg::info("__z8_run_cart is a function after BIOS load\n");
    else if (lua_isnil(m_lua, -1))
        lol::msg::error("__z8_run_cart is nil after BIOS load!\n");
    else
        lol::msg::error("__z8_run_cart has type %d after BIOS load\n", lua_type(m_lua, -1));
    lua_pop(m_lua, 1);

    // Copy the BIOS font graphics from memory-mapped flash
    if (m_bios->get_gfx())
    {
        g_bios_gfx_ptr = m_bios->get_gfx();
    }

    // Apply monkey-patch to override __z8_run_cart for stream loading
    // glue_code 相当のループ制御をコード実行後にインラインで実行
    const char patch_code[] = R"PATCH(
__z8_run_cart = function(dummy)
    __z8_loop = cocreate(function()
        __init_ram()
        reload()
        __z8_reset_state()
        __z8_reset_cartdata()
        local code = __z8_compiled_cart_code
        if not code then
            color(14) print('syntax error: no code')
            error()
        end
        __z8_cart_running = true

        -- XPcall helper to dump stacktrace
        local function safe_call(name, fn, ...)
            if not fn then return end
            local ok, err = xpcall(fn, function(e)
                return tostring(e) .. "\n" .. (debug and debug.traceback and debug.traceback() or "")
            end, ...)
            if not ok then
                if printh then printh("[FATAL " .. name .. "] " .. err) end
                if cls then cls(0) end
                if camera then camera() end
                if cursor then cursor(0,0) end
                if color then color(8) end
                if print then 
                    print("FATAL ERROR IN " .. name)
                    print(sub(err, 1, 150))
                end
                if flip then flip() end
                error(err)
            end
        end

        -- ゲームコードを実行してグローバル関数(_init,_update,_draw等)を定義
        safe_call("LOAD", code)
        
        -- glue_code 相当: _init/_update/_draw のメインループ
        safe_call("INIT", _init)

        if _update or _update60 or _draw then
            while true do
                if _update60 then
                    _update_buttons()
                    _mainloop=_update60
                    _set_mainloop_exists(true)
                    safe_call("UPDATE60", _update60)
                    _mainloop=nil
                    _set_mainloop_exists(false)
                else
                    yield()
                    _update_buttons()
                    if _update then
                        _mainloop=_update
                        _set_mainloop_exists(true)
                        safe_call("UPDATE", _update)
                        _mainloop=nil
                        _set_mainloop_exists(false)
                    end
                end
                if _draw then
                    holdframe()
                    -- Dynamic frameskip: skip draw if C++ signals lag via GPIO (0x5f80)
                    local lag = peek(0x5f80)
                    local should_draw = true
                    if lag == 1 then
                        __frameskip_counter = (__frameskip_counter or 0) + 1
                        if __frameskip_counter % 2 == 1 then
                            should_draw = false
                        end
                    else
                        __frameskip_counter = 0
                    end
                    
                    if should_draw then
                        _mainloop=_draw
                        _set_mainloop_exists(true)
                        safe_call("DRAW", _draw)
                        _mainloop=nil
                        _set_mainloop_exists(false)
                        flip()
                    end
                else
                    yield()
                end
            end
        end
    end)
end
)PATCH";
    int status_patch = luaL_dostring(m_lua, patch_code);
    if (status_patch != LUA_OK)
    {
        lol::msg::error("Failed to apply VM monkey patch: %s\n", lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
    }
    else
    {
        lol::msg::info("VM monkey patch applied successfully!\n");
    }

    // ── PICO-8 compatibility aliases (MUST be after BIOS + monkey-patch) ──
    // tostr -> tostring (PICO-8 uses tostr() instead of tostring())
    lua_getglobal(m_lua, "tostring");
    lua_setglobal(m_lua, "tostr");

    // flr -> math.floor (PICO-8 uses flr() instead of math.floor())
    lua_getglobal(m_lua, "math");
    lua_getfield(m_lua, -1, "floor");
    lua_setglobal(m_lua, "flr");
    lua_pop(m_lua, 1); // pop math table

    // ── PICO-8 arithmetic API batch bindings ──
    // PICO-8 calls sin(), cos(), atan2(), sqrt(), abs(), ceil() globally
    // instead of math.sin(), math.cos(), etc.
    {
        struct { const char* name; const char* field; } math_map[] = {
            {"cos", "cos"}, {"sin", "sin"}, {"atan2", "atan2"}, {"sqrt", "sqrt"},
            {"ceil", "ceil"}
        };
        for (auto const& entry : math_map) {
            lua_getglobal(m_lua, "math");
            lua_getfield(m_lua, -1, entry.field);
            lua_setglobal(m_lua, entry.name);
            lua_pop(m_lua, 1); // pop math table
        }
    }

    // ── PICO-8 bit manipulation functions ──
    // shr(x, n) - right shift
    lua_pushcfunction(m_lua, [](lua_State* L) {
        int x = (int)luaL_checkinteger(L, 1);
        int n = (int)luaL_checkinteger(L, 2);
        lua_pushinteger(L, (lua_Integer)((unsigned)x >> n));
        return 1;
    });
    lua_setglobal(m_lua, "shr");

    // shl(x, n) - left shift
    lua_pushcfunction(m_lua, [](lua_State* L) {
        int x = (int)luaL_checkinteger(L, 1);
        int n = (int)luaL_checkinteger(L, 2);
        lua_pushinteger(L, (lua_Integer)(x << n));
        return 1;
    });
    lua_setglobal(m_lua, "shl");

    // band(x, y) - bitwise AND
    lua_pushcfunction(m_lua, [](lua_State* L) {
        lua_pushinteger(L, luaL_checkinteger(L, 1) & luaL_checkinteger(L, 2));
        return 1;
    });
    lua_setglobal(m_lua, "band");

    // bor(x, y) - bitwise OR
    lua_pushcfunction(m_lua, [](lua_State* L) {
        lua_pushinteger(L, luaL_checkinteger(L, 1) | luaL_checkinteger(L, 2));
        return 1;
    });
    lua_setglobal(m_lua, "bor");

    // bxor(x, y) - bitwise XOR
    lua_pushcfunction(m_lua, [](lua_State* L) {
        lua_pushinteger(L, luaL_checkinteger(L, 1) ^ luaL_checkinteger(L, 2));
        return 1;
    });
    lua_setglobal(m_lua, "bxor");

    // mid(x, minv, maxv) - clamp value to range
    lua_pushcfunction(m_lua, [](lua_State* L) {
        lua_Number x = lua_tonumber(L, 1);
        lua_Number minv = lua_tonumber(L, 2);
        lua_Number maxv = lua_tonumber(L, 3);
        if (x < minv) x = minv;
        if (x > maxv) x = maxv;
        lua_pushnumber(L, x);
        return 1;
    });
    lua_setglobal(m_lua, "mid");

    // sgn(x) - sign of x
    lua_pushcfunction(m_lua, [](lua_State* L) {
        fix32 x = fix32(lua_tonumber(L, 1));
        fix32 zero = fix32::frombits(0);
        fix32 neg_one = fix32::frombits((int32_t)-65536);
        fix32 pos_one = fix32::frombits((int32_t)65536);
        if (x == zero) lua_pushnumber(L, lua_Number(zero));
        else if (x < zero) lua_pushnumber(L, lua_Number(neg_one));
        else lua_pushnumber(L, lua_Number(pos_one));
        return 1;
    });
    lua_setglobal(m_lua, "sgn");

    // abs(x) - absolute value
    lua_pushcfunction(m_lua, [](lua_State* L) {
        fix32 x = fix32::abs(fix32(lua_tonumber(L, 1)));
        lua_pushnumber(L, lua_Number(x));
        return 1;
    });
    lua_setglobal(m_lua, "abs");

    // ── PICO-8 nil-guard polyfill (SAFE: no native bitwise operators) ──
    // PICO-8 returns 0 for undefined/out-of-bounds accesses instead of nil.
    // This polyfill wraps all getter functions to force nil→0 conversion.
    // IMPORTANT: Avoids &, |, <<, >> operators which cause syntax errors in Lua 5.2.
    const char* pico8_polyfill = R"lua(
-- (0) Dummy debug table for compatibility
debug = debug or {
    getinfo = function() return {} end,
    traceback = function() return "" end,
    sethook = function() end,
    getlocal = function() return nil end,
}

-- (1) Safe arithmetic helpers
tostr = function(x) return tostring(x or "") end
tonum = function(x) return tonumber(x) or 0 end
flr = function(x) return math.floor(x or 0) end
ceil = function(x) return math.ceil(x or 0) end
abs = function(x) return math.abs(x or 0) end
sqrt = function(x) return math.sqrt(math.max(x or 0, 0)) end
max = function(a,b) return math.max(a or 0, b or 0) end
min = function(a,b) return math.min(a or 0, b or 0) end
time = time or function() return 0 end
t = time

cos = function(x) return math.cos((x or 0) * 6.2831853) end
sin = function(x) return -math.sin((x or 0) * 6.2831853) end
atan2 = function(dx,dy) return math.atan2(dy or 0, dx or 0) / 6.2831853 end
sgn = function(x) return (x and x < 0) and -1 or 1 end
mid = function(a,b,c) return max(min(max(a or 0, b or 0), c or 0), min(a or 0, b or 0)) end

-- (2) Bit manipulation: keep existing C bindings, just add nil-guards
-- Using 'or' pattern to avoid overwriting the C functions defined above
band = band or function(a,b) return 0 end
bor = bor or function(a,b) return 0 end
bxor = bxor or function(a,b) return 0 end
bnot = bnot or function(a) return 0 end
shl = shl or function(a,b) return 0 end
shr = shr or function(a,b) return 0 end

split = function(s, sep)
    local tbl = {}
    if type(s) ~= "string" then return tbl end
    sep = sep or ","
    for str in string.gmatch(s, "([^"..sep.."]+)") do
        local n = tonumber(str)
        table.insert(tbl, n ~= nil and n or str)
    end
    return tbl
end

-- (3) Safe wrapper for stat (which can return nil)
local _orig_stat = stat
stat = function(...)
    if not _orig_stat then return 0 end
    local r = _orig_stat(...)
    return r == nil and 0 or r
end

local _orig_rnd = rnd
rnd = function(x)
    if type(x) == "table" then
        local c = #x
        if c == 0 then return nil end
        local idx = flr((_orig_rnd and _orig_rnd(c) or (math.random() * c))) + 1
        return x[idx]
    end
    if _orig_rnd then
        if x == nil then return _orig_rnd(1) end
        local r = _orig_rnd(x)
        return r == nil and 0 or r
    end
    return math.random() * (x or 1)
end

-- (4) Removed API stub injection to restore original functionality for standard cartridges
)lua";

    // Execute polyfill with error monitoring
    int polyfill_status = luaL_dostring(m_lua, pico8_polyfill);
    if (polyfill_status != LUA_OK)
    {
        lol::msg::error("PICO-8 polyfill LOAD FAILED: %s\n", lua_tostring(m_lua, -1));
#if defined(ARDUINO)
        Serial.printf("[ERROR] PICO-8 polyfill LOAD FAILED: %s\n", lua_tostring(m_lua, -1));
        Serial.flush();
#endif
        lua_pop(m_lua, 1);
    }
    else
    {
        lol::msg::info("[SUCCESS] PICO-8 polyfill applied successfully!\n");
#if defined(ARDUINO)
        Serial.println("[SUCCESS] PICO-8 polyfill applied successfully!");
        Serial.flush();
#endif
    }

    //
    lua_gc(m_lua, LUA_GCCOLLECT, 0);
#if defined(ARDUINO)
    Serial.printf("[DEBUG] vm constructor finished: free heap = %u bytes\n", (unsigned int)ESP.getFreeHeap());
#endif
}

vm::~vm()
{
    save(true);
    if (g_active_lua == m_lua) {
        g_active_lua = nullptr;
    }
    lua_close(m_lua);
}

// ──────────────────────────────────────────────────────────────
// Accessors
// ──────────────────────────────────────────────────────────────

// ⭕ Static buffers to minimize heap allocation for code bridge
static char g_code_bridge[16384];
static std::string g_code_bridge_str;
static std::string g_mutable_code_bridge_str;

std::string const &vm::get_code() const
{
    const std::string& code = m_cart.get_code();
    size_t len = code.size();
    if (len >= sizeof(g_code_bridge)) len = sizeof(g_code_bridge) - 1;
    memcpy(g_code_bridge, code.data(), len);
    g_code_bridge[len] = '\0';
    g_code_bridge_str.assign(g_code_bridge);
    return g_code_bridge_str;
}

std::string &vm::get_mutable_code()
{
    const std::string& code = m_cart.get_mutable_code();
    size_t len = code.size();
    if (len >= sizeof(g_code_bridge)) len = sizeof(g_code_bridge) - 1;
    memcpy(g_code_bridge, code.data(), len);
    g_code_bridge[len] = '\0';
    g_mutable_code_bridge_str.assign(g_code_bridge);
    return g_mutable_code_bridge_str;
}


u4mat2<128,128> const &vm::get_front_screen() const
{
    return get_current_screen();
}

u4mat2<128,128> const &vm::get_current_screen() const
{
    // Multiscreen disabled for memory savings
    return m_ram.hw_state.mapping_screen == 0 ? m_ram.gfx : m_ram.screen;
}

u4mat2<128,128> &vm::get_current_screen()
{
    return const_cast<u4mat2<128,128> &>(static_cast<const vm &>(*this).get_current_screen());
}

lol::ivec2 vm::get_screen_resolution() const
{
    // Multiscreen disabled; always 128x128
    return lol::ivec2(128, 128);
}

std::tuple<uint8_t *, size_t> vm::ram()
{
    return std::make_tuple(&m_ram[0], sizeof(m_ram));
}

std::tuple<uint8_t *, size_t> vm::rom()
{
    auto &rom = m_cart.get_rom();
    return std::make_tuple((uint8_t*)&rom[0], rom.size());
}

// ──────────────────────────────────────────────────────────────
// Lua hooks
// ──────────────────────────────────────────────────────────────

void vm::runtime_error(std::string str)
{
    luaL_error(m_sandbox_lua, str.c_str());
}

int vm::panic_hook(lua_State *l)
{
    char const *message = lua_tostring(l, -1);
    lol::msg::error("Lua panic: %s\n", message);
#if defined(ARDUINO)
    Serial.printf("\n*** LUA PANIC: %s ***\n", message ? message : "(null)");
    Serial.flush();
    while (1) { delay(1000); }
#else
    assert(false);
#endif
    return 0;
}

void vm::instruction_hook(lua_State *l, lua_Debug *)
{
    lua_getglobal(l, "\x01");
    vm *that = (vm *)lua_touserdata(l, -1);
    lua_remove(l, -1);

    that->m_instructions += 1000;
    if (that->m_instructions >= that->m_max_instructions)
    {
        lua_getglobal(l, "__z8_is_inside_main_loop");
        bool is_inside_loop = lua_toboolean(l, -1);

        lua_yield(l, 0);
        if (!is_inside_loop)
            that->private_buttons();
    }
}

// ──────────────────────────────────────────────────────────────
// RAM initialisation
// ──────────────────────────────────────────────────────────────

void vm::private_init_ram()
{
    ::memset(&m_ram, 0, sizeof(m_ram));

    m_ram.hw_state.mapping_screen   = 0x60;
    m_ram.hw_state.mapping_map      = 0x20;
    m_ram.hw_state.mapping_map_width= 0x80;

#if defined(ARDUINO)
    api_srand(fix32::frombits((int32_t)millis()));
    m_timer_last_ms = millis();
#else
    api_srand(fix32::frombits((int32_t)time(nullptr)));
    m_timer_last_ms = 0;
#endif
    m_time = 0;
}

// ──────────────────────────────────────────────────────────────
// Cart loading / saving
// ──────────────────────────────────────────────────────────────

bool file_exists(std::string const &filepath)
{
#if defined(ARDUINO)
    return SD.exists(filepath.c_str());
#else
    FILE *f = fopen(filepath.c_str(), "r");
    if (f) { fclose(f); return true; }
    return false;
#endif
}

static void make_dir(std::string const &path)
{
#if defined(ARDUINO)
    SD.mkdir(path.c_str());
#else
    // best-effort on desktop (for testing)
#endif
}

bool vm::private_load(std::string name, opt<std::string> breadcrumb, opt<std::string> params)
{
    save(true);

    std::string previous_cart = m_cart.get_filename();

    // If name starts with '/', it's an absolute path — use as-is
    // Otherwise, prepend the active directory
    if (name.empty() || name[0] != '/')
        name = get_path_active_dir() + "/" + name;

    if (!lol::ends_with(lol::tolower(name), ".p8") &&
        !lol::ends_with(lol::tolower(name), ".png"))
        name += ".p8";

    if (!load_cart(m_cart, name))
        return false;

    if (breadcrumb.has_value() && (*breadcrumb).length() > 1)
    {
        breadcrumb_path bc;
        strncpy(bc.cart_path, previous_cart.c_str(), sizeof(bc.cart_path) - 1);
        bc.cart_path[sizeof(bc.cart_path) - 1] = '\0';
        strncpy(bc.title, breadcrumb->c_str(), sizeof(bc.title) - 1);
        bc.title[sizeof(bc.title) - 1] = '\0';
        strncpy(bc.params, params.has_value() ? params->c_str() : "", sizeof(bc.params) - 1);
        bc.params[sizeof(bc.params) - 1] = '\0';
        if (breadcrumbs_count < MAX_BREADCRUMBS)
            breadcrumbs_buf[breadcrumbs_count++] = bc;
    }

    run();
    return true;
}

void vm::load(std::string const &name)
{
    save(true);
    set_path_active_dir(name);
    load_cart(m_cart, name);
}

bool vm::load_cart(cart &target_cart, std::string const &filename)
{
    bool is_pc8c_file = lol::ends_with(lol::tolower(filename), ".pc8c") || lol::ends_with(lol::tolower(filename), ".p8c");
    if (is_pc8c_file)
    {
#if defined(ARDUINO)
        std::string full_sd_path = filename;
        if (full_sd_path.rfind("/sd", 0) != 0) {
            full_sd_path = "/sd" + full_sd_path;
        }
        FILE* f = fopen(full_sd_path.c_str(), "rb");
        if (f)
        {
            char magic[4];
            if (fread(magic, 1, 4, f) == 4 && strncmp(magic, "PC8C", 4) == 0)
            {
                fseek(f, 32, SEEK_CUR); // skip name
                uint32_t rom_size = 0;
                fread(&rom_size, 1, 4, f);
                
                fread(&m_ram, 1, rom_size > sizeof(m_ram) ? sizeof(m_ram) : rom_size, f);
                uint32_t copy_size = rom_size > 17408 ? 17408 : rom_size;
                ::memcpy(target_cart.get_rom().data(), &m_ram, copy_size);
                
                target_cart.init_filename(filename);
                fclose(f);
                Serial.printf("[DEBUG] SD PC8C load: ROM loaded (%u bytes)\n", rom_size);
                return true;
            }
            fclose(f);
        }
        Serial.printf("[WARN] SD PC8C load failed for: %s\n", full_sd_path.c_str());
        return false;
#else
        return false;
#endif
    }

#if defined(ARDUINO)
    Serial.printf("[DEBUG] load_cart start: free heap = %u bytes\n", (unsigned int)ESP.getFreeHeap());
    if (m_lua)
    {
        lua_gc(m_lua, LUA_GCCOLLECT, 0);
        Serial.printf("[DEBUG] load_cart after GC: free heap = %u bytes\n", (unsigned int)ESP.getFreeHeap());
    }

    // Try loading from flash partition cache first
    if (target_cart.load_from_partition(filename))
    {
        Serial.printf("[DEBUG] Loaded from flash partition cache\n");
        return true;
    }
#endif

    // Fall back to SD card load
    bool has_loaded = target_cart.load(filename);
    if (has_loaded)
    {
#if defined(ARDUINO)
        // Cache to flash partition for faster future loads
        // Only attempt if enough heap is available (need ~2x file size for string + vector)
        if (ESP.getFreeHeap() > 64 * 1024)
            target_cart.save_to_partition();
        else
            lol::msg::info("Skipping partition cache (low heap: %u bytes)\n", (unsigned)ESP.getFreeHeap());
#endif
        std::string name_cstore = get_path_cstore(filename);
        if (file_exists(name_cstore))
        {
            auto reload_cart = std::make_shared<cart>();
            reload_cart->load(name_cstore);
            target_cart.set_from_ram(reload_cart->get_rom().data(), 0, 0, 0x4300);
        }
    }
    return has_loaded;
}

bool vm::save_cart(cart &target_cart, std::string const &filename)
{
    std::string name_cstore = get_path_cstore(filename);
    return target_cart.save(name_cstore);
}

// ──────────────────────────────────────────────────────────────
// Run / step
// ──────────────────────────────────────────────────────────────

void vm::reset()
{
    load(m_cart.get_filename());
    run();
}

void vm::run()
{
    m_auto_run = true;
}

bool vm::step(float /* seconds */)
{
    if (m_auto_run)
    {
        m_auto_run = false;
        api_run();
    }

#if defined(ARDUINO)
    uint32_t now_ms = millis();
    if (!m_in_pause)
        m_time += (now_ms - m_timer_last_ms) / 1000.0;
    m_timer_last_ms = now_ms;
#else
    // On desktop, seconds is passed by the caller
    if (!m_in_pause)
        m_time += 1.0 / 30.0; // assume 30fps for simplicity
#endif

    if (m_exit_requested)
    {
        save(true);
        m_is_running = false;
        return false;
    }

    bool ret = false;
    lua_gc(m_lua, LUA_GCCOLLECT, 0);
    lua_getglobal(m_lua, "__z8_tick");
    int status = lua_pcall(m_lua, 0, 1, 0);
    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        static unsigned long last_error_time = 0;
#if defined(ARDUINO)
        unsigned long now = millis();
#else
        unsigned long now = 0; // fallback for desktop
#endif
        if (now - last_error_time > 1000)
        {
            last_error_time = now;
            lol::msg::error("error %d in main loop: %s\n", status, message ? message : "(null)");
        }
    }
    else
    {
        ret = (int)lua_tonumber(m_lua, -1) >= 0;
    }
    lua_pop(m_lua, 1);

    m_instructions = 0;

    save(false);

    return ret;
}

// ──────────────────────────────────────────────────────────────
// Input
// ──────────────────────────────────────────────────────────────

void vm::button(int player, int index, int state_)
{
    m_state.buttons[1][player * 8 + index] += state_;
}

void vm::mouse(lol::ivec2 coords, lol::ivec2 relative, int buttons, int scroll)
{
    m_state.mouse.x = (double)coords.x;
    m_state.mouse.y = (double)coords.y;
    m_state.mouse.b = (double)buttons;
    m_state.mouse.s[0] = (double)scroll;

    bool has_button_pressed = buttons > 0 && m_state.mouse.lb != m_state.mouse.b;
    if (has_button_pressed) m_state.mouse.ac = 4;
    else
    {
        double dx = (double)relative.x - (double)m_state.mouse.rx;
        double dy = (double)relative.y - (double)m_state.mouse.ry;
        m_state.mouse.ac = (int)std::min(std::abs(dx) + std::abs(dy), 3.0);
    }

    m_state.mouse.rx = (double)relative.x;
    m_state.mouse.ry = (double)relative.y;

    if (m_ram.draw_state.mouse_flags.buttons)
    {
        m_state.buttons[1][5] += (buttons & 0x1) ? 1 : 0;
        m_state.buttons[1][4] += (buttons & 0x2) ? 1 : 0;
        m_state.buttons[1][6] += (buttons & 0x4) ? 1 : 0;
    }
}

void vm::text(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        ch = '\x80' + (ch - 'A');
    m_state.kbd.chars[m_state.kbd.stop] = ch;
    m_state.kbd.stop = (m_state.kbd.stop + 1) % (int)sizeof(m_state.kbd.chars);
}

void vm::sixaxis(lol::vec3 angle)
{
    m_state.rotation.x = angle.x;
    m_state.rotation.y = angle.y;
    m_state.rotation.z = angle.z;
}

void vm::axis(int player, float valueX, float valueY)
{
    m_state.axes[player][0] = valueX;
    m_state.axes[player][1] = valueY;
}

// ──────────────────────────────────────────────────────────────
// System API
// ──────────────────────────────────────────────────────────────




void vm::api_run()
{
    save(true);

    ::memset(m_state.buttons, 0, sizeof(m_state.buttons));
    ::memset(&m_state.mouse,  0, sizeof(m_state.mouse));

    m_ram.draw_state.misc_features.multi_screen = false;
    // Multiscreen disabled; no need to set x/y

    /* Ensure __z8_run_cart is defined — if not, execute BIOS bytecode first */
    lua_getglobal(m_lua, "__z8_run_cart");
    bool run_cart_defined = !lua_isnil(m_lua, -1);
    lua_pop(m_lua, 1);

    if (!run_cart_defined)
    {
#if defined(ARDUINO)
        Serial.println("[DEBUG] __z8_run_cart not defined, loading BIOS partition to init globals...");
        const esp_partition_t* bios_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bios");
        if (bios_part)
        {
            spi_flash_mmap_handle_t bios_map_handle;
            const void* bios_map_ptr = nullptr;
            esp_err_t berr = esp_partition_mmap(bios_part, 0, bios_part->size,
                                                SPI_FLASH_MMAP_DATA, &bios_map_ptr, &bios_map_handle);
            if (berr == ESP_OK)
            {
                const char* bbase = (const char*)bios_map_ptr;
                if (strncmp(bbase, "PC8C", 4) == 0)
                {
                    // New format: magic(4) + name(32) + rom_size(4) + ROM + bc_size(4) + bytecode
                    uint32_t brom_size = *(const uint32_t*)(bbase + 36);
                    uint32_t bbc_size  = *(const uint32_t*)(bbase + 40 + brom_size);
                    const char* bbc   = bbase + 40 + brom_size + 4;

                    struct BMR { const char* ptr; uint32_t rem; char buf[256]; };
                    BMR bmr; bmr.ptr = bbc; bmr.rem = bbc_size;
                    auto bios_reader = [](lua_State*, void* ud, size_t* sz) -> const char* {
                        BMR* r = (BMR*)ud;
                        if (!r->rem) { *sz = 0; return nullptr; }
                        uint32_t c = r->rem < sizeof(r->buf) ? r->rem : sizeof(r->buf);
                        memcpy(r->buf, r->ptr, c); r->ptr += c; r->rem -= c;
                        *sz = c; return r->buf;
                    };
                    int bst = lua_load(m_lua, bios_reader, &bmr, "@bios", "b");
                    spi_flash_munmap(bios_map_handle);
                    if (bst == LUA_OK)
                    {
                        bst = lua_pcall(m_lua, 0, 0, 0);
                        if (bst != LUA_OK)
                        {
                            const char* emsg = lua_tostring(m_lua, -1);
                            Serial.printf("[ERROR] BIOS init pcall failed: %s\n", emsg ? emsg : "(null)");
                            lua_pop(m_lua, 1);
                            m_is_running = false;
                            return;
                        }
                        Serial.println("[DEBUG] BIOS globals initialized.");
                    }
                    else
                    {
                        const char* emsg = lua_tostring(m_lua, -1);
                        Serial.printf("[ERROR] BIOS bytecode load failed: %s\n", emsg ? emsg : "(null)");
                        lua_pop(m_lua, 1);
                        m_is_running = false;
                        return;
                    }
                }
                else
                {
                    spi_flash_munmap(bios_map_handle);
                    Serial.println("[ERROR] BIOS partition: bad magic (old format?)");
                    m_is_running = false;
                    return;
                }
            }
            else
            {
                Serial.printf("[ERROR] BIOS partition mmap failed: 0x%x\n", berr);
                m_is_running = false;
                return;
            }
        }
        else
        {
            Serial.println("[ERROR] BIOS partition not found!");
            m_is_running = false;
            return;
        }
#else
        lol::msg::error("__z8_run_cart is nil in m_lua!\n");
        m_is_running = false;
        return;
#endif
    }

    m_sandbox_lua = m_lua;

    // ── ゲームバイトコードのロード ─────────────────────────────
    int compile_status = LUA_OK;

#if defined(ARDUINO)
    // 最大限の GC を事前に実行
    lua_gc(m_lua, LUA_GCCOLLECT, 0);
    lua_gc(m_lua, LUA_GCCOLLECT, 0);
    Serial.printf("[DEBUG] api_run start. Free heap=%u MaxFree=%u\n",
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)ESP.getMaxAllocHeap());

    Serial.println("=== LISTING ALL PARTITIONS ===");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t* p = esp_partition_get(it);
        Serial.printf("  Name: %s, Type: 0x%02x, SubType: 0x%02x, Offset: 0x%08x, Size: 0x%08x\n",
                      p->label, p->type, p->subtype, (unsigned int)p->address, (unsigned int)p->size);
        it = esp_partition_next(it);
    }
    Serial.println("==============================");

    bool game_loaded = false;
    // game パーティションから mmap を試みる
    const esp_partition_t* game_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "game");

    bool is_bios = (m_cart.get_filename() == "/bios.p8" || m_cart.get_filename() == "bios.p8");
    bool is_pc8c = lol::ends_with(lol::tolower(m_cart.get_filename()), ".pc8c") ||
                  lol::ends_with(lol::tolower(m_cart.get_filename()), ".p8c");

    auto try_mmap_load = [&](uint32_t expected_bc_size) -> bool {
        if (!game_part || is_bios) return false;
        
        spi_flash_mmap_handle_t game_map_handle;
        const void* game_map_ptr = nullptr;
        esp_err_t err = esp_partition_mmap(game_part, 0, game_part->size,
                                           SPI_FLASH_MMAP_DATA, &game_map_ptr, &game_map_handle);
        if (err == ESP_OK)
        {
            const char* base = (const char*)game_map_ptr;
            if (strncmp(base, "PC8C", 4) == 0)
            {
                char cached_name[33];
                memcpy(cached_name, base + 4, 32);
                cached_name[32] = '\0';

                std::string req_base = m_cart.get_filename();
                size_t last_slash = req_base.find_last_of('/');
                if (last_slash != std::string::npos) {
                    req_base = req_base.substr(last_slash + 1);
                }

                // Remove extensions from both to allow matching e.g. .pc8c from SD with .p8 in flash
                size_t req_dot = req_base.find_last_of('.');
                if (req_dot != std::string::npos) {
                    req_base = req_base.substr(0, req_dot);
                }

                std::string cached_base = cached_name;
                size_t cached_dot = cached_base.find_last_of('.');
                if (cached_dot != std::string::npos) {
                    cached_base = cached_base.substr(0, cached_dot);
                }

                if (req_base == cached_base)
                {
                    uint32_t rom_size = *(const uint32_t*)(base + 36);
                    uint32_t bc_size  = *(const uint32_t*)(base + 40 + rom_size);

                    if (expected_bc_size != 0 && bc_size != expected_bc_size)
                    {
                        Serial.printf("[DEBUG] game partition PC8C size mismatch: cached bc=%u, expected=%u\n", bc_size, expected_bc_size);
                        spi_flash_munmap(game_map_handle);
                        return false;
                    }

                    const char* bc    = base + 40 + rom_size + 4;

                    Serial.printf("[DEBUG] game partition PC8C matched: %s, rom=%u bc=%u\n", cached_name, rom_size, bc_size);

                    // ROM全体 (GFX, MAP, GFF, MUSIC, SFX) を PICO-8 RAM とカートリッジ ROM に展開
                    if (rom_size > sizeof(m_ram)) {
                        rom_size = sizeof(m_ram);
                    }
                    ::memcpy(&m_ram, base + 40, rom_size);
                    uint32_t copy_size = rom_size > 17408 ? 17408 : rom_size;
                    ::memcpy(m_cart.get_rom().data(), base + 40, copy_size);

                    // mmap された bytecode を lua_load で直接ロード（コピーなし！）
                    struct MmapReader {
                        const char* ptr;
                        uint32_t    remaining;
                        char        buf[256];
                    };
                    MmapReader mr;
                    mr.ptr       = bc;
                    mr.remaining = bc_size;

                    auto mmap_reader = [](lua_State*, void* ud, size_t* size) -> const char* {
                        MmapReader* r = (MmapReader*)ud;
                        if (r->remaining == 0) { *size = 0; return nullptr; }
                        uint32_t chunk = r->remaining < sizeof(r->buf) ? r->remaining : sizeof(r->buf);
                        memcpy(r->buf, r->ptr, chunk);
                        r->ptr       += chunk;
                        r->remaining -= chunk;
                        *size = chunk;
                        return r->buf;
                    };

                    int old_stepmul = lua_gc(m_lua, LUA_GCSETSTEPMUL, 2000);
                    int old_pause   = lua_gc(m_lua, LUA_GCSETPAUSE, 25);
                    compile_status = lua_load(m_lua, mmap_reader, &mr, "@game", "b");
                    lua_gc(m_lua, LUA_GCSETSTEPMUL, old_stepmul);
                    lua_gc(m_lua, LUA_GCSETPAUSE,   old_pause);
                    Serial.printf("[DEBUG] game bytecode load status=%d. Free heap=%u\n",
                                  compile_status, (unsigned int)ESP.getFreeHeap());
                    // Force GC after mmap bytecode load
                    if (compile_status == LUA_OK) {
                        lua_gc(m_lua, LUA_GCCOLLECT, 0);
                        Serial.printf("[DEBUG] GC after mmap load. Free heap=%u\n", (unsigned)ESP.getFreeHeap());
                    }
                    spi_flash_munmap(game_map_handle);
                    return (compile_status == LUA_OK);
                }
                else
                {
                    Serial.printf("[DEBUG] game partition PC8C mismatch: '%s' != '%s', bypass partition load\n",
                                  cached_name, req_base.c_str());
                }
            }
            else
            {
                Serial.printf("[DEBUG] game partition magic mismatch: %02X%02X%02X%02X\n",
                              (uint8_t)base[0], (uint8_t)base[1],
                              (uint8_t)base[2], (uint8_t)base[3]);
            }

            // 重要：mmapされたメモリの読み込み(lua_load)は完了したので、マッピングを解放してTLB枯渇を防ぐ
            spi_flash_munmap(game_map_handle);
        }
        else
        {
            Serial.printf("[WARN] game partition mmap failed: 0x%x\n", err);
        }
        return false;
    };

    uint32_t expected_bc_size = 0;
    if (is_pc8c)
    {
#if defined(ARDUINO)
        std::string full_sd_path = m_cart.get_filename();
        if (full_sd_path.rfind("/sd", 0) != 0) {
            full_sd_path = "/sd" + full_sd_path;
        }
        FILE* f = fopen(full_sd_path.c_str(), "rb");
        if (f)
        {
            char magic[4];
            if (fread(magic, 1, 4, f) == 4 && strncmp(magic, "PC8C", 4) == 0)
            {
                fseek(f, 32, SEEK_CUR); // skip name
                uint32_t rom_size = 0;
                fread(&rom_size, 1, 4, f);
                fseek(f, rom_size, SEEK_CUR); // skip rom
                fread(&expected_bc_size, 1, 4, f);
            }
            fclose(f);
        }
#endif
    }

    game_loaded = try_mmap_load(expected_bc_size);

    if (is_pc8c && !game_loaded)
    {
#if defined(ARDUINO)
        if (game_part) {
            std::string full_sd_path = m_cart.get_filename();
            if (full_sd_path.rfind("/sd", 0) != 0) {
                full_sd_path = "/sd" + full_sd_path;
            }
            FILE* f = fopen(full_sd_path.c_str(), "rb");
            if (f)
            {
                char magic[4];
                if (fread(magic, 1, 4, f) == 4 && strncmp(magic, "PC8C", 4) == 0)
                {
                    Serial.println("[DEBUG] Copying SD PC8C directly to game partition...");
                    fseek(f, 0, SEEK_END);
                    long total_size = ftell(f);
                    fseek(f, 0, SEEK_SET);

                    esp_err_t er = esp_partition_erase_range(game_part, 0, game_part->size);
                    if (er == ESP_OK)
                    {
                        char write_buf[512];
                        uint32_t offset = 0;
                        bool write_ok = true;
                        while (offset < total_size)
                        {
                            size_t to_read = (total_size - offset) < sizeof(write_buf) ? (total_size - offset) : sizeof(write_buf);
                            size_t r = fread(write_buf, 1, to_read, f);
                            if (r == 0) break;
                            esp_err_t werr = esp_partition_write(game_part, offset, write_buf, r);
                            if (werr != ESP_OK) {
                                Serial.printf("[ERROR] SD to flash copy failed at offset %u: 0x%x\n", offset, werr);
                                write_ok = false;
                                break;
                            }
                            offset += r;
                        }
                        if (write_ok) {
                            Serial.printf("[DEBUG] Copy success. Copied %u bytes to game partition.\n", offset);
                        }
                    }
                    else
                    {
                        Serial.printf("[ERROR] Failed to erase game partition: 0x%x\n", er);
                    }
                }
                fclose(f);

                // コピー完了後、gameパーティションから再ロード
                game_loaded = try_mmap_load(expected_bc_size);
            }
        }
#else
        // Desktop
#endif
    }
    else
    {
        if (is_bios) {
            Serial.println("[DEBUG] bios.p8 requested, skipping game partition check.");
        } else {
            if (!game_loaded) {
                Serial.println("[WARN] 'game' partition not found.");
            }
        }
    }

    if (!game_loaded && !is_pc8c)
    {
        // ── SDカードの.p8をオンデバイスコンパイル → gameパーティションにキャッシュ書き込み ──
        std::string sd_path = m_cart.get_filename(); // e.g. "/jelpi.p8"
        Serial.printf("[DEBUG] On-device compile from SD: %s\n", sd_path.c_str());

        static const size_t ROM_SIZE = 17408;
        uint8_t* rom_dest = (uint8_t*)&m_ram;
        ::memset(rom_dest, 0, ROM_SIZE);
        bool parse_success = false;

        // --- パス1: ROMデータのパース ---
        std::string full_sd_path = sd_path;
        if (full_sd_path.rfind("/sd", 0) != 0) {
            full_sd_path = "/sd" + full_sd_path;
        }
        FILE* sdfile = fopen(full_sd_path.c_str(), "r");
        if (!sdfile) {
            Serial.printf("[WARN] Cannot open SD file %s (full: %s)\n", sd_path.c_str(), full_sd_path.c_str());
        } else {
            enum class Sec { NONE, LUA, GFX, MAP, GFF, MUSIC, SFX, OTHER };
            Sec sec = Sec::NONE;
            bool hdr_done = false;
            size_t gfx_off=0, map_off=0x2000, gff_off=0x3000, sfx_off=0x3200, music_pattern_id=0, sfx_index=0;
            char linebuf[512];
            auto hexval = [](char c) -> int {
                if (c>='0'&&c<='9') return c-'0';
                if (c>='a'&&c<='f') return c-'a'+10;
                if (c>='A'&&c<='F') return c-'A'+10;
                return -1;
            };
            while (fgets(linebuf, sizeof(linebuf), sdfile)) {
                size_t ll = strlen(linebuf);
                while (ll>0 && (linebuf[ll-1]=='\n'||linebuf[ll-1]=='\r')) linebuf[--ll]='\0';
                if (!hdr_done) {
                    if (strncmp(linebuf,"version ",8)==0) hdr_done=true;
                    continue;
                }
                if (strcmp(linebuf,"__lua__")==0)   { sec=Sec::LUA; continue; }
                if (strcmp(linebuf,"__gfx__")==0)   { sec=Sec::GFX; continue; }
                if (strcmp(linebuf,"__map__")==0)   { sec=Sec::MAP; continue; }
                if (strcmp(linebuf,"__gff__")==0)   { sec=Sec::GFF; continue; }
                if (strcmp(linebuf,"__music__")==0) { sec=Sec::MUSIC; continue; }
                if (strcmp(linebuf,"__sfx__")==0)   { sec=Sec::SFX; continue; }
                if (strncmp(linebuf,"__",2)==0&&ll>=4&&linebuf[ll-1]=='_'&&linebuf[ll-2]=='_') {
                    sec=Sec::OTHER; continue;
                }
                if (sec == Sec::LUA) {
                    continue;
                } else if (sec != Sec::NONE && sec != Sec::OTHER) {
                    size_t* p_off=nullptr; size_t max_off=0; bool swap=false;
                    if (sec==Sec::GFX)   { p_off=&gfx_off; max_off=0x2000; swap=true; }
                    else if (sec==Sec::MAP)   { p_off=&map_off; max_off=0x3000; }
                    else if (sec==Sec::GFF)   { p_off=&gff_off; max_off=0x3100; }

                    if (sec == Sec::MUSIC) {
                        const char* p = linebuf;
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p && music_pattern_id < 64) {
                            int flags_hi = hexval(p[0]);
                            int flags_lo = hexval(p[1]);
                            if (flags_hi >= 0 && flags_lo >= 0) {
                                int flags = (flags_hi << 4) | flags_lo;
                                p += 2;
                                while (*p && hexval(*p) < 0) p++;
                                size_t target_off = 0x3100 + 4 * music_pattern_id;
                                for (int ch = 0; ch < 4; ++ch) {
                                    while (*p && hexval(*p) < 0) p++;
                                    int sfx_id = 0x40; // Default: empty (0x40 = 64)
                                    if (p[0] && p[1]) {
                                        int sfx_hi = hexval(p[0]);
                                        int sfx_lo = hexval(p[1]);
                                        if (sfx_hi >= 0 && sfx_lo >= 0) {
                                            sfx_id = (sfx_hi << 4) | sfx_lo;
                                            p += 2;
                                        }
                                    }
                                    uint8_t flag_bit = (flags >> ch) & 1;
                                    uint8_t val = (sfx_id & 0x7f) | (flag_bit << 7);
                                    if (target_off < 0x3200) {
                                        rom_dest[target_off++] = val;
                                    }
                                }
                                music_pattern_id++;
                            }
                        }
                    } else if (sec == Sec::SFX) {
                        const char* p = linebuf;
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p && sfx_index < 64) {
                            uint8_t current_sfx[84] = {0};
                            int current_sfx_size = 0;
                            while (p[0] && p[1] && current_sfx_size < 84) {
                                int hi = hexval(p[0]);
                                int lo = hexval(p[1]);
                                if (hi < 0 || lo < 0) { p++; continue; }
                                current_sfx[current_sfx_size++] = (uint8_t)((hi << 4) | lo);
                                p += 2;
                            }

                            if (current_sfx_size >= 84) {
                                size_t target_sfx_off = 0x3200 + 68 * sfx_index;
                                for (int j = 0; j < 32; ++j) {
                                    uint32_t ins = ((uint32_t)current_sfx[4 + j * 5 / 2 + 0] << 16)
                                                 | ((uint32_t)current_sfx[4 + j * 5 / 2 + 1] << 8)
                                                 | ((uint32_t)current_sfx[4 + j * 5 / 2 + 2]);
                                    ins = (j & 1) ? ins & 0xfffff : ins >> 4;

                                    uint8_t key        = (ins & 0x3f000) >> 12;
                                    uint8_t instrument = (ins & 0x700)   >> 8;
                                    uint8_t volume     = (ins & 0x70)    >> 4;
                                    uint8_t effect     =  ins & 0x7;
                                    uint8_t custom     = (ins & 0x800)   >> 11;

                                    uint16_t note_val = (key & 0x3f)
                                                      | ((instrument & 0x07) << 6)
                                                      | ((volume & 0x07) << 9)
                                                      | ((effect & 0x07) << 12)
                                                      | ((custom & 0x01) << 15);

                                    rom_dest[target_sfx_off + j * 2 + 0] = note_val & 0xff;
                                    rom_dest[target_sfx_off + j * 2 + 1] = (note_val >> 8) & 0xff;
                                }
                                rom_dest[target_sfx_off + 64] = current_sfx[0]; // filters
                                rom_dest[target_sfx_off + 65] = current_sfx[1]; // speed
                                rom_dest[target_sfx_off + 66] = current_sfx[2]; // loop_start
                                rom_dest[target_sfx_off + 67] = current_sfx[3]; // loop_end

                                sfx_index++;
                            }
                        }
                    } else if (p_off) {
                        const char* p=linebuf;
                        while (p[0]&&p[1]&&*p_off<max_off) {
                            int hi=hexval(p[0]),lo=hexval(p[1]);
                            if (hi<0||lo<0){p++;continue;}
                            rom_dest[*p_off]=(uint8_t)(swap?((lo<<4)|hi):((hi<<4)|lo));
                            (*p_off)++; p+=2;
                        }
                    }
                }
            }
            fclose(sdfile);
            parse_success = true;
            Serial.printf("[DEBUG] SD ROM parse done.\n");
        }

        if (parse_success) {
            // --- パス2: Luaコードのストリームコンパイル ---
            struct SdLuaReaderContext {
                FILE* file;
                char buffer[512];
                size_t buf_pos;
                size_t buf_len;
                bool in_lua;
                bool finished;
            };

            SdLuaReaderContext* ctx = new SdLuaReaderContext();
            std::string full_sd_path = sd_path;
            if (full_sd_path.rfind("/sd", 0) != 0) {
                full_sd_path = "/sd" + full_sd_path;
            }
            ctx->file = fopen(full_sd_path.c_str(), "r");
            ctx->buf_pos = 0;
            ctx->buf_len = 0;
            ctx->in_lua = false;
            ctx->finished = false;

            if (ctx->file) {
                lua_gc(m_lua, LUA_GCCOLLECT, 0);
                Serial.printf("[DEBUG] Compiling Lua stream. Free heap=%u\n", (unsigned)ESP.getFreeHeap());

                auto sd_lua_reader = [](lua_State* L, void* ud, size_t* size) -> const char* {
                    SdLuaReaderContext* c = (SdLuaReaderContext*)ud;
                    if (c->finished) { *size = 0; return nullptr; }

                    if (c->buf_pos < c->buf_len) {
                        *size = c->buf_len - c->buf_pos;
                        const char* res = c->buffer + c->buf_pos;
                        c->buf_pos = c->buf_len;
                        return res;
                    }

                    char line[512];
                    while (fgets(line, sizeof(line), c->file)) {
                        size_t len = strlen(line);
                        char clean_line[512];
                        strcpy(clean_line, line);
                        size_t cl = len;
                        while (cl > 0 && (clean_line[cl-1] == '\n' || clean_line[cl-1] == '\r')) {
                            clean_line[--cl] = '\0';
                        }

                        if (strcmp(clean_line, "__lua__") == 0) {
                            c->in_lua = true;
                            continue;
                        }
                        if (strncmp(clean_line, "__", 2) == 0 && cl >= 4 && clean_line[cl-1] == '_' && clean_line[cl-2] == '_') {
                            c->finished = true;
                            break;
                        }

                        if (c->in_lua) {
                            c->buf_len = len;
                            memcpy(c->buffer, line, len);
                            c->buf_pos = len;
                            *size = len;
                            return c->buffer;
                        }
                    }

                    c->finished = true;
                    *size = 0;
                    return nullptr;
                };

                int old_stepmul = lua_gc(m_lua, LUA_GCSETSTEPMUL, 2000);
                int old_pause   = lua_gc(m_lua, LUA_GCSETPAUSE, 25);
                compile_status = lua_load(m_lua, sd_lua_reader, ctx, ("@" + sd_path).c_str(), "t");
                lua_gc(m_lua, LUA_GCSETSTEPMUL, old_stepmul);
                lua_gc(m_lua, LUA_GCSETPAUSE,   old_pause);
                fclose(ctx->file);

                Serial.printf("[DEBUG] Stream compile done. status=%d. Free heap=%u\n", compile_status, (unsigned)ESP.getFreeHeap());

                // Force aggressive GC after bytecode compilation to reclaim memory
                if (compile_status == LUA_OK) {
                    lua_gc(m_lua, LUA_GCCOLLECT, 0);
                    Serial.printf("[DEBUG] GC after compile. Free heap=%u\n", (unsigned)ESP.getFreeHeap());
                }

                if (compile_status == LUA_OK && game_part) {
                    Serial.println("[DEBUG] Writing bytecode to game partition...");
                    
                    esp_err_t er = esp_partition_erase_range(game_part, 0, game_part->size);
                    if (er == ESP_OK) {
                        std::string bname = sd_path;
                        size_t sl = bname.find_last_of('/');
                        if (sl != std::string::npos) bname = bname.substr(sl+1);
                        char name_buf[32];
                        memset(name_buf, 0, 32);
                        strncpy(name_buf, bname.c_str(), 31);

                        uint32_t rom_sz = (uint32_t)ROM_SIZE;

                        uint32_t woff = 0;
                        esp_partition_write(game_part, woff, "PC8C", 4);     woff += 4;
                        esp_partition_write(game_part, woff, name_buf, 32);  woff += 32;
                        esp_partition_write(game_part, woff, &rom_sz, 4);    woff += 4;
                        esp_partition_write(game_part, woff, &m_ram, ROM_SIZE); woff += ROM_SIZE;
                        
                        uint32_t bc_size_offset = woff;
                        woff += 4; // プレースホルダー書き込みはスルー（NOR Flash 1->0書き込みを考慮）

                        struct PartitionWriterContext {
                            const esp_partition_t* part;
                            uint32_t current_offset;
                            uint32_t total_written;
                        };
                        PartitionWriterContext pw_ctx;
                        pw_ctx.part = game_part;
                        pw_ctx.current_offset = woff;
                        pw_ctx.total_written = 0;

                        auto partition_dump_writer = [](lua_State* L, const void* p, size_t sz, void* ud) -> int {
                            PartitionWriterContext* c = (PartitionWriterContext*)ud;
                            esp_err_t err = esp_partition_write(c->part, c->current_offset, p, sz);
                            if (err != ESP_OK) {
                                Serial.printf("[ERROR] partition write failed: 0x%x\n", err);
                                return 1;
                            }
                            c->current_offset += sz;
                            c->total_written += sz;
                            return 0;
                        };

                        int dump_status = lua_dump(m_lua, partition_dump_writer, &pw_ctx);
                        if (dump_status == 0) {
                            uint32_t final_bc_size = pw_ctx.total_written;
                            esp_partition_write(game_part, bc_size_offset, &final_bc_size, 4);
                            Serial.printf("[DEBUG] game partition written directly: %s rom=%u bc=%u total=%u bytes\n",
                                          name_buf, (unsigned)ROM_SIZE, (unsigned)final_bc_size, (unsigned)pw_ctx.current_offset);
                        } else {
                            Serial.printf("[ERROR] lua_dump failed: %d\n", dump_status);
                        }
                    } else {
                        Serial.printf("[WARN] game partition erase failed: 0x%x, running from RAM\n", er);
                    }

                    ::memcpy(m_cart.get_rom().data(), &m_ram, ROM_SIZE);
                    game_loaded = true;
                } else if (compile_status == LUA_OK) {
                    ::memcpy(m_cart.get_rom().data(), &m_ram, ROM_SIZE);
                    game_loaded = true;
                }
            }
            delete ctx;
        }
    }
#endif

    if (!game_loaded)
    {
#if defined(ARDUINO)
        // 最終フォールバック: カートに残っているソースコードを使用
        std::string code;
        if (!m_cart.has_includes())
            code = std::move(m_cart.get_mutable_code());
        else {
            code = m_cart.preprocess_code();
            m_cart.get_mutable_code().clear();
            m_cart.get_mutable_code().shrink_to_fit();
        }
        lua_gc(m_lua, LUA_GCCOLLECT, 0);
        compile_status = luaL_loadbuffer(m_lua, code.c_str(), code.size(), "@game");
        code.clear(); code.shrink_to_fit();
        if (compile_status == LUA_OK) game_loaded = true;
#else
        // デスクトップ/インクルードファイルのフォールバック
        std::string code;
        if (!m_cart.has_includes())
            code = std::move(m_cart.get_mutable_code());
        else {
            code = m_cart.preprocess_code();
            m_cart.get_mutable_code().clear();
            m_cart.get_mutable_code().shrink_to_fit();
        }
        lua_gc(m_lua, LUA_GCCOLLECT, 0);
        compile_status = luaL_loadbuffer(m_lua, code.c_str(), code.size(), "@game");
        code.clear(); code.shrink_to_fit();
        if (compile_status == LUA_OK) game_loaded = true;
#endif
    }

    m_cart.clear_code();

    if (compile_status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("Cart compilation error %d: %s\n", compile_status, message ? message : "(null)");
#if defined(ARDUINO)
        Serial.printf("[OOM DEBUG] Lua mem used: %d KB  Free heap: %u  MaxFree: %u\n",
                      lua_gc(m_lua, LUA_GCCOUNT, 0),
                      (unsigned int)ESP.getFreeHeap(),
                      (unsigned int)ESP.getMaxAllocHeap());
#endif
        lua_pop(m_lua, 1);
        m_is_running = false;
        return;
    }

    // コンパイル済みチャンク（スタックトップ）を __z8_compiled_cart_code に保存
    // ※ glue_code はモンキーパッチ済み __z8_run_cart 内で別途実行する
    lua_setglobal(m_lua, "__z8_compiled_cart_code");

#if defined(ARDUINO)
    lua_gc(m_lua, LUA_GCCOLLECT, 0);
    Serial.printf("[DEBUG] chunk saved. Free heap=%u\n", (unsigned int)ESP.getFreeHeap());
#endif

    // Call __z8_run_cart(nil)
    lua_getglobal(m_lua, "__z8_run_cart");
    lua_pushnil(m_lua);
    int status = lua_pcall(m_lua, 1, 0, 0);
    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("error %d running __z8_run_cart: %s\n", status, message ? message : "(null)");
        lua_pop(m_lua, 1);
        m_is_running = false;
    }
}

void vm::api_reload(int16_t in_dst, int16_t in_src, opt<int16_t> in_size, opt<std::string> filename)
{
    using std::min;

    int dst = 0, src = 0, size = offsetof(memory, code);

    if (in_size && *in_size <= 0) return;

    if (in_size)
    {
        dst  = in_dst  & 0xffff;
        src  = in_src  & 0xffff;
        size = *in_size & 0xffff;
    }

    if (dst + size > (int)sizeof(m_ram))
    {
        runtime_error("bad memory access");
        return;
    }

    if (src > (int)offsetof(memory, code))
    {
        int amount = min(size, (int)sizeof(m_ram) - src);
        ::memset(&m_ram[dst], 0, amount);
        dst  += amount;
        src   = (src + amount) & 0xffff;
        size -= amount;
    }

    int amount = min(size, (int)offsetof(memory, code) - src);

    if (filename.has_value())
    {
        std::string name = get_path_active_dir() + "/" + filename.value();
        if (!lol::ends_with(lol::tolower(name), ".p8") &&
            !lol::ends_with(lol::tolower(name), ".png"))
            name += ".p8";
        auto reload_cart = std::make_shared<cart>();
        load_cart(*reload_cart, name);
        ::memcpy(&m_ram[dst], reload_cart->get_rom().data() + src, amount);
    }
    else
    {
        ::memcpy(&m_ram[dst], m_cart.get_rom().data() + src, amount);
    }

    dst  += amount;
    size -= amount;
    ::memset(&m_ram[dst], 0, size);

    update_registers();
}

void vm::api_cstore(int16_t in_dst, int16_t in_src, opt<int16_t> in_size, opt<std::string> filename)
{
    int dst = 0, src = 0, size = offsetof(memory, code);

    if (in_size && *in_size <= 0) return;

    if (in_size)
    {
        dst  = in_dst  & 0xffff;
        src  = in_src  & 0xffff;
        size = *in_size & 0xffff;
    }

    if (filename.has_value())
    {
        std::string name = get_path_active_dir() + "/" + filename.value();
        if (!lol::ends_with(lol::tolower(name), ".p8") &&
            !lol::ends_with(lol::tolower(name), ".png"))
            name += ".p8";
        auto reload_cart = std::make_shared<cart>();
        load_cart(*reload_cart, name);
        reload_cart->set_from_ram((const uint8_t*)&m_ram, dst, src, size);
        save_cart(*reload_cart, reload_cart->get_filename());
    }
    else
    {
        m_cart.set_from_ram((const uint8_t*)&m_ram, dst, src, size);
        save_cart(m_cart, m_cart.get_filename());
    }

    update_registers();
}

fix32 vm::api_dget(int16_t n)
{
    return n >= 0 && n < 64 ? api_peek4(0x5e00 + 4 * n, 1)[0] : fix32(0);
}

void vm::api_dset(int16_t n, fix32 x)
{
    if (n >= 0 && n < 64)
        api_poke4(0x5e00 + 4 * n, std::vector<fix32>{ x });
}

// ──────────────────────────────────────────────────────────────
// Memory peek / poke
// ──────────────────────────────────────────────────────────────

uint8_t vm::raw_peek(int16_t addr)
{
    addr = address_translate(addr);
    return m_ram[addr];
}

std::vector<int16_t> vm::api_peek(int16_t addr, opt<int16_t> count)
{
    std::vector<int16_t> ret;
    size_t n = count ? std::max(0, std::min((int)*count, 8192)) : 1;
    for (; ret.size() < n; ++addr)
        ret.push_back(raw_peek(addr));
    return ret;
}

std::vector<int16_t> vm::api_peek2(int16_t addr, opt<int16_t> count)
{
    std::vector<int16_t> ret;
    size_t n = count ? std::max(0, std::min((int)*count, 8192)) : 1;
    for (; ret.size() < n; addr += 2)
    {
        int16_t bits = 0;
        for (int i = 0; i < 2; ++i)
            bits |= raw_peek(addr + i) << (8 * i);
        ret.push_back(bits);
    }
    return ret;
}

std::vector<fix32> vm::api_peek4(int16_t addr, opt<int16_t> count)
{
    std::vector<fix32> ret;
    size_t n = count ? std::max(0, std::min((int)*count, 8192)) : 1;
    for (; ret.size() < n; addr += 4)
    {
        int32_t bits = 0;
        for (int i = 0; i < 4; ++i)
            bits |= raw_peek(addr + i) << (8 * i);
        ret.push_back(fix32::frombits(bits));
    }
    return ret;
}

int16_t vm::address_translate(int16_t addr)
{
    if (addr >= 0x0000 && addr < 0x2000 && m_ram.hw_state.mapping_spritesheet == 0x60)
        addr = addr + 0x6000;
    if (addr >= 0x6000 && m_ram.hw_state.mapping_screen == 0)
        addr = addr - 0x6000;
    return addr;
}

void vm::raw_poke(int16_t addr, uint8_t val)
{
    if (addr >= 0x5e00 && addr < 0x5f00) m_savefile.set_dirty();
    addr = address_translate(addr);
    m_ram[addr] = (uint8_t)val;
}

void vm::api_poke(int16_t addr, std::vector<int16_t> args)
{
    if (args.empty()) args.push_back(0);
    for (auto val : args) raw_poke(addr++, (uint8_t)val);
    update_registers();
}

void vm::api_poke2(int16_t addr, std::vector<int16_t> args)
{
    if (args.empty()) args.push_back(0);
    for (auto val : args)
    {
        raw_poke(addr++, (uint8_t)val);
        raw_poke(addr++, (uint8_t)((uint16_t)val >> 8));
    }
    update_registers();
}

void vm::api_poke4(int16_t addr, std::vector<fix32> args)
{
    if (args.empty()) args.push_back(fix32(0));
    for (auto val : args)
    {
        uint32_t x = (uint32_t)val.bits();
        raw_poke(addr++, (uint8_t)x);
        raw_poke(addr++, (uint8_t)(x >> 8));
        raw_poke(addr++, (uint8_t)(x >> 16));
        raw_poke(addr++, (uint8_t)(x >> 24));
    }
    update_registers();
}

void vm::api_memcpy(int16_t in_dst, int16_t in_src, int16_t in_size)
{
    using std::min;
    if (in_size <= 0) return;

    int src = in_src & 0xffff;
    int dst = in_dst & 0xffff;
    int size = in_size & 0xffff;

    if (src < dst)
    {
        int16_t addr_dst = dst + size;
        int16_t addr_src = src + size;
        for (int16_t i = 0; i < size; ++i)
            raw_poke(--addr_dst, raw_peek(--addr_src));
    }
    else
    {
        int16_t addr_dst = dst;
        int16_t addr_src = src;
        for (int16_t i = 0; i < size; ++i)
            raw_poke(addr_dst++, raw_peek(addr_src++));
    }
    update_registers();
}

void vm::api_memset(int16_t dst, uint8_t val, int16_t size)
{
    if (size <= 0) return;
    for (int16_t i = 0; i < size; ++i)
        raw_poke(dst++, val);
    update_registers();
}

void vm::update_registers()
{
    for (uint8_t &btn : m_ram.hw_state.btn_state)
        btn &= 0x3f;
}

void vm::update_prng()
{
    auto &prng = m_ram.hw_state.prng;
    prng.a = ((prng.a >> 16) | (prng.a << 16)) + prng.b;
    prng.b += prng.a;
}

// ──────────────────────────────────────────────────────────────
// PRNG / stat / printh / extcmd
// ──────────────────────────────────────────────────────────────

fix32 vm::api_private_rnd(opt<fix32> in_range)
{
    if (in_range && in_range->bits() == 0) return fix32(0);
    update_prng();
    uint32_t a = m_ram.hw_state.prng.a;
    uint32_t range = in_range ? uint32_t(in_range->bits()) : 0x10000;
    return fix32::frombits(range > 0 ? a % range : 0);
}

void vm::api_srand(fix32 seed)
{
    seed &= fix32::frombits(0x7fffffff);
    auto &prng = m_ram.hw_state.prng;
    prng.b = seed ? seed.bits() : 0xdeadbeef;
    prng.a = prng.b ^ 0xbead29ba;
    for (int i = 0; i < 32; ++i) update_prng();
}

template<class... T>
static auto any_to_variant(std::any a) -> z8::pico8::var<T...>
{
    (void)a;
    return nullptr;
}

var<bool, int16_t, fix32, std::string, std::nullptr_t> vm::api_stat(int16_t id)
{
    for (int i = 0; i < m_stats_count; i++)
    {
        if (m_stats_buf[i].id == id)
        {
            auto ret = m_stats_buf[i].fn();
            return any_to_variant<bool, int16_t, fix32, std::string, std::nullptr_t>(ret);
        }
    }

    if (id == 0)
    {
        static uint32_t last_gc_frame = 0;
        static uint32_t frame_count = 0;
        frame_count++;
        bool force_gc = false;
#if defined(ARDUINO)
        if (ESP.getFreeHeap() < 8192)
        {
            force_gc = true;
        }
#endif
        if (force_gc || (frame_count - last_gc_frame >= 5))
        {
            lua_gc(m_sandbox_lua, LUA_GCCOLLECT, 0);
            last_gc_frame = frame_count;
        }
        int32_t bits = ((int)lua_gc(m_sandbox_lua, LUA_GCCOUNT, 0) << 16)
                     + ((int)lua_gc(m_sandbox_lua, LUA_GCCOUNTB, 0) << 6);
        return fix32::frombits(bits);
    }

    if (id == 1 || id == 2) return fix32(m_instructions / float(m_max_instructions));
    if (id == 3) return int16_t(0); // Multiscreen disabled
    if (id == 4) return std::string();
    if (id == 5) return int16_t(PICO8_VERSION);
    if (id == 6) { if (breadcrumbs_count > 0) return breadcrumbs_buf[breadcrumbs_count-1].params; return ""; }
    if (id == 7 || id == 8 || id == 9) return int16_t(30);
    if (id == 11) return int16_t(1); // Multiscreen disabled, always 1
    if (id >= 12 && id <= 15) return int16_t(0);

    if ((id >= 16 && id <= 26) || (id >= 46 && id <= 56))
    {
        int16_t audio_id = (id <= 26) ? id : id - 30;
        if (audio_id >= 16 && audio_id <= 19)
            return m_state.channels[audio_id & 3].main_sfx.sfx;
        if (audio_id >= 20 && audio_id <= 23)
            return m_state.channels[audio_id & 3].main_sfx.sfx == -1 ? fix32(-1)
                 : fix32((int)m_state.channels[audio_id & 3].main_sfx.offset);
        if (audio_id == 24) return int16_t(m_state.music.pattern);
        if (audio_id == 25) return int16_t(m_state.music.count);
        if (audio_id == 26) return int16_t(m_state.music.offset);
    }

    if (id == 57) return m_state.music.pattern != -1;
    if (id == 29) return int16_t(0);

    if (id >= 30 && id <= 39)
    {
        bool devkit_mode    = m_ram.draw_state.mouse_flags.enabled;
        bool devkit_pointer = devkit_mode && m_ram.draw_state.mouse_flags.locked;
        bool has_text = devkit_mode && m_state.kbd.start != m_state.kbd.stop;

        switch (id)
        {
            case 30: return has_text;
            case 31:
                if (!has_text) return std::string();
                if (m_state.kbd.stop > m_state.kbd.start)
                {
                    std::string ret(&m_state.kbd.chars[m_state.kbd.start],
                                    m_state.kbd.stop - m_state.kbd.start);
                    m_state.kbd.start = m_state.kbd.stop = 0;
                    return ret;
                }
                { std::string ret(&m_state.kbd.chars[m_state.kbd.start],
                                  (int)sizeof(m_state.kbd.chars) - m_state.kbd.start);
                  m_state.kbd.start = 0; }
                return (int16_t)0;
            case 32: return devkit_mode ? m_state.mouse.x : fix32(0);
            case 33: return devkit_mode ? m_state.mouse.y : fix32(0);
            case 34: return devkit_mode ? m_state.mouse.b : fix32(0);
            case 35: return (int16_t)0;
            case 36: return devkit_mode ? m_state.mouse.s[2] : fix32(0);
            case 37: return devkit_mode ? m_state.mouse.ac : fix32(0);
            case 38: return devkit_pointer ? m_state.mouse.rx : fix32(0);
            case 39: return devkit_pointer ? m_state.mouse.ry : fix32(0);
        }
    }

    if ((id >= 80 && id <= 85) || (id >= 90 && id <= 95))
    {
        time_t t;
        time(&t);
        auto const *tm = (id <= 85 ? std::gmtime : std::localtime)(&t);
        switch (id % 10)
        {
            case 0: return int16_t(tm->tm_year + 1900);
            case 1: return int16_t(tm->tm_mon + 1);
            case 2: return int16_t(tm->tm_mday);
            case 3: return int16_t(tm->tm_hour);
            case 4: return int16_t(tm->tm_min);
            case 5: return int16_t(tm->tm_sec);
        }
    }

    if (id >= 48 && id < 72)
    {
        if (id == 49 || (id >= 58 && id <= 63)) return std::string();
        return nullptr;
    }
    if (id >= 72 && id < 100) return (int16_t)0;

    if (id == 100) { if (breadcrumbs_count > 0) return breadcrumbs_buf[breadcrumbs_count-1].title; return nullptr; }
    if (id == 101) return nullptr;
    if (id == 120 || id == 121) return false;
    if (id == 124) return std::string("/");

    if (id == 130) return m_metadata_title;
    if (id == 131) return m_metadata_author;
    if (id == 140) return fix32(m_state.music.volume_music);
    if (id == 141) return fix32(m_state.music.volume_sfx);
    if (id == 142) return getfiltername_callback ? getfiltername_callback(m_filter_index) : std::string("none");
    if (id == 143) return int16_t(m_fullscreen);
    if (id == 147) return int16_t(m_save_slot);
    if (id == 149) return m_quit_confirmation;

    if (id == 150) return fix32(m_state.axes[0][0]);
    if (id == 151) return fix32(m_state.axes[0][1]);
    if (id == 152) return fix32(m_state.axes[1][0]);
    if (id == 153) return fix32(m_state.axes[1][1]);

    if (id == 160)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%f %f %f",
                 (float)m_state.rotation.x,
                 (float)m_state.rotation.y,
                 (float)m_state.rotation.z);
        return std::string(buf);
    }

    int ui_texts_size = sizeof(m_ui_texts) / sizeof(m_ui_texts[0]);
    if (id >= 200 && id < (int16_t)(200 + ui_texts_size))
        return std::string(m_ui_texts[id - 200]);

    return (int16_t)0;
}

void vm::add_stat(int16_t id, std::function<std::any()> fn)
{
    for (int i = 0; i < m_stats_count; i++)
    {
        if (m_stats_buf[i].id == id)
        {
            m_stats_buf[i].fn = fn;
            return;
        }
    }
    if (m_stats_count < MAX_STATS)
    {
        m_stats_buf[m_stats_count].id = id;
        m_stats_buf[m_stats_count].fn = fn;
        m_stats_count++;
    }
}

void vm::api_printh(rich_string str, opt<std::string> filename, opt<bool> overwrite)
{
    std::string decoded;
    for (uint8_t ch : str)
        decoded += std::string(charset::to_utf8[ch]);

    if (filename.has_value())
    {
        // On Arduino: write to SD card
#if defined(ARDUINO)
        File f = SD.open(filename.value().c_str(),
                         overwrite.value_or(false) ? FILE_WRITE : FILE_APPEND);
        if (f) { decoded += "\n"; f.print(decoded.c_str()); f.close(); }
#else
        FILE *file = fopen(filename.value().c_str(), overwrite.value_or(false) ? "w" : "a");
        if (file) { decoded += "\n"; fwrite(decoded.data(), 1, decoded.size(), file); fclose(file); }
#endif
    }
    else
    {
        lol::msg::info("%s\n", decoded.c_str());
    }
}

void vm::fill_metadata(cart &metadata_cart)
{
    strncpy(m_metadata_title, metadata_cart.get_title().c_str(), sizeof(m_metadata_title) - 1);
    m_metadata_title[sizeof(m_metadata_title) - 1] = '\0';
    strncpy(m_metadata_author, metadata_cart.get_author().c_str(), sizeof(m_metadata_author) - 1);
    m_metadata_author[sizeof(m_metadata_author) - 1] = '\0';
    auto &label = metadata_cart.get_label();
    if (label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        for (int y = 0; y < LABEL_HEIGHT; ++y)
            for (int x = 0; x < LABEL_WIDTH; x += 2)
            {
                uint8_t col  = label[y * LABEL_WIDTH + x]     & 0x1f;
                col |= (label[y * LABEL_WIDTH + x + 1] & 0x1f) << 4;
                m_ram[(x/2) + y * 64] = col;
            }
    }
}

void vm::api_extcmd(std::string cmdline)
{
    std::string cmd  = cmdline.substr(0, cmdline.find(" "));
    std::string args = cmdline.substr(std::min(cmd.length() + 1, cmdline.length()));

    if (cmd == "z8_load_metadata")
    {
        if (args.length() > 0)
        {
            std::string name = get_path_active_dir() + "/" + args;
            if (!lol::ends_with(lol::tolower(name), ".p8") &&
                !lol::ends_with(lol::tolower(name), ".png"))
                name += ".p8";
            auto metadata_cart = std::make_shared<cart>();
            load_cart(*metadata_cart, name);
            fill_metadata(*metadata_cart);
        }
        else fill_metadata(m_cart);
    }
    else if (cmd == "reset")       { api_run(); }
    else if (cmd == "pause")       { luaL_dostring(m_lua, "__z8_enter_pause()"); }
    else if (cmd == "z8_volume_music_up")
    {
        m_state.music.volume_music = std::clamp(m_state.music.volume_music + 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_music_down")
    {
        m_state.music.volume_music = std::clamp(m_state.music.volume_music - 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_sfx_up")
    {
        m_state.music.volume_sfx = std::clamp(m_state.music.volume_sfx + 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_sfx_down")
    {
        m_state.music.volume_sfx = std::clamp(m_state.music.volume_sfx - 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_set_cpu_limit")
    {
        m_max_instructions = (args.length() > 0) ? std::stoi(args) * 1000 : m_default_max_instructions;
    }
    else if (cmd == "breadcrumb" || cmd == "go_back")
    {
        if (breadcrumbs_count <= 0) return;
        breadcrumb_path bc = breadcrumbs_buf[--breadcrumbs_count];
        save(true);
        m_cart.load(bc.cart_path);
        run();
    }
    else if (cmd == "z8_app_requestexit") { request_exit(); }
    else if (cmd == "z8_save_slot")
    {
        if (args.length() > 0)
        {
            save_cartdata(true);
            m_save_slot = std::max(0, std::stoi(args));
            if (!load_cartdata()) memset(m_ram.persistent, 0, 256);
        }
    }
    else if (cmd == "z8_setuitext")
    {
        auto param1 = args.substr(0, args.find(" "));
        auto param2 = args.substr(std::min(param1.length() + 1, args.length()));
        if (param1.length() > 0 && param2.length() > 0)
        {
            try {
                int index = std::stoi(param1);
                int ui_texts_size = sizeof(m_ui_texts) / sizeof(m_ui_texts[0]);
                if (index >= 0 && index < ui_texts_size)
                {
                    strncpy(m_ui_texts[index], param2.c_str(), sizeof(m_ui_texts[index]) - 1);
                    m_ui_texts[index][sizeof(m_ui_texts[index]) - 1] = '\0';
                }
            } catch (...) {}
        }
    }
    else { lol::msg::info("unknown extcmd: %s\n", cmdline.c_str()); }
}

void vm::add_extcmd(std::string const &name, std::function<void(std::string const &)> fn)
{
    (void)name;
    (void)fn;
}

void vm::api_map_display(int16_t id)
{
    // Multiscreen disabled, no-op
    (void)id;
}

// ──────────────────────────────────────────────────────────────
// I/O
// ──────────────────────────────────────────────────────────────

void vm::private_buttons()
{
    uint8_t *btn_state = m_ram.hw_state.btn_state;

    for (int i = 0; i < 64; ++i)
    {
        if (m_state.buttons[1][i] == 0) m_state.buttons[2][i] = 1;
        if (m_state.buttons[2][i] == 0) m_state.buttons[1][i] = 0;

        if (m_state.buttons[1][i])
        {
            btn_state[i / 8] |= 1 << (i % 8);
            ++m_state.buttons[0][i];
        }
        else
        {
            btn_state[i / 8] &= ~(1 << (i % 8));
            m_state.buttons[0][i] = 0;
        }
        m_state.buttons[1][i] = 0;
    }

    m_state.mouse.s[2] = m_state.mouse.s[1] - m_state.mouse.s[0];
    m_state.mouse.s[1] = m_state.mouse.s[0];
    m_state.mouse.lb   = m_state.mouse.b;

    // pointer lock — no-op on Cardputer
    if (m_pointer_locked != (bool)m_ram.draw_state.mouse_flags.locked)
    {
        m_pointer_locked = m_ram.draw_state.mouse_flags.locked;
        if (pointerLock_callback) pointerLock_callback(m_ram.draw_state.mouse_flags.locked);
    }
}

void vm::private_mask_buttons()
{
    for (int i = 0; i < 64; ++i)
        m_state.buttons[2][i] = 0;
}

var<bool, int16_t> vm::api_btn(opt<int16_t> n, int16_t p)
{
    if (n)
    {
        if (*n < 0 || *n >= 8 || p < 0 || p >= 8) return false;
        return bool(m_state.buttons[0][(*n + 8 * p) & 0x3f]);
    }
    int16_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= m_state.buttons[0][i] ? 1 << i : 0;
    return bits;
}

var<bool, int16_t> vm::api_btnp(opt<int16_t> n, int16_t p)
{
    int delay = m_ram.hw_state.btnp_delay ? m_ram.hw_state.btnp_delay : 15;
    int rate  = m_ram.hw_state.btnp_rate  ? m_ram.hw_state.btnp_rate  : 4;

    auto was_pressed = [delay, rate](int i) -> bool
    {
        if (i == 1) return true;
        if (delay != 255 && i > delay && (i - delay - 1) % rate == 0) return true;
        return false;
    };

    if (n)
    {
        if (*n < 0 || *n >= 8 || p < 0 || p >= 8) return false;
        return was_pressed(m_state.buttons[0][(*n + 8 * p) & 0x3f]);
    }

    int16_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= was_pressed(m_state.buttons[0][i]) ? 1 << i : 0;
    return bits;
}

void vm::api_serial(int16_t chan, int16_t address, int16_t len)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "serial(0x%04x, 0x%04x, 0x%04x)", chan, address, len);
    private_stub(std::string(buf));
}

fix32 vm::api_time()
{
    return fix32(m_time);
}

// ──────────────────────────────────────────────────────────────
// Save / load
// ──────────────────────────────────────────────────────────────

bool vm::save(bool force)
{
    save_cartdata(force);
    save_config(force);
    return true;
}

bool vm::load_cartdata()
{
    if (strlen(m_cartdata) == 0) return false;
    return m_savefile.read_save(get_path_save(m_cartdata), m_ram.persistent);
}

bool vm::save_cartdata(bool force)
{
    if (strlen(m_cartdata) == 0) return false;
    if (!m_savefile.tick(force)) return true;
    return m_savefile.write_save(get_path_save(m_cartdata), m_ram.persistent);
}

// Config helpers
static bool config_parse_256(std::string line, std::string name, float &value)
{
    if (!line.rfind(name, 0) == 0) return false;   // starts_with
    if (line.find(name) != 0) return false;
    std::string rest = line.substr(name.length());
    try { int rawvalue = std::stoi(rest); value = std::clamp(rawvalue / 256.0f, 0.0f, 1.0f); return true; }
    catch (...) { return false; }
}

static bool config_parse_int(std::string line, std::string name, int &value)
{
    if (line.find(name) != 0) return false;
    std::string rest = line.substr(name.length());
    try { value = std::stoi(rest); return true; }
    catch (...) { return false; }
}

bool vm::load_config()
{
    std::string s;
    if (!lol::file::read(get_path_config(), s)) return false;
    auto ss = std::stringstream(s);
    for (std::string line; std::getline(ss, line, '\n');)
    {
        config_parse_256(line, "sound_volume ",   m_state.music.volume_sfx);
        config_parse_256(line, "music_volume ",   m_state.music.volume_music);
        config_parse_int(line, "filter_index ",   m_filter_index);
        config_parse_int(line, "fullscreen_method ", m_fullscreen);
        config_parse_int(line, "save_slot ",      m_save_slot);
    }
    return true;
}

bool vm::save_config(bool force)
{
    if (!m_configfile.tick(force)) return true;

    auto make_256 = [](std::string name, float value) -> std::string
    {
        int v = std::clamp((int)(value * 256.0f), 0, 256);
        return name + " " + std::to_string(v) + "\n";
    };
    auto make_int = [](std::string name, int value) -> std::string
    {
        return name + " " + std::to_string(value) + "\n";
    };

    std::string content;
    content += make_256("sound_volume",      m_state.music.volume_sfx);
    content += make_256("music_volume",      m_state.music.volume_music);
    content += make_int("filter_index",      m_filter_index);
    content += make_int("fullscreen_method", m_fullscreen);
    content += make_int("save_slot",         m_save_slot);

    return lol::file::write(get_path_config(), content);
}

// Path helpers
std::string vm::get_path_config()
{
    return "/config/config.txt";
}

std::string vm::get_path_cstore(std::string cart_name)
{
    size_t found = cart_name.find_last_of("/\\");
    cart_name = cart_name.substr(found == std::string::npos ? 0 : found + 1);
    if (lol::ends_with(lol::tolower(cart_name), ".p8.png"))
        cart_name = cart_name.substr(0, cart_name.length() - 4);

    std::string dir = "/saves/cstore";
    if (m_save_slot > 0) dir += "_" + std::to_string(m_save_slot);
    dir += "/";
    make_dir(dir);
    return dir + cart_name;
}

std::string vm::get_path_save(std::string cart_name)
{
    std::string dir = "/saves/cdata";
    if (m_save_slot > 0) dir += "_" + std::to_string(m_save_slot);
    dir += "/";
    make_dir(dir);
    return dir + cart_name + ".p8d.txt";
}

void vm::set_path_active_dir(std::string filename)
{
    size_t found = filename.find_last_of("/\\");
    if (found != std::string::npos)
        snprintf(m_path_active_dir, sizeof(m_path_active_dir), "%.*s", (int)found, filename.c_str());
    else
        snprintf(m_path_active_dir, sizeof(m_path_active_dir), "/cartridges");
}

std::string vm::get_path_active_dir()
{
    if (strlen(m_path_active_dir) == 0)
        set_path_active_dir(get_default_carts_dir());
    return std::string(m_path_active_dir);
}

std::string vm::get_default_carts_dir()
{
    make_dir("/cartridges");
    return "/cartridges";
}

} // namespace z8::pico8
