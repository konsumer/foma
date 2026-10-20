# SPDX-License-Identifier: MIT
# Copyright (c) 2023 Andrey Grigoryev (kagaimiq), 2026 M-VAVE (sdk/Package.cs in fm1_open); Python port for foma.
# The FM-1 update package (.fwsc) for M-VAVE's SDK layout: a Python port of fm1_open's sdk/Package.cs
# (MIT, kagaimiq's jl-misctools formats + M-VAVE's FM1 layout and writer).
import re
import struct
from pathlib import Path

AREA, ENTRY, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MIN_VM = 0xFF000, 536576, 4096, 32768
PUBLIC_CODE = 0x980F


def _crc_table():
  t = []
  for i in range(256):
    v = i << 8
    for _ in range(8):
      v = ((v << 1) ^ (0x1021 if v & 0x8000 else 0)) & 0xFFFF
    t.append(v)
  return t


CRC_T = _crc_table()


def crc(b, begin=0, length=None):
  v = 0
  for x in b[begin : begin + (len(b) - begin if length is None else length)]:
    v = ((v << 8) & 0xFFFF) ^ CRC_T[(v >> 8) ^ x]
  return v


def cipher(b, begin, length, state):
  for i in range(begin, begin + length):
    b[i] ^= state & 0xFF
    state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xFFFF


def round_up(n, a):
  return (n + a - 1) & -a


def num(b, off, v, w):
  for i in range(w):
    b[off + i] = (v >> (8 * i)) & 0xFF


def checksum(b, field, off, size):
  num(b, field, crc(b, off, size), 2)


def record(b, off, name, c, address, size, attrs, last, extra=None):
  b[off : off + 32] = bytes(32)
  num(b, off + 2, c, 2)
  num(b, off + 4, address & 0xFFFFFFFF, 4)
  num(b, off + 8, size & 0xFFFFFFFF, 4)
  num(b, off + 12, attrs, 2)
  num(b, off + 14, last, 2)
  b[off + 16 : off + 32] = b"\xff" * 16
  n = name.encode()
  b[off + 16 : off + 16 + len(n)] = n
  b[off + 16 + len(n)] = 0
  if extra is not None:
    num(b, off + 28, extra, 4)
  checksum(b, off, off + 2, 30)


def encode_boot(plain):
  res = bytearray(plain)
  banks = struct.unpack_from("<H", plain, 0)[0]
  if banks < 1 or banks > len(plain) // 16:
    raise ValueError("invalid boot directory")
  prev = banks * 16
  for i in range(banks):
    h = i * 16
    length = struct.unpack_from("<H", plain, h + 2)[0]
    start = struct.unpack_from("<i", plain, h + 8)[0]
    if (
      length == 0
      or start < prev
      or start > len(plain) - length
      or crc(plain, h, 14) != struct.unpack_from("<H", plain, h + 14)[0]
      or crc(plain, start, length) != struct.unpack_from("<H", plain, h + 12)[0]
    ):
      raise ValueError("invalid boot bank")
    cipher(res, h, 16, 0xFFFF)
    cipher(res, start, length, 0xFFFF)
    prev = start + length
  return bytes(res)


def _resource(d, name, size):
  b = (Path(d) / name).read_bytes()
  if len(b) != size:
    raise ValueError("unexpected FM1 resource: " + name)
  return b


