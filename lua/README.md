# ch9326 — Lua 5.4 module for the WCH CH9326

A small Lua C module around the portable `lib/ch9326.c`. It gives Lua scripts a
serial port over the CH9326 USB-HID bridge (`1a86:e010`), plus the chip's GPIO pins.
It works on x86 and ARM (Raspberry Pi).

## Build

```bash
sudo apt install liblua5.4-dev libusb-1.0-0-dev pkg-config
make                    # -> ch9326.so
sudo make install       # optional: -> /usr/local/lib/lua/5.4/
```

Without installing it, point Lua at this folder:
`LUA_CPATH="/path/to/CH9326LinuxTest/lua/?.so;;" lua5.4 script.lua`.

For access without root, install `../99-ch9326.rules` (see the comments in that file).

## Example

```lua
local ch9326 = require "ch9326"

local dev = assert(ch9326.open())               -- first CH9326 found
dev:configure{baud = 115200, parity = "none", stop = 1, bits = 8}
dev:write("\xA5\x00\xA5")
local reply = dev:read(11, 0.4)                 -- up to 11 bytes, wait at most 0.4 s
print(#reply, dev:strings())                    -- 11  WCH.CN 4  HID To Serial  12345678
dev:close()
```

## API

| Function | Description |
|---|---|
| `ch9326.find()` | Number of CH9326 devices on the bus |
| `ch9326.open([index])` | Open device `index` (1-based, default 1). Returns a device, or `nil, message` |
| `dev:configure{baud, parity, stop, bits, interval}` | UART setup. `baud`: 300 … 115200 (the chip's 13 rates); `parity`: `"none"`/`"odd"`/`"even"`/`"space"`; `stop`: 1/2; `bits`: 5–8; `interval`: receive packing timeout (default `0x10`). Defaults: 9600 8N1 |
| `dev:write(s)` | Send bytes. Returns the count sent, or `nil, message` |
| `dev:read(n [, timeout])` | Up to `n` received bytes, waiting at most `timeout` seconds (default 0: only what's already buffered) |
| `dev:flush()` | Discard buffered input; returns the number of bytes dropped |
| `dev:gpio_direction(mask)` | Bit n = IO(n+1); 1 = output |
| `dev:gpio_write(mask)` | Bit n = IO(n+1); 1 = high |
| `dev:gpio_read()` | Raw input byte (per WCH: bit 5 = IO1, bit 3 = IO2), or `nil, message` |
| `dev:strings()` | USB manufacturer, product, serial (ASCII) |
| `dev:connected()` | `true` while open and not unplugged |
| `dev:close()` | Close. Also runs on garbage collection and for `local dev <close> = …` |
| `ch9326.sleep(s)`, `ch9326.monotonic()` | Sleep / monotonic clock in seconds (plain Lua has neither) |

Received data is buffered in the background by `lib/ch9326.c` (8 KiB), so `read` can
be called at any time. A device can be open only once at a time.

Tested on Lua 5.4 against a real CH9326 (an SLM-25 sound level meter).
See https://github.com/slugh23/slm25-linux for a protocol library built on it.
