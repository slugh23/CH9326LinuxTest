/*
 * Lua 5.4 binding for the CH9326 USB-HID to UART bridge (lib/ch9326.c).
 *
 *   local ch9326 = require "ch9326"
 *   local dev = assert(ch9326.open())          -- first device found
 *   dev:configure{baud = 115200, parity = "none", stop = 1, bits = 8}
 *   dev:write("hello")
 *   local s = dev:read(16, 0.5)                -- up to 16 bytes, wait <= 0.5 s
 *   dev:close()
 *
 * Errors that are part of normal use (no device, write failed) are returned as
 * nil, message; misuse (bad arguments, closed device) raises a Lua error.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <time.h>

#include <lua.h>
#include <lauxlib.h>

#include "ch9326.h"

#define DEVICE_MT       "ch9326.device"
#define MAX_DEVICES     16
#define READ_POLL_NS    2000000L        /* 2 ms between buffer checks in read() */
#define STRING_BUF      256

typedef struct {
    int index;          /* 0-based slot in lib/ch9326.c */
    int open;
} device;

static int slot_open[MAX_DEVICES];      /* lib/ch9326.c allows one open per device */

static device *check_device(lua_State *L)
{
    device *d = luaL_checkudata(L, 1, DEVICE_MT);
    if (!d->open)
        luaL_error(L, "attempt to use a closed ch9326 device");
    return d;
}

static double monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_seconds(double s)
{
    struct timespec ts;
    if (s <= 0)
        return;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* ch9326.find() -> number of CH9326 devices on the bus */
static int l_find(lua_State *L)
{
    lua_pushinteger(L, ch9326_find());
    return 1;
}

/* ch9326.open([index]) -> device | nil, err   (index is 1-based, default 1) */
static int l_open(lua_State *L)
{
    lua_Integer index = luaL_optinteger(L, 1, 1);
    int count;
    device *d;

    luaL_argcheck(L, index >= 1 && index <= MAX_DEVICES, 1, "index must be 1..16");
    count = ch9326_find();
    if (count < index) {
        lua_pushnil(L);
        lua_pushfstring(L, "no CH9326 device #%d (found %d)", (int)index, count);
        return 2;
    }
    if (slot_open[index - 1]) {
        lua_pushnil(L);
        lua_pushfstring(L, "CH9326 device #%d is already open", (int)index);
        return 2;
    }
    if (!ch9326_open((unsigned char)(index - 1))) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot open the CH9326 (permissions? see 99-ch9326.rules)");
        return 2;
    }
    d = lua_newuserdatauv(L, sizeof(*d), 0);
    d->index = (int)(index - 1);
    d->open = 1;
    slot_open[d->index] = 1;
    luaL_setmetatable(L, DEVICE_MT);
    return 1;
}