def build_flash(app, d, end, vm_end, user=USER_SIZE):
  boot = _resource(d, "uboot.boot", 14488)
  config = _resource(d, "boot-config.bin", 721)
  tool = _resource(d, "cfg_tool.bin", 383)
  img = bytearray(b"\xff" * end)
  settings = bytearray(34 + len(config))
  for bit in range(16):  # jl-misctools chipkeybin descriptor for the public image
    settings[bit] = 16
    settings[16 + bit] = 16 ^ (0 if (PUBLIC_CODE >> bit) & 1 else 255)
  checksum(settings, 32, 0, 32)
  settings[34:] = config
  img[0:32] = bytes(
    [
      0,
      0,
      0x20,
      2,
      0x2F,
      0x10,
      0x4C,
      0xC9,
      0,
      0xF0,
      0x0F,
      0,
      0x10,
      0x10,
      0xA4,
      0xFF,
      0x4E,
      0x5D,
      0x0B,
      0x41,
      0xE0,
      0xCD,
      0x78,
      0x1D,
      0xC8,
      0x56,
      0x41,
      0x7F,
      0xB3,
      0x67,
      0xCF,
      0x9F,
    ]
  )
  checksum(img, 0, 2, 30)
  config_at = 160 + len(boot)
  record(img, 32, "uboot.boot", crc(boot), 160, len(boot), 0, 0)
  record(img, 64, "isd_config.ini", crc(settings), config_at, len(settings), 0x8002, 0)
  record(img, 96, "app_dir_head", 0xFFFF, AREA, 0xFFFFFFFF, 0xFF81, 0)
  record(img, 128, "key_mac", 0xFFFF, FLASH_LIMIT, 4096, 0x0112, 0xFFFF)
  for p in range(0, 160, 32):
    cipher(img, p, 32, 0xFFFF)
  img[160 : 160 + len(boot)] = encode_boot(boot)
  img[config_at : config_at + len(settings)] = settings
  payload = APP_DATA + len(app) + len(tool)
  a = AREA + APP_DATA
  img[a : a + len(app)] = app
  img[a + len(app) : a + len(app) + len(tool)] = tool
  record(img, AREA + 32, "app.bin", crc(app), APP_DATA, len(app), 0xFF82, 0)
  record(img, AREA + 64, "cfg_tool.bin", crc(tool), APP_DATA + len(app), len(tool), 0xFF82, 0)
  record(img, AREA + 96, "VM", 0xFFFF, end, vm_end - end, 0x8112, 0, MIN_VM)
  record(img, AREA + 128, "PRCT", 0xFFFF, 0, end, 0x8292, 0, 0x45444F43)
  record(img, AREA + 160, "BTIF", 0xFFFF, vm_end, BT_SIZE, 0x8192, 1, 0x4F545541)
  record(img, AREA, "app_area_head", crc(img, AREA + 32, payload - 32), ENTRY, payload, 0xFF83, 0)
  reserved = AREA + payload
  record(img, reserved + 32, "USER", 0xFFFF, FLASH_LIMIT - user, user, 0x8192, 1, 0x4F545541)
  record(img, reserved, "EXT_RESERVED", crc(img, reserved + 32, 32), 32, 64, 0xFF93, 1)
  for p in range(0, payload + 64, 32):
    cipher(img, AREA + p, min(32, payload + 64 - p), PUBLIC_CODE ^ (p >> 2))
  return img


def wrap(flash, loader, product):
  if not 1 <= len(loader) < 32768:
    raise ValueError("invalid FM1 update loader")
  loader_at = 1024 + round_up(len(flash), 32)
  total = loader_at + round_up(len(loader), 32)
  u = bytearray(b"\xff" * total)
  u[0:224] = bytes(224)
  for i, (data, name) in enumerate(((flash, "flash.bin"), (loader, "ota.bin"))):
    r = 64 + i * 80
    num(u, r, 0 if i == 0 else 100, 2)
    num(u, r + 2, i, 2)
    num(u, r + 4, crc(data), 2)
    num(u, r + 8, 1024 if i == 0 else loader_at, 4)
    num(u, r + 12, len(data), 4)
    num(u, r + 16, round_up(len(data), 32), 4)
    u[r + 64 : r + 64 + len(name)] = name.encode()
    cipher(u, r, 80, 0xFFFF)
  num(u, 4, total, 4)
  num(u, 8, 2, 2)
  num(u, 10, 4, 2)
  num(u, 12, 512, 4)
  u[16:22] = b"AC791N"
  checksum(u, 2, 64, 160)
  checksum(u, 0, 2, 62)
  cipher(u, 0, 64, 0xFFFF)
  u[1024 : 1024 + len(flash)] = flash
  u[loader_at : loader_at + len(loader)] = loader
  out = bytearray()
  for s in range(total):  # the product identity, one byte after every 47 of the first 940
    out.append(u[s])
    if s < 940 and (s + 1) % 47 == 0:
      i = s // 47
      out.append((ord(product[i]) + i + 1) & 0xFF if i < len(product) else 125)
  return bytes(out)


def create(app, resources, product, user=USER_SIZE):
  """app: .text + .data + .ram0_data + .cache_ram_data; user: the user partition's bytes (M-VAVE's: USER_SIZE).
  Returns (package bytes, layout report)."""
  if user < 4096 or user % 4096:
    raise ValueError("the user partition must be whole 4 KiB sectors")
  if not re.fullmatch(r"FM-1_8[0-9]{6}", product):
    raise ValueError("invalid FM1 update identity (FM-1_8 and six digits)")
  room = FLASH_LIMIT - user - BT_SIZE - MIN_VM - AREA - APP_DATA - 383 - 64
  if not 0 < len(app) <= room:
    raise ValueError(f"the firmware is {len(app)} bytes: {room} fit the FM1 layout")
  size = round_up(AREA + APP_DATA + len(app) + 383 + 64, 4096)
  user_at = FLASH_LIMIT - user
  vm_end = user_at - BT_SIZE
  if size > vm_end - MIN_VM:
    raise ValueError("insufficient FM1 settings space")
  pkg = wrap(build_flash(app, resources, size, vm_end, user), (Path(resources) / "ota.bin").read_bytes(), product)
  return pkg, {"image": size, "app": len(app), "room": room, "vm": vm_end - size, "user": user}
