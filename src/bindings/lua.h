// bindings/lua.h — Lua binding mechanism
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: removed lol/engine.h dependency, includes lol_compat.h via zepto8.h

#pragma once

#include <optional>
#include <variant>

#include "3rdparty/z8lua/lua.h"
#include "3rdparty/z8lua/lauxlib.h"
#include "3rdparty/z8lua/lualib.h"

// fix32 is included via z8lua
#include "fix32.h"

namespace z8::bindings
{

//
// Push a standard type to the Lua stack
//

template<typename T> static int lua_push(lua_State *l, T const &);

template<> int lua_push(lua_State *l, bool const &x)        { lua_pushboolean(l, x); return 1; }
template<> int lua_push(lua_State *l, uint8_t const &x)     { lua_pushnumber(l, x);  return 1; }
template<> int lua_push(lua_State *l, int16_t const &x)     { lua_pushnumber(l, x);  return 1; }
template<> int lua_push(lua_State *l, fix32 const &x)       { lua_pushnumber(l, x);  return 1; }
template<> int lua_push(lua_State *l, std::string const &s) { lua_pushlstring(l, s.c_str(), (int)s.size()); return 1; }
template<> int lua_push(lua_State *l, std::nullptr_t const &) { lua_pushnil(l); return 1; }

template<typename... T> int lua_push(lua_State *l, std::variant<T...> const &x)
{
    return std::visit([l](auto &&arg) -> int { return lua_push(l, arg); }, x);
}

template<typename T> int lua_push(lua_State *l, std::optional<T> const &x)
{
    return x ? lua_push(l, *x) : 0;
}

template<typename... T> int lua_push(lua_State *l, std::tuple<T...> const &t)
{
    std::apply([l](auto &&... x){ ((lua_push(l, x)), ...); }, t);
    return (int)sizeof...(T);
}

template<typename... T> int lua_push(lua_State *l, std::vector<T...> const &v)
{
    for (auto &x : v) lua_push(l, x);
    return (int)v.size();
}

//
// Get a standard type from the Lua stack
//

template<typename T> static T    lua_get(lua_State *l, int i);
template<typename T> static void lua_get(lua_State *l, int i, T &);

template<> void lua_get(lua_State *l, int i, fix32   &arg) { arg = lua_tonumber(l, i); }
template<> void lua_get(lua_State *l, int i, bool    &arg) { arg = (bool)lua_toboolean(l, i); }
template<> void lua_get(lua_State *l, int i, uint8_t &arg) { arg = (uint8_t)lua_tonumber(l, i); }
template<> void lua_get(lua_State *l, int i, int16_t &arg) { arg = (int16_t)lua_tonumber(l, i); }

template<> void lua_get(lua_State *l, int n, std::string &arg)
{
    if (lua_isstring(l, n))
    {
        size_t len;
        char const *s = lua_tolstring(l, n, &len);
        arg.assign(s, len);
    }
}

template<typename T> void lua_get(lua_State *l, int i, std::optional<T> &arg)
{
    if (!lua_isnone(l, i))
        arg = lua_get<T>(l, i);
}

template<typename T> void lua_get(lua_State *l, int i, std::vector<T> &arg)
{
    while (!lua_isnone(l, i))
        arg.push_back(lua_get<T>(l, i++));
}

template<typename T> static T lua_get(lua_State *l, int i)
{
    T ret; lua_get(l, i, ret); return ret;
}

//
// Lua binding mechanism
//

class lua
{
public:
    template<typename T>
    static void init(lua_State *l, T *that)
    {
        auto lib = typename T::template exported_api<lua>().data;
        lib.push_back({});

        lua_pushglobaltable(l);
        lua_pushlightuserdata(l, that); // Push VM instance pointer as upvalue 1
        luaL_setfuncs(l, lib.data(), 1);
    }

    template<auto FN> struct bind
    {
        static int wrap(lua_State *l)
        {
            return dispatch(l, FN, make_seq(FN));
        }

        template<typename T, typename R, typename... A>
        static constexpr auto make_seq(R (T::*)(A...))
        {
            return std::index_sequence_for<A...>();
        }
    };

    struct bind_desc : luaL_Reg
    {
        bind_desc() : luaL_Reg({ nullptr, nullptr }) {}

        template<auto FN>
        bind_desc(char const *str, bind<FN> b)
            : luaL_Reg({ str, &b.wrap })
        {}
    };

private:
    template<typename T, typename R, typename... A, size_t... IS>
    static inline int dispatch(lua_State *l, R (T::*f)(A...),
                               std::index_sequence<IS...>)
    {
        // Retrieve VM instance from upvalue 1 (extremely fast O(1))
        T *that = (T *)lua_touserdata(l, lua_upvalueindex(1));

        that->m_sandbox_lua = l;

        if constexpr (std::is_same<R, void>::value)
            return (that->*f)(lua_get<A>(l, IS + 1)...), 0;
        else
            return lua_push(l, (that->*f)(lua_get<A>(l, IS + 1)...));
    }
};

} // namespace z8::bindings
