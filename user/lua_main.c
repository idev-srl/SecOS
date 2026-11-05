/* SecOS lua interpreter front-end. A freestanding replacement for lua.c: opens
 * the standard libraries that work on SecOS (base/table/string/math/coroutine/
 * utf8 + io/os over the SecOS libc) and implements the usual command line:
 *
 *   lua                      interactive REPL when stdin is a terminal
 *   lua -e 'chunk' [-e ...]  run chunk(s)
 *   lua script.lua [args]    run a script; `arg` holds the arguments
 *   lua -                    run stdin as a script
 *   lua -i [script]          enter the REPL after running the script
 *   lua -v                   print the version
 *   lua --demo               the built-in M34 demo (also when started argc==0,
 *                            e.g. by the M34_LUA_DEMO kernel gate)
 *
 * REPL: a line is first tried as `return <line>` so expressions print their
 * value (like lua 5.4); an incomplete statement (error ending in <eof>) keeps
 * reading continuation lines with the `>>` prompt. Ctrl-D on an empty line
 * (EOF) leaves the REPL; Ctrl-C at the prompt cancels the line being typed. No
 * package/debug libs (no dynamic loading on SecOS). */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>

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

/* ---- libc gaps used by the io/os libraries (see user/lua_port.h) ---- */
extern int spawn(const char* path, char* const argv[]);
extern int waitpid(int pid);

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
    (void)buf; (void)size; (void)mode;
    return f ? 0 : -1;                            /* SecOS FILE: fixed policy */
}
void clearerr(FILE* f) { if (f) f->flags &= ~(4 | 8); }   /* EOF | error */
FILE* tmpfile(void) {
    static unsigned seq = 0;
    char path[48];
    snprintf(path, sizeof(path), "/tmp/lua_tmp%u_%u", (unsigned)getpid(), seq++);
    return fopen(path, "w+");
}
char* tmpnam(char* s) {
    static char buf[L_tmpnam];
    static unsigned seq = 0;
    char* d = s ? s : buf;
    snprintf(d, L_tmpnam, "/tmp/lua_%u_%u", (unsigned)getpid(), seq++);
    return d;
}
/* os.execute: run the command through /bin/bash -c (signed, from the VFS). */
int system(const char* cmd) {
    if (!cmd) return 1;                           /* a shell is available */
    char* argv[] = { (char*)"bash", (char*)"-c", (char*)cmd, NULL };
    int pid = spawn("/bin/bash", argv);
    if (pid < 0) return -1;
    int st;
    while ((st = waitpid(pid)) == -4) { }          /* retry on EINTR */
    return st < 0 ? -1 : st;                       /* POSIX wait status */
}
double difftime(time_t a, time_t b) { return (double)(a - b); }

/* Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms). */
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
struct tm* gmtime_r(const time_t* t, struct tm* r) {
    long s = (long)*t, days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem % 3600 / 60); r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)(((days % 7) + 11) % 7);    /* 1970-01-01 was a Thursday */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    r->tm_year = (int)(y - 1900); r->tm_mon = (int)m - 1; r->tm_mday = (int)d;
    r->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    r->tm_isdst = 0;
    return r;
}
struct tm* localtime_r(const time_t* t, struct tm* r) { return gmtime_r(t, r); }   /* UTC */
struct tm* gmtime(const time_t* t) { static struct tm r; return gmtime_r(t, &r); }
struct tm* localtime(const time_t* t) { return gmtime(t); }
time_t mktime(struct tm* tm) {
    long y = tm->tm_year + 1900L, mon = tm->tm_mon;
    y += mon / 12; mon %= 12; if (mon < 0) { mon += 12; y--; }
    long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    time_t t = (time_t)(days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
    gmtime_r(&t, tm);                             /* normalize the fields */
    return t;
}
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm) {
    static const char* wd[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char* mn[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    size_t n = 0;
    char tmp[64];
    for (; *fmt; fmt++) {
        const char* add = tmp;
        if (*fmt != '%') { tmp[0] = *fmt; tmp[1] = 0; }
        else {
            fmt++;
            switch (*fmt) {
            case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
            case 'y': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
            case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
            case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
            case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
            case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
            case 'a': add = wd[tm->tm_wday % 7]; break;
            case 'b': case 'h': add = mn[tm->tm_mon % 12]; break;
            case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': case 'X':
                snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'D': case 'x':
                snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
            case 'c': snprintf(tmp, sizeof(tmp), "%s %s %2d %02d:%02d:%02d %d", wd[tm->tm_wday % 7],
                               mn[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                               tm->tm_year + 1900); break;
            case 'Z': add = "UTC"; break;
            case 'z': add = "+0000"; break;
            case 'n': add = "\n"; break;
            case 't': add = "\t"; break;
            case '%': add = "%"; break;
            case 0: fmt--; add = ""; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
            }
        }
        size_t l = strlen(add);
        if (n + l >= max) return 0;
        memcpy(s + n, add, l); n += l;
    }
    if (max) s[n] = 0;
    return n;
}

static void open_safe_libs(lua_State* L) {
    static const luaL_Reg libs[] = {
        { LUA_GNAME,       luaopen_base },
        { LUA_TABLIBNAME,  luaopen_table },
        { LUA_STRLIBNAME,  luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_COLIBNAME,   luaopen_coroutine },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_IOLIBNAME,   luaopen_io },
        { LUA_OSLIBNAME,   luaopen_os },
        { NULL, NULL }
    };
    for (const luaL_Reg* l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
}

static void put(int fd, const char* s) { write(fd, s, strlen(s)); }

/* libc isatty() is true for any fd 0-2; ask the kernel (TCGETS fails on a
 * redirected pipe/file) so `echo code | lua` runs it as a script, no prompt. */
static int stdin_is_tty(void) { struct termios t; return tcgetattr(0, &t) == 0; }

/* Report the error on the stack top (if any) and pop it. */
static int report(lua_State* L, int status) {
    if (status != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (!msg) msg = "(error object is not a string)";
        dprintf(2, "lua: %s\n", msg);
        lua_pop(L, 1);
    }
    return status;
}

/* Message handler: append a traceback to string errors. */
static int msghandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static int docall(lua_State* L, int narg, int nres) {
    int base = lua_gettop(L) - narg;
    lua_pushcfunction(L, msghandler);
    lua_insert(L, base);
    int status = lua_pcall(L, narg, nres, base);
    lua_remove(L, base);
    return status;
}

static int dochunk(lua_State* L, const char* s, size_t len, const char* name) {
    int status = luaL_loadbuffer(L, s, len, name);
    if (status == LUA_OK) status = docall(L, 0, 0);
    return report(L, status);
}

/* Read a whole fd into a malloc'd buffer (scripts, `lua -`). */
static char* slurp(int fd, size_t* outlen) {
    size_t cap = 4096, n = 0;
    char* b = (char*)malloc(cap);
    if (!b) return NULL;
    for (;;) {
        if (n + 1024 > cap) {
            char* nb = (char*)realloc(b, cap * 2);
            if (!nb) { free(b); return NULL; }
            b = nb; cap *= 2;
        }
        int r = (int)read(fd, b + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    b[n] = 0; *outlen = n;
    return b;
}

static int dofile(lua_State* L, const char* path) {
    int fd = (strcmp(path, "-") == 0) ? 0 : open(path, O_RDONLY);
    if (fd < 0) { dprintf(2, "lua: cannot open %s\n", path); return LUA_ERRFILE; }
    size_t n = 0;
    char* buf = slurp(fd, &n);
    if (fd != 0) close(fd);
    if (!buf) { put(2, "lua: out of memory\n"); return LUA_ERRMEM; }
    const char* code = buf;
    if (n > 0 && buf[0] == '#') {                 /* skip a #! line */
        while (*code && *code != '\n') code++;
    }
    char chunkname[160];
    snprintf(chunkname, sizeof(chunkname), "@%s", fd == 0 ? "stdin" : path);
    int st = luaL_loadbuffer(L, code, strlen(code), chunkname);
    free(buf);
    if (st == LUA_OK) {
        /* script arguments are the ... of the main chunk */
        lua_getglobal(L, "arg");
        int nargs = 0;
        if (lua_istable(L, -1)) {
            lua_Integer i;
            for (i = 1; lua_rawgeti(L, -1 - nargs, i) != LUA_TNIL; i++) nargs++;
            lua_pop(L, 1);                        /* the nil */
            lua_remove(L, -1 - nargs);            /* the arg table */
        } else {
            lua_pop(L, 1);
        }
        st = docall(L, nargs, 0);
    }
    return report(L, st);
}

/* ---- REPL ---- */
#define REPL_MAX 8192
static char g_input[REPL_MAX];

/* Read one line from fd 0 into buf. Returns length (>=0, newline stripped),
 * -1 on EOF, -2 when the line was cancelled (Ctrl-C / EINTR). */
static int read_line(char* buf, int cap) {
    int n = 0;
    for (;;) {
        char c;
        int r = (int)read(0, &c, 1);
        if (r < 0) {
            if (errno == EINTR) return -2;
            return n ? n : -1;
        }
        if (r == 0) { if (n == 0) return -1; break; }
        if (c == '\n') break;
        if (n < cap - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static int incomplete(lua_State* L, int status) {
    if (status == LUA_ERRSYNTAX) {
        size_t lmsg;
        const char* msg = lua_tolstring(L, -1, &lmsg);
        const char* mark = "<eof>";
        size_t lm = strlen(mark);
        if (lmsg >= lm && strcmp(msg + lmsg - lm, mark) == 0) {
            lua_pop(L, 1);
            return 1;
        }
    }
    return 0;
}

static void print_results(lua_State* L) {
    int n = lua_gettop(L);
    if (n > 0) {
        luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
        lua_getglobal(L, "print");
        lua_insert(L, 1);
        if (lua_pcall(L, n, 0, 0) != LUA_OK) {
            dprintf(2, "lua: error calling 'print' (%s)\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

static void repl(lua_State* L) {
    put(1, LUA_COPYRIGHT "\n");
    for (;;) {
        fflush(stdout);                                 /* io.write output before the prompt */
        put(1, "> ");
        int len = read_line(g_input, REPL_MAX);
        if (len == -1) { put(1, "\n"); break; }        /* EOF (Ctrl-D) */
        if (len == -2) { put(1, "\n"); continue; }      /* cancelled line */
        if (len == 0) continue;
        lua_settop(L, 0);
        /* expressions first: `return <line>` */
        char* ret = (char*)malloc((size_t)len + 8);
        int status = LUA_ERRSYNTAX;
        if (ret) {
            memcpy(ret, "return ", 7); memcpy(ret + 7, g_input, (size_t)len + 1);
            status = luaL_loadbuffer(L, ret, strlen(ret), "=stdin");
            free(ret);
            if (status != LUA_OK) lua_pop(L, 1);
        }
        if (status != LUA_OK) {
            /* a statement, possibly spanning several lines */
            int total = len;
            for (;;) {
                status = luaL_loadbuffer(L, g_input, (size_t)total, "=stdin");
                if (!incomplete(L, status)) break;
                if (total + 2 >= REPL_MAX) break;
                put(1, ">> ");
                g_input[total++] = '\n';
                int more = read_line(g_input + total, REPL_MAX - total);
                if (more == -1 || more == -2) { status = -1; break; }
                total += more;
            }
            if (status == -1) { put(1, "\n"); lua_settop(L, 0); continue; }
        }
        if (status == LUA_OK) status = docall(L, 0, LUA_MULTRET);
        if (status == LUA_OK) print_results(L);
        else report(L, status);
        lua_settop(L, 0);
    }
}

int main(int argc, char** argv) {
    lua_State* L = luaL_newstate();
    if (!L) { put(2, "lua: cannot create state (out of memory)\n"); return 1; }
    open_safe_libs(L);

    /* argc==0: started by the kernel without argv (M34_LUA_DEMO gate) */
    if (argc < 1 || (argc == 2 && strcmp(argv[1], "--demo") == 0)) {
        int rc = dochunk(L, DEMO, strlen(DEMO), "=demo") == LUA_OK ? 0 : 1;
        lua_close(L);
        return rc;
    }

    /* Parse options up to the script name. */
    int script = 0, want_repl = 0, ran = 0, i;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) { put(2, "lua: '-e' needs argument\n"); lua_close(L); return 1; }
            i++;                                   /* run after `arg` is built */
        } else if (strcmp(a, "-i") == 0) { want_repl = 1;
        } else if (strcmp(a, "-v") == 0) { put(1, LUA_COPYRIGHT "\n"); ran = 1;
        } else if (strcmp(a, "--") == 0) { if (i + 1 < argc) script = i + 1; break;
        } else if (a[0] == '-' && a[1] != 0) {
            dprintf(2, "lua: unrecognized option '%s'\n"
                       "usage: lua [-e stat] [-i] [-v] [script [args]]\n", a);
            lua_close(L); return 1;
        } else { script = i; break; }
    }

    /* arg table: arg[0] = script, arg[1..] = its arguments, negatives = options */
    lua_createtable(L, argc, 1);
    int base = script ? script : argc;
    for (int k = 0; k < argc; k++) {
        lua_pushstring(L, argv[k]);
        lua_rawseti(L, -2, k - base);
    }
    lua_setglobal(L, "arg");

    int rc = 0;
    int end = script ? script : argc;
    for (i = 1; i < end; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            const char* chunk = argv[++i];
            if (dochunk(L, chunk, strlen(chunk), "=(command line)") != LUA_OK) { lua_close(L); return 1; }
            ran = 1;
        }
    }
    if (script) {
        if (dofile(L, argv[script]) != LUA_OK) rc = 1;
        ran = 1;
    }
    if (want_repl) repl(L);
    else if (!ran) {
        if (stdin_is_tty()) repl(L);
        else if (dofile(L, "-") != LUA_OK) rc = 1;
    }
    lua_close(L);
    return rc;
}
