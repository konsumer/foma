#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build everything from src/ (run it in the toolchain container: ./fm1 build).

  tools/build.py              build/web/: the site (emulator, installer, rescue) with
                              firmware.wasm (the emulator) and firmware.fwsc (the FM-1 package)
  tools/build.py --dev PORT   the same, then serve build/web on PORT and rebuild on every change

The FM-1 part is Dinghy's / Felucca's build (Leo Kuroshita, Hügelton Instruments; zvenson):
the app, the update loader, the checks that keep a build from bricking the FM-1, the package.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAT = ROOT / "platform"
SRC = ROOT / "src"
WEB = ROOT / "web"
OUT = ROOT / "build"
FW = OUT / "fm1"
LDR = OUT / "loader"
SITE = OUT / "web"
sys.path.insert(0, str(ROOT / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402
import mvpack  # noqa: E402

TC = Path(os.environ.get("JIELI_TOOLCHAIN", "/opt/jieli/toolchain"))
APP_XIP = 0x02000120  # app.bin offset 0 in the XIP map; the SPL jumps here
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"  # the file name the SPL looks for
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
INCLUDES = ["-Iplatform", "-Iplatform/hal", "-Iplatform/src", "-Iplatform/gen"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")


class BuildError(Exception):
  pass


def run(cmd):
  r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
  if r.returncode:
    raise BuildError((r.stdout + r.stderr).strip() or f"{cmd[0]} failed")
  if r.stderr.strip():
    print(r.stderr.rstrip(), file=sys.stderr)
  return r.stdout


def rel(a):
  return str(Path(a).resolve().relative_to(ROOT)) if isinstance(a, Path) else a


def tc(tool, *args):
  """a JieLi toolchain binary: cc, ld, objcopy, objdump"""
  args = [rel(a) for a in args]
  if tool == "cc":
    return run([str(TC / "pi32v2/bin/clang"), "-target", "pi32v2", *args])
  if tool == "ld":
    return run([str(TC / "pi32v2/bin/ld"), *args])
  return run([str(TC / "common/bin" / tool), *args])


def tc_all(*cmds):
  with ThreadPoolExecutor(len(cmds)) as ex:
    return list(ex.map(lambda c: tc(*c), cmds))


# firmware.json "options": the platform's parts you can leave out (all in unless set false), as FOMA_* in fm1.h.
# bluetooth needs the "mvave" platform (and is on there unless set false)
OPTIONS = {"usb_audio": "FOMA_USB_AUDIO", "trs_midi": "FOMA_TRS_MIDI", "midi_clock": "FOMA_MIDI_CLOCK", "bluetooth": "FOMA_BLUETOOTH"}
# firmware.json "platform": what runs under your code on the FM-1
#   felucca  Felucca's platform (the default): all source, Felucca's flash layout and updater, the USB rescue
#   mvave    M-VAVE's FM1 SDK (platform/mvave, its binary library): Bluetooth, M-VAVE's layout and updater
PLATFORMS = ("felucca", "mvave")


def sources():
  return sorted(SRC.glob("**/*.c"))


def config():
  c = json.loads((ROOT / "firmware.json").read_text())
  m = re.fullmatch(r"(\d+)\.(\d+)(\.\d+)?(-[A-Za-z0-9]+)?", c.get("version", ""))
  if not m:
    raise BuildError('firmware.json: "version" must look like 0.1, 1.2.3 or 0.1-beta')
  if not re.fullmatch(r"[A-Z0-9 _.+-]{1,16}", c.get("name", "")):
    raise BuildError('firmware.json: "name" must be 1..16 capitals, digits, spaces or _.+-')
  c["platform"] = c.get("platform", "felucca")
  if c["platform"] not in PLATFORMS:
    raise BuildError(f'firmware.json: "platform" must be one of {", ".join(PLATFORMS)}')
  if c["platform"] == "mvave":  # M-VAVE's updater takes FM-1_8 and six digits: the version's major and minor
    c["product"] = f"FM-1_8{int(m[1]) % 1000:03d}{int(m[2]) % 1000:03d}"
  else:
    c["product"] = c["identity"] + m[2][-1]  # FM-1_95 + the minor version's last digit
    if not re.fullmatch(r"FM-1_\d{3}", c["product"]):
      raise BuildError('firmware.json: "identity" must be FM-1_ and two digits (the third is your minor version)')
  c["file"] = re.sub(r"[^a-z0-9.-]+", "-", c["name"].lower()).strip("-") or "firmware"
  opts = c.get("options", {})
  bad = sorted(set(opts) - set(OPTIONS))
  if bad:
    raise BuildError(f"firmware.json: unknown option {', '.join(bad)} (there are: {', '.join(OPTIONS)})")
  mv = c["platform"] == "mvave"
  c["user_bytes"] = mvpack.USER_SIZE  # M-VAVE's layout (the SDK's flash API takes no other)
  if opts.get("bluetooth") and not mv:
    raise BuildError('firmware.json: the "bluetooth" option needs "platform": "mvave"')
  on = {k: bool(opts.get(k, mv if k == "bluetooth" else True)) for k in OPTIONS}
  c["defines"] = [f"-D{OPTIONS[k]}={int(on[k])}" for k in OPTIONS] + [f"-DFOMA_MVAVE={int(mv)}"]
  return c


# ---- the emulator: the same code against platform/web.c, as WebAssembly


def build_wasm(cfg):
  SITE.mkdir(parents=True, exist_ok=True)
  run(
    [
      "clang",
      "--target=wasm32",
      "-O2",
      "-nostdlib",
      "-fno-builtin",
      "-Wall",
      "-Wno-unused-function",
      "-Wno-incompatible-library-redeclaration",
      *INCLUDES,
      f'-DAPP_NAME="{cfg["name"]}"',
      f'-DAPP_VER="{cfg["version"]}"',
      *cfg["defines"],
      "-Wl,--no-entry",
      "-Wl,-z,stack-size=1048576",
      "-o",
      rel(SITE / "firmware.wasm"),
      "platform/web.c",
      *[rel(s) for s in sources()],
    ]
  )
  return (SITE / "firmware.wasm").stat().st_size


# ---- the update loader


def crc16(d, c=0):
  for b in d:
    c ^= b << 8
    for _ in range(8):
      c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
    c &= 0xFFFF
  return c


def ldr_head(dcrc, a, b, attr, name):
  body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
  return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
  """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
  blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
  blocks = bytearray()
  for i in range(0, len(image), 4096):
    raw = image[i : i + 4096]
    c = lz4blk.compress(raw)
    if len(c) > 4096 or lz4blk.decompress(c) != raw:
      raise BuildError(f"loader: block {i // 4096} does not compress below 4096 B")
    blocks += struct.pack("<II", len(c), len(raw)) + c
  inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
  body = inner + bytes(blocks)
  return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
  LDR.mkdir(parents=True, exist_ok=True)
  src = PLAT / "loader"
  tc_all(
    ("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
    ("cc", *CFLAGS, *INCLUDES, "-c", src / "loader.c", "-o", LDR / "loader.o"),
  )
  elf = LDR / "loader.elf"
  tc("ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld", LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
  _, dis, hdr = tc_all(("objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"), ("objdump", "-d", elf), ("objdump", "-h", elf))
  (LDR / "loader.dis").write_text(dis)
  for ln in hdr.splitlines():  # the loader rewrites the flash: nothing may run from XIP
    p = ln.split()
    if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
      raise BuildError(f"loader: section {p[1]} at {p[3]} is outside RAM")
  image = (LDR / "loader.bin").read_bytes()
  if not image or len(image) > 0x14000:
    raise BuildError("loader: image empty or too big")
  return ldr_wrap(image)


# ---- the app


def build_app(cfg):
  FW.mkdir(parents=True, exist_ok=True)
  flags = [*CFLAGS, *INCLUDES, f'-DAPP_NAME="{cfg["name"]}"', f'-DAPP_VER="{cfg["version"]}"', f'-DFELUCCA_ID="{cfg["product"]}"', *cfg["defines"]]
  objs, cmds = [], []
  for s in [PLAT / "crt0.S", PLAT / "hal/fm1_vec.S", PLAT / "hal/fm1_isr.S"]:
    objs.append(FW / (s.stem + ".o"))
    cmds.append(("cc", "-c", s, "-o", objs[-1]))
  for s in [PLAT / "fm1.c", *sources()]:
    objs.append(FW / (("platform_" if s.parent == PLAT else "app_") + s.stem + ".o"))
    cmds.append(("cc", *flags, "-c", s, "-o", objs[-1]))
  tc_all(*cmds)
  elf = FW / "firmware.elf"
  try:
    tc("ld", "--gc-sections", "-e", "_start", "-T", PLAT / "app.ld", *objs, "-o", elf)
  except BuildError as e:
    raise BuildError(explain_link(str(e))) from None
  for sect in ("text.bin", "data.bin", "ramtext.bin"):
    (FW / sect).unlink(missing_ok=True)
  *_, syms, dis, rt = tc_all(
    ("objcopy", "-O", "binary", "-j", ".text", elf, FW / "text.bin"),
    ("objcopy", "-O", "binary", "-j", ".data", elf, FW / "data.bin"),
    ("objcopy", "-O", "binary", "-j", ".ram_text", elf, FW / "ramtext.bin"),
    ("objdump", "-t", elf),
    ("objdump", "-d", elf),
    ("objdump", "-d", "-j", ".ram_text", elf),
  )
  (FW / "firmware.dis").write_text(dis)

  def symv(name):
    return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)

  img = bytearray((FW / "text.bin").read_bytes())
  # .ram_text and .data follow .text at their load addresses; crt0 copies them by words
  for sect, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
    load = symv(lname)
    if load % 4:
      raise BuildError(f"{lname} {load:#x} is not word aligned")
    blob = (FW / sect).read_bytes() if (FW / sect).exists() else b""
    if blob:
      if load - APP_XIP < len(img):
        raise BuildError(f"{lname} overlaps the image")
      img += b"\xff" * (load - APP_XIP - len(img))
      img += blob
  img += b"\xff" * (-len(img) % 4)
  (FW / "firmware.bin").write_bytes(img)
  return bytes(img), syms, dis, rt


LINK_HINTS = [
  (
    r"__(?:float|fix|extend|trunc)\w*|__(?:add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|un)[sd]f[23]\b",
    "float or double: the FM-1 has no FPU and no float library. Use integers (fixed point, e.g. Q15)",
  ),
  (r"__u?(?:div|mod)di3\b", "a 64-bit division or modulo: there is no libgcc on the FM-1. Shift, or divide 32-bit values"),
  (
    r"\b(?:printf|sprintf|snprintf|malloc|free|strlen|strcpy|sin|cos|pow|sqrt)\b",
    "there is no C library on the FM-1: see fm1.h for what there is (format_int, sine, ...)",
  ),
]


def explain_link(err):
  """the linker's 'undefined reference' errors, in words, with where they come from"""
  out = []
  for ln in err.splitlines():
    m = re.search(r"^(\S+?):\(\.text\.(\w+)[^)]*\): undefined reference to `(\w+)'", ln)
    if not m:
      continue
    why = next((h for rx, h in LINK_HINTS if re.search(rx, m[3])), f"{m[3]} does not exist")
    line = f"  in {m[2]}(): {why} ({m[3]})"
    if line not in out:
      out.append(line)
  return "link failed:\n" + "\n".join(out) if out else err


def check(img, syms, dis, rt):
  errors, notes = [], []
  m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
  if not m or int(m.group(1), 16) != APP_XIP:
    errors.append(f"_start is not at {APP_XIP:#x}")
  if img[:4] != bytes.fromhex("04818000"):
    errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
  rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
  if rt_calls:  # RAM code runs with the flash off: no calls into XIP
    errors.append(f".ram_text contains calls: {rt_calls[:3]}")
  fn = "?"
  for ln in dis.splitlines():  # nothing may call or load an address in the chip ROM
    h = re.match(r"^[0-9a-f]+ <([^>]+)>:", ln)
    if h:
      fn = h.group(1)
      continue
    mm = LINE.match(ln)
    if not mm:
      continue
    for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
      val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
      if 0xFFC00000 <= val < 0xFFD00000:
        errors.append(f"{fn}: a constant {val:#010x} lies in the chip ROM (a call there bricks the FM-1): change the arithmetic so it never appears")
  if len(img) > APP_SLOT:
    errors.append(f"image {len(img)} B exceeds the app slot ({APP_SLOT} B)")

  def sym(name):
    mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
    return int(mm.group(1), 16) if mm else 0

  ram = sym("_bss_end") - 0x01C08000
  pool = sym("_pool_end") - 0x01C20000
  notes.append(f"code {len(img) // 1024} of {APP_SLOT // 1024} KiB, RAM {ram // 1024} of 96 KiB, pool {pool // 1024} of 336 KiB")
  if ram > 96 * 1024:
    errors.append(f"RAM (.data + .bss) {ram} B > 96 KiB: make big arrays POOL or const")
  if pool > 0x54000:
    errors.append(f"pool {pool} B > 336 KiB")
  return errors, notes


# Every hardware register access lives in platform/hal/. Elsewhere (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000-, XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH();
#  - no HAL register macro (a hal/ #define that is, or expands to, a volatile access).
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check(files=None):
  def strip(s):
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)

  defs = {}
  for h in sorted((PLAT / "hal").glob("*.h")):
    for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
      defs[m.group(1)] = m.group(2)
  regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
  while True:
    more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
    if not more:
      break
    regs |= more
  errors = []
  if files is None:
    files = [*SRC.glob("**/*.[ch]"), *(PLAT / "src").glob("*.[ch]"), *(PLAT / "loader").glob("*.c"), PLAT / "fm1.c", PLAT / "common.c"]
  for f in sorted(files):
    for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
      where = f"{f.relative_to(ROOT)}:{no}"
      for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"), (MMIO_ASM, "inline asm")):
        m = rx.search(ln)
        if m:
          errors.append(f"{where}: {what} outside platform/hal ({m.group(0).strip()})")
      used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
      if used:
        errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside platform/hal")
  return errors


# ---- the app on M-VAVE's FM1 SDK (platform/mvave): its compiler flags, linker script and package (fm1_open's
# sdk/Build.ps1 and Package.cs). The SDK side (sdk.c, board/) sees the SDK's headers, the foma side (port.c, src/)
# foma's fm1.h: the two share bridge.h

MV = PLAT / "mvave"
MV_FLAGS = [
  "-integrated-as", "-mcpu=r3", "-mfprev1", "-Wuninitialized", "-Wno-invalid-noreturn", "-fno-common", "-fno-lto",
  "-fallow-pointer-null", "-fprefer-gnu-section", "-femulated-tls", "-Wno-shift-negative-value", "-Wframe-larger-than=2560",
  "-fms-extensions", "-fno-unwind-tables", "-ffunction-sections", "-fdata-sections", "-mllvm", "-pi32v2-large-program=true",
  "-D__GCC_PI32V2__", "-O2", "-fwrapv", "-fno-fast-math", "-ffp-contract=off", "-Wall", "-Wno-unused-function",
]  # fmt: skip
MV_SDK_INC = [f"-I{rel(MV / d)}" for d in ("sdk/include", "sdk/include/libc", "sdk/config", "board", ".")]
MV_FOMA_INC = ["-Iplatform", "-Iplatform/src", "-Iplatform/gen", f"-I{rel(MV)}"]
MV_KEEP = ["connectivity_wl82_bluetooth_start", "audio_app_bluetooth_open", "bt_get_local_name", "bt_get_mac_addr"]


def build_mvave(cfg):
  out = OUT / "mvave"
  out.mkdir(parents=True, exist_ok=True)
  common = [*MV_FLAGS, f'-DAPP_NAME="{cfg["name"]}"', f'-DAPP_VER="{cfg["version"]}"', *cfg["defines"]]
  bid = cfg["product"].encode().ljust(32, b"\0")[:32]
  (out / "build_identity.c").write_text(
    '#include "layout.h"\n'
    f"const struct fm1_firmware_layout fm1_layout = {{{cfg['user_bytes']}U,7U}};\n"
    f"const unsigned char fm1_build_id[32] = {{{','.join(str(b) for b in bid)}}};\n"
    f'const unsigned char fm1_update_identity[27] = "{cfg["product"]}";\n'
  )
  units = [(s, MV_SDK_INC) for s in (MV / "sdk.c", MV / "board/audio.c", MV / "board/device.c", MV / "board/panel.c", out / "build_identity.c")]
  units += [(s, MV_FOMA_INC) for s in (MV / "port.c", *sources())]
  objs, cmds = [], []
  for i, (s, inc) in enumerate(units):
    objs.append(out / f"{i}_{s.stem}.o")
    cmds.append(("cc", *common, *inc, "-c", s, "-o", objs[-1]))
  tc_all(*cmds)
  ld = (MV / "sdk/config/linker.ld.in").read_text()
  ld = ld.replace("@SBC_DECODER@", "sbc_decoder\n").replace("@ACL_TX_EXTRA@", " + 1024").replace("@ACL_RX_EXTRA@", " + 4096")
  (out / "linker.ld").write_text(ld)
  elf = out / "firmware.elf"
  lib = MV / "sdk/lib"
  try:
    tc(
      "ld", "-o", elf, *objs, "--sort-common", "--start-group", "--whole-archive", lib / "libfm1_engine.a", "--no-whole-archive",
      lib / "libc.a", lib / "libcompiler_rt.a", "--end-group", "--gc-sections", "-T", out / "linker.ld", f"-Map={rel(out / 'firmware.map')}",
      *[f"--undefined={k}" for k in MV_KEEP],
    )  # fmt: skip
  except BuildError as e:
    raise BuildError(explain_link(str(e))) from None
  parts = ("text", "data", "ram0_data", "cache_ram_data")
  for p in parts:
    (out / f"{p}.bin").unlink(missing_ok=True)
  tc_all(*[("objcopy", "-O", "binary", "-j", f".{p}", elf, out / f"{p}.bin") for p in parts])
  app = b"".join((out / f"{p}.bin").read_bytes() for p in parts if (out / f"{p}.bin").exists())

  def size(sect):
    m = re.search(r"^\s*\d+\s+\." + sect + r"\s+([0-9a-f]+)", tc("objdump", "-h", elf), re.MULTILINE)
    return int(m.group(1), 16) if m else 0

  try:
    pkg, rep = mvpack.create(app, str(MV / "sdk/pack"), cfg["product"], cfg["user_bytes"])
  except ValueError as e:
    raise BuildError(str(e)) from None
  ram = size("ram0_data") + size("ram0_bss")
  notes = [f"M-VAVE SDK: image {len(app) // 1024} of {rep['room'] // 1024} KiB", f"RAM {ram // 1024} of 511 KiB (the rest is the SDK's heap)"]
  return pkg, notes


def build_fm1(cfg):
  if cfg["platform"] == "mvave":
    errors = mmio_check([*SRC.glob("**/*.[ch]")])
    if errors:
      raise BuildError("the FM-1 build is refused (it could crash or brick the FM-1):\n" + "\n".join("  " + e for e in errors))
    pkg, notes = build_mvave(cfg)
    (SITE / "firmware.fwsc").write_bytes(pkg)
    (OUT / f"{cfg['file']}-{cfg['version']}.fwsc").write_bytes(pkg)
    return notes
  ota = build_loader()
  img, syms, dis, rt = build_app(cfg)
  errors, notes = check(img, syms, dis, rt)
  errors += mmio_check()
  if errors:
    raise BuildError("the FM-1 build is refused (it could crash or brick the FM-1):\n" + "\n".join("  " + e for e in errors))
  pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, cfg["product"])
  (SITE / "firmware.fwsc").write_bytes(pkg)
  (OUT / f"{cfg['file']}-{cfg['version']}.fwsc").write_bytes(pkg)
  return notes


# ---- the site


def build_site():
  SITE.mkdir(parents=True, exist_ok=True)
  for f in WEB.iterdir():
    if f.is_file():
      shutil.copy(f, SITE / f.name)


def build():
  t0 = time.time()
  info = {"id": str(time.time_ns()), "wasm": False, "fwsc": False, "log": ""}
  log = []
  try:
    cfg = config()
    info.update(name=cfg["name"], version=cfg["version"], identity=cfg["product"])
    build_site()
    try:
      n = build_wasm(cfg)
      info["wasm"] = True
      log.append(f"emulator  build/web/firmware.wasm  {n // 1024} KiB")
    except BuildError as e:
      log.append(f"emulator: compile failed\n{e}")
    if info["wasm"]:  # (a compile error is reported once)
      try:
        notes = build_fm1(cfg)
        info["fwsc"] = True
        log.append(f"FM-1      build/{cfg['file']}-{cfg['version']}.fwsc  identity {cfg['product']}  ({'; '.join(notes)})")
      except BuildError as e:
        log.append(f"FM-1: {e}")
  except BuildError as e:
    log.append(str(e))
  info["ok"] = info["wasm"] and info["fwsc"]
  info["log"] = "\n".join(log)
  SITE.mkdir(parents=True, exist_ok=True)
  (SITE / "build.json").write_text(json.dumps(info))
  print(info["log"])
  print(f"{'ok' if info['ok'] else 'FAILED'} in {time.time() - t0:.1f} s")
  return info["ok"]


# ---- dev: serve the site, rebuild on change


class Quiet(SimpleHTTPRequestHandler):
  def end_headers(self):
    self.send_header("Cache-Control", "no-store")
    super().end_headers()

  def log_message(self, *a):
    pass


def snapshot():
  files = [ROOT / "firmware.json", *SRC.rglob("*"), *PLAT.rglob("*"), *WEB.rglob("*")]
  return {str(f): f.stat().st_mtime for f in files if f.is_file()}


def dev(port):
  build()
  httpd = ThreadingHTTPServer(("0.0.0.0", port), partial(Quiet, directory=str(SITE)))
  threading.Thread(target=httpd.serve_forever, daemon=True).start()
  print(f"\nemulator, installer and rescue: http://localhost:{port}  (rebuilds when you save; ctrl-c stops)\n")
  seen = snapshot()
  try:
    while True:
      time.sleep(0.4)
      now = snapshot()
      if now != seen:
        seen = now
        print("\nchanged, rebuilding ...")
        build()
  except KeyboardInterrupt:
    pass


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--dev", type=int, metavar="PORT", help="serve build/web on PORT and rebuild on change")
  a = ap.parse_args()
  if not (TC / "pi32v2/bin/clang").exists():
    raise SystemExit("no JieLi toolchain here: run this through ./fm1 (it uses the Docker image)")
  if a.dev:
    dev(a.dev)
    return 0
  return 0 if build() else 1


if __name__ == "__main__":
  sys.exit(main())
