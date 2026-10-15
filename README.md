# foma

**F**m-**O**ne **M**akers' **A**ssistant

Write firmware for the M-VAVE FM-1 with nothing installed but Docker. Edit one C file, hear it in the browser emulator
as you save, install it on the FM-1 from the same page.

```sh
./fm1 dev          # open http://localhost:8000
```

That pulls the toolchain image ([konsumer/foma-toolchain](https://hub.docker.com/r/konsumer/foma-toolchain)) the first time, then builds your firmware and serves a page with:

- **Emulator**: your code, compiled to WebAssembly, on a virtual FM-1 panel: the screen, keys, buttons, knobs, LEDs,
  sound, MIDI in/out (Web MIDI), settings, `print()` to a console. Save a file and it reloads; a compile error shows on
  the page.
- **Install**: puts your build (or any FM-1 `.fwsc`) on the FM-1 over USB, from Chrome or Edge.
- **Rescue**: the USB rescue, finishing a cut-off install, going back to the official firmware.

It is [Dinghy](https://github.com/zvenson/dinghy) made smaller: the same proven FM-1 platform (Felucca's, via SLOOP)
under a smaller API, a Docker toolchain, and a web emulator in the spirit of
[X0X](https://github.com/charlesvestal/fm1-x0x)'s.

## Your firmware

`src/main.c` (and any other `.c` in `src/`). Three functions, like an Arduino sketch:

```c
#include "fm1.h"

static uint32_t phase;
static volatile uint32_t inc;         /* written by loop(), read by audio() */

void setup(void) {                       /* once, at power-on */
  clear(BLACK);
  text(80, 100, "HELLO", WHITE, FONT_BIG);
}

void loop(void) {                        /* over and over: read the panel, draw what changed */
  inc = key_held(0) ? note_inc(KEY_FIRST_NOTE) : 0;
}

void audio(int32_t *out, uint32_t frames) {   /* the sound: add to out, L R L R, +-32767 */
  for (uint32_t i = 0; i < frames; i++) {
    phase += inc;
    int32_t s = inc ? sine(phase) / 4 : 0;
    out[2 * i] += s;
    out[2 * i + 1] += s;
  }
}
```

Everything you can call is in [platform/fm1.h](platform/fm1.h): keys (`key_pressed`, `key_held`, ...), buttons
(`BTN_PLAY`, ...), encoders (`encoder(ENC_KNOB1)`), LEDs, the screen (`clear`, `rect`, `line`, `pixel`, `text`),
MIDI (`midi_read`, `midi_send`), `settings_load` / `settings_save`, `millis`, and sound helpers (`sine`, `note_inc`,
`random32`).

Name, version and package identity: [firmware.json](firmware.json).

## The rules of the chip

The FM-1 runs a JieLi AC791N. The emulator is forgiving, the chip is not; the build refuses what would break it.

- **Integers only.** No FPU and no float library: `float` / `double` fail the FM-1 build (the emulator still runs, and
  the page shows why). Use fixed point: Q15 levels, 32-bit phases.
- **No 64-bit division**, no C library (`printf`, `malloc`, `sin`). 64-bit multiplication is fine.
- **A division by zero does not trap** on the FM-1 (it gives a wrong value); in the emulator it crashes. Test divisors.
- **`audio()` has 5.8 ms per 256 frames**, less in practice. No screen, no flash, no waiting in it. Share with `loop()`
  through `volatile` variables, one writer each.
- **`loop()` should return within a few ms**: the USB, the updater and the watchdog run between passes.
- **Draw only what changed**: sending a full screen takes ~80 ms.
- **Save settings when silent**: a flash erase stops the sound for ~50 ms. Never in a loop: flash wears out.
- **Memory**: 96 KiB RAM for your variables; mark big buffers `POOL` (~200 KiB more). `const` tables live in flash.
- The build also refuses constants that look like a chip-ROM address, and hardware register access outside
  `platform/hal/`.

## Commands

|               |                                                                                      |
| ------------- | ------------------------------------------------------------------------------------ |
| `./fm1 dev`   | build, serve on http://localhost:8000 (`PORT=9000 ./fm1 dev`), rebuild on every save |
| `./fm1 build` | `build/web/` (the site) and `build/NAME-VERSION.fwsc` (the package)                  |
| `./fm1 fmt`   | format: clang-format (C), prettier (web), ruff (python)                              |
| `./fm1 shell` | a shell in the toolchain container                                                   |
| `./fm1 image` | build the toolchain image from the `Dockerfile` here, instead of pulling it          |

Windows: `fm1.cmd dev` (Docker Desktop). Without the script:
`docker run --rm -it --platform linux/amd64 -p 8000:8000 -v "$PWD:/work" konsumer/foma-toolchain python3 tools/build.py --dev 8000`.

## Publishing

Push to GitHub and set **Settings → Pages → Source: GitHub Actions**: every push to `main` builds the firmware and
publishes the emulator and the installer, so anyone can try your firmware and install it with a link. A tag (`v0.2`)
also makes a release with the `.fwsc`.

The toolchain image has its own workflow, `.github/workflows/toolchain.yml`: when the `Dockerfile` changes on `main`
(or when run by hand) it builds the image, checks that it builds the firmware, and pushes it to Docker Hub. It needs the
repo secrets `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN`.

**Using this as a template:** copy it (GitHub's "Use this template", or clone it), then make it yours once:

```sh
./fm1 new "MY SYNTH" --identity FM-1_96      # --repo URL too, if there is no git remote "origin" yet
```

That renames the firmware, points the page's Source link at your repo, writes your README, adds you to
LICENSING.md, and removes what only foma needs (the `Dockerfile` and its workflow: your project uses the published
`konsumer/foma-toolchain`).

Pick your own `identity` in `firmware.json` before you share a build: the FM-1 and the installers tell firmwares apart
by it. Taken so far: `900` SLOOP, `91N` Felucca, `93N` sloopDX, `97N` zp12, `98N` a Doom port, `99N` Dinghy; check the
[FM-1 Firmware Hub](https://fm1.designburgapps.com).

## When it goes wrong

- **It crashes**: a red screen with the address (look it up in `build/fm1/firmware.dis`), then a restart. Crashing
  twice at boot sends it to the USB rescue by itself.
- **It won't start**: hold **OCT-** while switching on: the USB rescue (no sound, only the updater). Install from the
  page.
- **An install was cut off**: the FM-1 waits in update mode; the Rescue tab finishes it.
- **Back to stock**: the Rescue tab installs M-VAVE's V15 (checked by its hash first).
- **Nothing answers**: the chip's boot ROM can still be reached from a computer, not a browser: see the Rescue tab.

Installing firmware is at your own risk.

## Layout

```
src/main.c            your firmware
firmware.json         name, version, identity
platform/fm1.h        the API
platform/fm1.c        the FM-1 side (boot, rescue, updater, USB, panel, screen, audio)
platform/web.c        the emulator side (the same API, for WebAssembly)
platform/common.c     shared: drawing, fonts, sound helpers
platform/hal, src, loader, gen, sdk   the Felucca platform, vendored unchanged
tools/build.py        the build (in Docker), with the checks
web/                  the page: emulator, installer, rescue
```

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
