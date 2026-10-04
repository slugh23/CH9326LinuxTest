# Build the ch9326 Lua 5.4 module (ch9326.so) from lib/ch9326.c and the binding.
#
#   sudo apt install liblua5.4-dev libusb-1.0-0-dev pkg-config
#   make            # -> ch9326.so
#   make install    # -> $(LUA_CDIR) (default /usr/local/lib/lua/5.4)
#
# Or skip installing and point Lua at this folder:
#   LUA_CPATH="/path/to/lua/?.so;;" lua5.4 example.lua

LUA_VERSION ?= 5.4
LUA_CFLAGS  ?= $(shell pkg-config --cflags lua$(LUA_VERSION) 2>/dev/null || echo -I/usr/include/lua$(LUA_VERSION))
USB_CFLAGS  ?= $(shell pkg-config --cflags libusb-1.0)
USB_LIBS    ?= $(shell pkg-config --libs libusb-1.0)
LUA_CDIR    ?= /usr/local/lib/lua/$(LUA_VERSION)

CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -fPIC -std=c99 -I../lib $(LUA_CFLAGS) $(USB_CFLAGS)

ch9326.so: lch9326.c ../lib/ch9326.c ../lib/ch9326.h
	$(CC) $(CFLAGS) -shared -o $@ lch9326.c ../lib/ch9326.c $(USB_LIBS) -pthread

install: ch9326.so
	install -d $(DESTDIR)$(LUA_CDIR)
	install -m 0755 ch9326.so $(DESTDIR)$(LUA_CDIR)/

clean:
	rm -f ch9326.so

.PHONY: install clean
