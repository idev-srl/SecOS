/* SecOS lua interpreter front-end. A freestanding replacement for lua.c: opens
 * the OS-independent standard libraries (base/table/string/math/coroutine/utf8)
 * and runs a script from a file argument, `-e <chunk>`, or a built-in demo.
 * No io/os/debug/package libs (they need a fuller POSIX surface). */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char* DEMO =
  "print('hello from lua ' .. _VERSION .. ' on SecOS')\n"
  "local s=0; for i=1,100 do s=s+i end\n"
  "print('sum 1..100 =', s)\n"
  "print('3.14 * 2 =', 3.14*2)\n"
  "print('math.sqrt(2) =', math.sqrt(2))\n"
  "print('1/3 =', 1/3)\n"
  "print('math.pi =', math.pi)\n"
  "print('2^0.5 =', 2^0.5)\n"
  "local t={} for i=1,5 do t[i]=i*i end\n"
  "print('squares =', table.concat(t, ','))\n"
  "print(string.format('format: %d %.4f %s %x', 42, 2.718281828, 'ok', 255))\n"
  "local function fib(n) if n<2 then return n end return fib(n-1)+fib(n-2) end\n"
  "print('fib(20) =', fib(20))\n"
  "print('upper =', string.upper('secos rocks'))\n";

static void open_safe_libs(lua_State* L) {
    static const luaL_Reg libs[] = {
        { LUA_GNAME,       luaopen_base },
        { LUA_TABLIBNAME,  luaopen_table },
        { LUA_STRLIBNAME,  luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_COLIBNAME,   luaopen_coroutine },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { NULL, NULL }
    };
    for (const luaL_Reg* l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
}

static char g_filebuf[262144];

int main(int argc, char** argv) {
    lua_State* L = luaL_newstate();
    if (!L) { write(2, "lua: cannot create state (out of memory)\n", 41); return 1; }
    open_safe_libs(L);

    const char* code = DEMO;
    const char* chunkname = "=demo";
    if (argc >= 2) {
        if (strcmp(argv[1], "-e") == 0 && argc >= 3) {
            code = argv[2]; chunkname = "=(command line)";
        } else {
            int fd = open(argv[1], O_RDONLY);
            if (fd < 0) { dprintf(2, "lua: cannot open '%s'\n", argv[1]); lua_close(L); return 1; }
            int n = 0, r;
            while (n < (int)sizeof(g_filebuf) - 1 &&
                   (r = read(fd, g_filebuf + n, sizeof(g_filebuf) - 1 - n)) > 0)
                n += r;
            close(fd);
            g_filebuf[n] = 0; code = g_filebuf; chunkname = argv[1];
        }
    }

    int rc = 0;
    if (luaL_loadbuffer(L, code, strlen(code), chunkname) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        dprintf(2, "lua: %s\n", msg ? msg : "unknown error");
        rc = 1;
    }
    lua_close(L);
    return rc;
}