/* dev:configure{baud=, parity=, stop=, bits=, interval=} -> true | nil, err */
static int l_configure(lua_State *L)
{
    static const lua_Integer bauds[] = {
        300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 76800, 115200
    };
    static const char *parities[] = {"odd", "even", "space", "none", NULL};
    device *d = check_device(L);
    lua_Integer baud, stop, bits, interval;
    int rate = 0, parity;

    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 2, "baud");
    baud = luaL_optinteger(L, -1, 9600);
    lua_getfield(L, 2, "parity");
    {
        const char *p = luaL_optstring(L, -1, "none");
        parity = 0;
        for (int i = 0; parities[i]; i++)
            if (strcmp(p, parities[i]) == 0)
                parity = P_ODD + i;                             /* P_ODD .. P_NONE */
        if (!parity)
            return luaL_error(L, "parity must be \"odd\", \"even\", \"space\" or \"none\"");
    }
    lua_getfield(L, 2, "stop");
    stop = luaL_optinteger(L, -1, 1);
    lua_getfield(L, 2, "bits");
    bits = luaL_optinteger(L, -1, 8);
    lua_getfield(L, 2, "interval");
    interval = luaL_optinteger(L, -1, 0x10);
    lua_pop(L, 5);

    for (int i = 0; i < (int)(sizeof(bauds) / sizeof(bauds[0])); i++)
        if (bauds[i] == baud)
            rate = B300 + i;
    if (!rate)
        return luaL_error(L, "unsupported baud rate %d", (int)baud);
    luaL_argcheck(L, stop == 1 || stop == 2, 2, "stop must be 1 or 2");
    luaL_argcheck(L, bits >= 5 && bits <= 8, 2, "bits must be 5..8");
    luaL_argcheck(L, interval >= 0 && interval <= 255, 2, "interval must be 0..255");

    if (!ch9326_set((unsigned char)d->index, (unsigned char)rate, (unsigned char)parity,
                    stop == 1 ? STOP_1 : STOP_2, (unsigned char)(BIT_5 + bits - 5),
                    (unsigned char)interval)) {
        lua_pushnil(L);
        lua_pushstring(L, "configure failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* dev:write(s) -> bytes sent | nil, err */
static int l_write(lua_State *L)
{
    device *d = check_device(L);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    unsigned long sent;

    if (len == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    sent = ch9326_send((unsigned char)d->index, (unsigned char *)s, len);
    if (sent == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "write failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)sent);
    return 1;
}

/* dev:read(n [, timeout]) -> string of up to n bytes. Waits up to timeout
 * seconds (default 0 = return what is already buffered). */
static int l_read(lua_State *L)
{
    device *d = check_device(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    double timeout = luaL_optnumber(L, 3, 0);
    double deadline = monotonic() + timeout;
    luaL_Buffer b;
    char *p;
    size_t got = 0;

    luaL_argcheck(L, n >= 0, 2, "count must be >= 0");
    p = luaL_buffinitsize(L, &b, (size_t)n);
    while (got < (size_t)n) {
        got += ch9326_recv((unsigned char)d->index, p + got, (unsigned long)(n - got));
        if (got >= (size_t)n || monotonic() >= deadline)
            break;
        sleep_seconds(READ_POLL_NS / 1e9);
    }
    luaL_pushresultsize(&b, got);
    return 1;
}

/* dev:flush() -> number of buffered bytes discarded */
static int l_flush(lua_State *L)
{
    device *d = check_device(L);
    char buf[256];
    lua_Integer total = 0;
    unsigned long n;

    while ((n = ch9326_recv((unsigned char)d->index, buf, sizeof(buf))) > 0)
        total += (lua_Integer)n;
    lua_pushinteger(L, total);
    return 1;
}

/* dev:gpio_direction(mask)  bit n = IO(n+1); 1 = output */
static int l_gpio_direction(lua_State *L)
{
    device *d = check_device(L);
    lua_pushboolean(L, ch9326_set_gpiodir((unsigned char)d->index,
                                          (unsigned char)luaL_checkinteger(L, 2)));
    return 1;
}

/* dev:gpio_write(mask)  bit n = IO(n+1); 1 = high */
static int l_gpio_write(lua_State *L)
{
    device *d = check_device(L);
    lua_pushboolean(L, ch9326_set_gpiodata((unsigned char)d->index,
                                           (unsigned char)luaL_checkinteger(L, 2)));
    return 1;
}

/* dev:gpio_read() -> raw input byte (per WCH docs bit 5 = IO1, bit 3 = IO2) | nil, err */
static int l_gpio_read(lua_State *L)
{
    device *d = check_device(L);
    char v;

    if (!ch9326_get_gpio((unsigned char)d->index, &v)) {
        lua_pushnil(L);
        lua_pushstring(L, "gpio read failed");
        return 2;
    }
    lua_pushinteger(L, (unsigned char)v);
    return 1;
}

/* dev:connected() -> true while open and not unplugged */
static int l_connected(lua_State *L)
{
    device *d = luaL_checkudata(L, 1, DEVICE_MT);
    lua_pushboolean(L, d->open && ch9326_connected((unsigned char)d->index));
    return 1;
}

/* Raw USB string descriptor -> plain string (UTF-16LE, non-ASCII as '?') */
static void push_descriptor(lua_State *L, const unsigned char *desc)
{
    luaL_Buffer b;
    int len = desc[0];

    luaL_buffinit(L, &b);
    for (int i = 2; i + 1 < len; i += 2)
        luaL_addchar(&b, desc[i + 1] == 0 && desc[i] >= 32 && desc[i] < 127 ? (char)desc[i] : '?');
    luaL_pushresult(&b);
}

/* dev:strings() -> manufacturer, product, serial  (nil for any that fail) */
static int l_strings(lua_State *L)
{
    unsigned char (*const getters[])(unsigned char, unsigned char *, unsigned long) = {
        ch9326_get_manufacturer_string, ch9326_get_product_string,
        ch9326_get_serial_number_string,
    };
    device *d = check_device(L);

    for (int i = 0; i < 3; i++) {
        unsigned char desc[STRING_BUF] = {0};
        if (getters[i]((unsigned char)d->index, desc, sizeof(desc)) && desc[0] >= 2)
            push_descriptor(L, desc);
        else
            lua_pushnil(L);
    }
    return 3;
}

/* dev:close()  (also called by __gc and __close) */
static int l_close(lua_State *L)
{
    device *d = luaL_checkudata(L, 1, DEVICE_MT);

    if (d->open) {
        ch9326_close((unsigned char)d->index);
        d->open = 0;
        slot_open[d->index] = 0;
    }
    return 0;
}

static int l_tostring(lua_State *L)
{
    device *d = luaL_checkudata(L, 1, DEVICE_MT);
    lua_pushfstring(L, "ch9326.device #%d (%s)", d->index + 1, d->open ? "open" : "closed");
    return 1;
}

/* ch9326.sleep(seconds) */
static int l_sleep(lua_State *L)
{
    sleep_seconds(luaL_checknumber(L, 1));
    return 0;
}

/* ch9326.monotonic() -> seconds from a monotonic clock */
static int l_monotonic(lua_State *L)
{
    lua_pushnumber(L, monotonic());
    return 1;
}

static const luaL_Reg device_methods[] = {
    {"configure", l_configure},
    {"write", l_write},
    {"read", l_read},
    {"flush", l_flush},
    {"gpio_direction", l_gpio_direction},
    {"gpio_write", l_gpio_write},
    {"gpio_read", l_gpio_read},
    {"connected", l_connected},
    {"strings", l_strings},
    {"close", l_close},
    {NULL, NULL}
};

static const luaL_Reg module_functions[] = {
    {"find", l_find},
    {"open", l_open},
    {"sleep", l_sleep},
    {"monotonic", l_monotonic},
    {NULL, NULL}
};

int luaopen_ch9326(lua_State *L)
{
    luaL_newmetatable(L, DEVICE_MT);
    luaL_newlib(L, device_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_close);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_close);
    lua_setfield(L, -2, "__close");
    lua_pushcfunction(L, l_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_newlib(L, module_functions);
    lua_pushliteral(L, "1.0");
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
