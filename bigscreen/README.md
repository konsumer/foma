# Big Screen

A small device's screen, shown big on something else (a computer, a phone, [fm1buddy](https://github.com/konsumer/fm1buddy)), over USB MIDI SysEx. Any MIDI connection works, so a web page with Web MIDI can be a viewer.

[`bigscreen.h`](bigscreen.h) has both ends in one C file: the device side (it sends the screen) and the viewer side (it asks for the screen and draws it). It's C99, needs only `stdint.h`, uses no malloc and no libc, and the comment at its top shows how to use it. [`test.c`](test.c) tests it: `cc -O2 -o /tmp/bstest test.c && /tmp/bstest`.

Version 2 is new. Version 1 is [pfkellogg's screen_mirror](https://github.com/pfkellogg/fm1_open), which [FM-1 Mirror](https://github.com/pfkellogg/fm1-mirror) speaks. A device using `bigscreen.h` speaks both, and a viewer using it asks for version 2 and falls back to version 1 by itself.

## Why version 2

On the FM-1 (240 x 240 pixels, about 48 KB/s of SysEx out), playing fomajam for 30 s and simulated against its USB budget:

|                      | frames the viewer got whole | average lag | data per frame |
| -------------------- | --------------------------- | ----------- | -------------- |
| version 1            | 74%                         | 14 ms       | 1.8 KB         |
| version 2            | 98%                         | 7 ms        | 0.7 KB         |
| version 1, slow host | 40%                         | 18 ms       | 1.4 KB         |
| version 2, slow host | 87%                         | 12 ms       | 0.7 KB         |

("Slow host" is a USB host that takes one packet a millisecond.)

Version 1, as FM-1 firmware has sent it, sends every pixel row that was drawn, all 240 pixels of it, at 4 bytes for each run of one color. Version 2 cuts the screen into 16 x 16 tiles and keeps a checksum of each (4 bytes a tile, never a copy of the screen). It sends only the tiles that changed, compressed with a small color cache, so text and anti-aliased edges cost about a byte a pixel and flat areas almost nothing. It also says when a frame is complete, so a viewer can show whole frames instead of half-drawn ones.

## The protocol

Every message is a SysEx: `F0 7D 'B' 'S' <cmd> ... F7` (`7D` is the non-commercial ID, `'B' 'S'` is `42 53`). Every byte between F0 and F7 is 7-bit. Numbers marked (14) take two bytes, low 7 bits first: `v & 127, v >> 7`.

### Viewer to device

**HELLO** `F0 7D 'B' 'S' 00 <version> <flags> F7`

- `version`: the highest version the viewer speaks (2).
- `flags` bit 0: send all of the screen again.

Send it about once a second. The device stops sending 3 s after the last hello, so nothing goes out when nobody's looking. Each hello is also the question "do you speak version 2?": a device that does answers every hello with INFO. Firmware that only knows version 1 ignores it (it listens for `'F' 'S'`). After 3 hellos with no INFO, use version 1. While using version 1, ask again now and then (`bigscreen.h` uses every 10th hello), so a firmware update brings the viewer up to version 2 by itself.

### Device to viewer

**INFO** `F0 7D 'B' 'S' 01 <version> <w (14)> <h (14)> <format> F7`, the answer to each hello. `version` is the version being sent (2), `w` and `h` are the screen's size, and `format` is the pixel format (0: RGB565).

**RECT** `F0 7D 'B' 'S' 02 <x (14)> <y (14)> <w (14)> <h (14)> <pixels...> F7` fills a rectangle. Its `w * h` pixels go row by row, left to right, in this code:

| byte                     | meaning                                                                          |
| ------------------------ | -------------------------------------------------------------------------------- |
| `00iiiiii`               | the color in cache slot `i`                                                      |
| `010nnnnn`               | the last color again, `n + 1` times (1 to 32)                                    |
| `0110nnnn` `b`           | the last color again, `(n << 7 \| b) + 33` times (33 to 2080)                    |
| `011100hh` `b1` `b2`     | a new color: `c = hh << 14 \| b1 << 7 \| b2` (RGB565). Then `cache[hash(c)] = c` |
| `0111 01xx`, `0111 1xxx` | not used in version 2: stop reading the message                                  |

`hash(c) = (r * 3 + g * 5 + b * 7) & 63`, where `r = c >> 11`, `g = (c >> 5) & 63` and `b = c & 31`.

Each message starts afresh: the cache is 64 zeros, and "the last color" is 0 (black). A run can go on past the end of a row into the next row. Every message stands alone, so one that's lost costs only its own rectangle.

**SYNC** `F0 7D 'B' 'S' 03 F7`: everything that had changed has now been sent. A viewer that shows the screen only now and then can do so here, and get no half-drawn frames. A screen that changes faster than it can be sent may not get to a SYNC for a while, so show what has come anyway when none arrives for about 60 ms.

A message is at most 256 bytes (`BS_MSG`; the FM-1 uses 128 so a note waits less behind one). A viewer needs room for that.

### Version 1 (pfkellogg's screen_mirror)

- Hello: `F0 7D 'F' 'S' <k> F7`. `k` is 0 for a hello, 1 for a hello plus "all again", 2 for the sound's name (not sent by `bigscreen.h`).
- Rows: `F0 7D 'F' 'S' 01 <row (14)> <col (14)> {<count> <c & 127> <c >> 7 & 127> <c >> 14>}... F7`, at most 70 bytes.

Each run is `count` pixels of color `c`, from `col` along the row. A device speaking version 2 to one viewer doesn't change to version 1 for another one.

## On the device

```c
#define BIGSCREEN_IMPLEMENTATION
#define BS_NO_VIEWER          // (only the device's half)
#include "bigscreen.h"

static bs_dev big;
bs_dev_init(&big, fb, 240, 240, 240, 0);    // RGB565 pixels, w, h, stride; 1 if they're stored byte-swapped
// each SysEx that comes in:
bs_dev_sysex(&big, msg, len, millis());
// after drawing: the area drawn (cheap when nobody's looking)
bs_dev_changed(&big, x0, y0, x1, y1);
// the main loop, while there's room for BS_MSG bytes:
const uint8_t *m;
uint32_t n;
while (room() && (n = bs_dev_next(&big, millis(), &m))) send_sysex(m, n);
```

RAM: about 1.5 KB for a 240 x 240 screen (`BS_MAX_TILES`, `BS_MSG` and `BS_MAX_W` set it). The checksums cost some time only when something's drawn and a viewer is looking: a 32-bit FNV-1a of each tile in the rows drawn.

Send the screen's SysEx after your other MIDI, a whole message at a time (a note can't go in the middle of a SysEx). MIDI clock and other realtime bytes can, so they needn't wait.

## On the viewer

```c
#define BIGSCREEN_IMPLEMENTATION
#define BS_NO_DEVICE
#include "bigscreen.h"

static uint16_t screen[240 * 240];
static bs_view view;
bs_view_init(&view, screen, 240, 240);
// every 100 ms or so:
uint8_t hello[BS_HELLO_MAX];
uint32_t n = bs_view_hello(&view, millis(), hello);
if (n) send_sysex(hello, n);
// each SysEx that comes in:
bs_rect r;
switch (bs_view_sysex(&view, msg, len, &r)) {
  case BS_DRAW: /* r changed in screen[] */ break;
  case BS_FRAME: /* a whole frame is in: show it */ break;
  case BS_INFO: /* view.w x view.h is the device's screen */ break;
}
```

Call `bs_view_reset` when a device is plugged in. It then asks which version again.
