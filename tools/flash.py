#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install a package on the FM-1 over USB, from the command line (./fm1 flash): what the page's Install button does.

  tools/flash.py                    build/web/firmware.fwsc
  tools/flash.py bt                 build/web/firmware-bt.fwsc (any package the build made: bt, smk37, ...)
  tools/flash.py FILE.fwsc          that package
  tools/flash.py --id               only say what the FM-1 runs (FM-1_000: the USB rescue; ota-...: an install isn't finished)

It runs on the computer, not in Docker (Docker on a Mac can't reach USB MIDI), with nothing to install: CoreMIDI on
macOS, ALSA's raw MIDI on Linux. Quit anything else that uses the FM-1's MIDI port first.
The protocol is web/fm1ota.js's (M-UPGRADE compatible): the running firmware reads parts of the package and starts the
update loader, the loader reads the whole image and writes it, then the new firmware starts.
"""

import os
import re
import select
import sys
import threading
import time

HS_QUERY = bytes([0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7])
UPGRADE = bytes([0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7])
MAXDATA = 512
PORTS = re.compile(r"fm-1|smk|mke|starry|felucca|ota|composite|sinco|usb-midi", re.I)  # never probe other gear


def is_ota(model):
  return model.lower().startswith("ota-")


def same_model(a, b):  # a package installs only on its own model (fm1ota.js sameModel)
  return re.sub(r"^ota-", "", a, flags=re.I) == re.sub(r"^ota-", "", b, flags=re.I)


# ---- the package (web/fm1pkg.js)
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def product_of(fwsc):
  if len(fwsc) < BLOCKS * BLK:
    raise SystemExit("flash: not an FM-1 package (too short)")
  return "".join(chr((fwsc[i * BLK + KEEP] - i - 1) & 0xFF) for i in range(BLOCKS) if fwsc[i * BLK + KEEP] != 0x7D)


def enc(buf, key=0xFFFF):
  for i in range(len(buf)):
    buf[i] ^= key & 0xFF
    key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
  return buf


def crc16(d, c=0):
  for b in d:
    c ^= b << 8
    for _ in range(8):
      c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
  return c


def deinterleave(fwsc, blocks):
  return b"".join(fwsc[i * BLK : i * BLK + KEEP] for i in range(blocks)) + fwsc[blocks * BLK :]


def files_ok(img):  # flash.bin (type 0) and ota.bin (type 100) match their CRCs
  if len(img) < 0x40:
    return False
  h = enc(bytearray(img[:0x40]))
  count = int.from_bytes(h[8:10], "little")
  if count < 1 or count > 8 or len(img) < 0x40 + 0x50 * count:
    return False
  for i in range(count):
    e = enc(bytearray(img[0x40 + i * 0x50 : 0x90 + i * 0x50]))
    typ, off, size = int.from_bytes(e[0:2], "little"), int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little")
    if off + size > len(img):
      return False
    if typ in (0, 100) and crc16(img[off : off + size]) != int.from_bytes(e[4:6], "little"):
      return False
  return True


def logical_image(fwsc):  # the .fwsc without its marker bytes: what the device addresses
  n = BLOCKS
  while n < 64 and (n + 1) * BLK <= len(fwsc) and fwsc[n * BLK + KEEP] == 0x7D:
    n += 1
  if n != BLOCKS:
    img = deinterleave(fwsc, n)
    if files_ok(img) or not files_ok(deinterleave(fwsc, BLOCKS)):
      return img
  return deinterleave(fwsc, BLOCKS)


# ---- SysEx framing (web/fm1ota.js)
def pack7(data):
  out, acc, nb = bytearray(), 0, 0
  for b in data:
    acc |= b << nb
    nb += 8
    while nb >= 7:
      out.append(acc & 0x7F)
      acc >>= 7
      nb -= 7
  if nb:
    out.append(acc & 0x7F)
  return bytes(out)


def unpack7(s):
  out, acc, nb = bytearray(), 0, 0
  for b in s:
    acc |= (b & 0x7F) << nb
    nb += 7
    while nb >= 8:
      out.append(acc & 0xFF)
      acc >>= 8
      nb -= 8
  return bytes(out)


def response(addr, data, fl=0):
  n = len(data)
  body = bytearray([0x00, 0x59, 0x30, (n + 8) & 0xFF, ((n + 8) >> 8) & 0xFF, 0, fl]) + addr.to_bytes(4, "little") + bytes([n & 0xFF, (n >> 8) & 0xFF, 0]) + data
  body.append(~sum(body[6:]) & 0xFF)
  return b"\xf0" + pack7(body) + b"\xf7"


def parse_request(p):
  u = unpack7(p[1:-1])
  if len(u) != 15 or u[0] != 0 or u[1] != 0x59 or u[2] != 0x30 or (~sum(u[6:14]) & 0xFF) != u[14]:
    return None
  return u[6], int.from_bytes(u[7:11], "little"), u[11] | u[12] << 8 | u[13] << 16


def parse_identity(p):
  d = unpack7(p[1:-1])
  if len(d) != 34 or d[0] != 0 or d[1] != 0x59 or d[2] != 0x11:
    return None
  m = re.match(r"^([^_]+)_(\d+)$", d[6:33].rstrip(b"\0").decode("latin-1"))
  return (m.group(0), m.group(1)) if m else None


# ---- MIDI ports: name -> a Link (send SysEx, read the SysEx that came in)
class Link:
  def __init__(self):
    self.q, self.cv, self.buf = [], threading.Condition(), bytearray()

  def feed(self, data):  # bytes as they come: whole SysEx messages into the queue
    for b in data:
      if b == 0xF0:
        self.buf = bytearray([b])
      elif self.buf:
        if b >= 0xF8:
          continue  # (realtime bytes can come in the middle)
        self.buf.append(b)
        if b == 0xF7:
          with self.cv:
            self.q.append(bytes(self.buf))
            self.cv.notify()
          self.buf = bytearray()
        elif b & 0x80:
          self.buf = bytearray()

  def read(self, s):
    end = time.monotonic() + s
    with self.cv:
      while not self.q:
        left = end - time.monotonic()
        if left <= 0 or self.lost():
          return None
        self.cv.wait(min(left, 0.2))
      return self.q.pop(0)

  def drain(self):
    with self.cv:
      self.q.clear()


class CoreMidi:  # macOS, by ctypes
  def __init__(self):
    import ctypes as C
    import ctypes.util

    self.C = C
    cm = C.cdll.LoadLibrary(ctypes.util.find_library("CoreMIDI"))
    cf = C.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    self.cm, self.cf = cm, cf
    cf.CFStringCreateWithCString.restype = C.c_void_p
    cf.CFStringCreateWithCString.argtypes = [C.c_void_p, C.c_char_p, C.c_uint32]
    cf.CFStringGetCString.argtypes = [C.c_void_p, C.c_char_p, C.c_long, C.c_uint32]
    cf.CFRelease.argtypes = [C.c_void_p]
    cf.CFRunLoopRunInMode.argtypes = [C.c_void_p, C.c_double, C.c_bool]
    self.mode = C.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode")
    self.pname = C.c_void_p.in_dll(cm, "kMIDIPropertyName")
    for f in ("MIDIGetSource", "MIDIGetDestination"):
      getattr(cm, f).restype = C.c_uint32
      getattr(cm, f).argtypes = [C.c_ulong]
    cm.MIDIGetNumberOfSources.restype = cm.MIDIGetNumberOfDestinations.restype = C.c_ulong
    cm.MIDIObjectGetStringProperty.argtypes = [C.c_uint32, C.c_void_p, C.POINTER(C.c_void_p)]
    cm.MIDIClientCreate.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32)]
    self.READ = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_void_p)
    cm.MIDIInputPortCreate.argtypes = [C.c_uint32, C.c_void_p, self.READ, C.c_void_p, C.POINTER(C.c_uint32)]
    cm.MIDIOutputPortCreate.argtypes = [C.c_uint32, C.c_void_p, C.POINTER(C.c_uint32)]
    cm.MIDIPortConnectSource.argtypes = [C.c_uint32, C.c_uint32, C.c_void_p]
    cm.MIDIPortDisconnectSource.argtypes = [C.c_uint32, C.c_uint32]
    cm.MIDIPacketListInit.restype = C.c_void_p
    cm.MIDIPacketListInit.argtypes = [C.c_void_p]
    cm.MIDIPacketListAdd.restype = C.c_void_p
    cm.MIDIPacketListAdd.argtypes = [C.c_void_p, C.c_ulong, C.c_void_p, C.c_uint64, C.c_ulong, C.c_char_p]
    cm.MIDISend.argtypes = [C.c_uint32, C.c_uint32, C.c_void_p]
    self.client, self.inp, self.out = C.c_uint32(), C.c_uint32(), C.c_uint32()
    if cm.MIDIClientCreate(self.cfstr("fm1 flash"), None, None, C.byref(self.client)):
      raise SystemExit("flash: CoreMIDI didn't start")
    self.link = None
    self.arm = os.uname().machine == "arm64"  # (packets are 4-byte aligned on arm64)
    self.cb = self.READ(self.on_read)
    cm.MIDIInputPortCreate(self.client, self.cfstr("in"), self.cb, None, C.byref(self.inp))
    cm.MIDIOutputPortCreate(self.client, self.cfstr("out"), C.byref(self.out))

  def cfstr(self, s):
    return self.cf.CFStringCreateWithCString(None, s.encode(), 0x08000100)

  def name(self, ep):
    C, v = self.C, self.C.c_void_p()
    if self.cm.MIDIObjectGetStringProperty(ep, self.pname, C.byref(v)) or not v.value:
      return ""
    b = C.create_string_buffer(256)
    self.cf.CFStringGetCString(v, b, 256, 0x08000100)
    self.cf.CFRelease(v)
    return b.value.decode(errors="replace")

  def on_read(self, plist, _ref, _src):
    C, link = self.C, self.link
    if not link or not plist:
      return
    n = C.c_uint32.from_address(plist).value
    p = plist + 4
    for _ in range(n):
      ln = C.c_uint16.from_address(p + 8).value
      link.feed(C.string_at(p + 10, ln))
      p += 10 + ln
      if self.arm:
        p = (p + 3) & ~3

  def pump(self, s):  # (endpoints come and go only while the run loop runs)
    self.cf.CFRunLoopRunInMode(self.mode, s, False)

  def ports(self):  # [(name, (source, destination))]
    cm = self.cm
    dests = {}
    for i in range(cm.MIDIGetNumberOfDestinations()):
      d = cm.MIDIGetDestination(i)
      dests.setdefault(self.name(d), d)
    out = []
    for i in range(cm.MIDIGetNumberOfSources()):
      s = cm.MIDIGetSource(i)
      nm = self.name(s)
      if nm in dests:
        out.append((nm, (s, dests[nm])))
    return out

  def open(self, ep):
    self.src, self.dst = ep
    link = Link()
    link.lost = lambda: self.src not in [cm_s for _, (cm_s, _d) in self.ports()]
    self.link = link
    self.cm.MIDIPortConnectSource(self.inp, self.src, None)
    link.send = self.send
    return link

  def close(self):
    if self.link:
      self.cm.MIDIPortDisconnectSource(self.inp, self.src)
      self.link = None

  def send(self, data):
    C = self.C
    buf = C.create_string_buffer(len(data) + 64)
    pkt = self.cm.MIDIPacketListInit(buf)
    if not self.cm.MIDIPacketListAdd(buf, len(buf), pkt, 0, len(data), data):
      return False
    return self.cm.MIDISend(self.out, self.dst, buf) == 0


class Alsa:  # Linux: /dev/snd/midiC*D*, read by a thread
  def __init__(self):
    self.fd, self.link = None, None

  def pump(self, s):
    time.sleep(s)

  def ports(self):
    out = []
    for dev in sorted(os.listdir("/dev/snd")) if os.path.isdir("/dev/snd") else []:
      m = re.match(r"midiC(\d+)D(\d+)$", dev)
      if m:
        try:
          nm = open(f"/proc/asound/card{m.group(1)}/id").read().strip()
        except OSError:
          nm = dev
        try:
          nm = open(f"/sys/class/sound/card{m.group(1)}/device/../product").read().strip() or nm
        except OSError:
          pass
        out.append((nm, "/dev/snd/" + dev))
    return out

  def open(self, path):
    self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    link = Link()
    self.link, fd = link, self.fd
    link.gone = False
    link.lost = lambda: link.gone

    def reader():
      while self.fd == fd:
        try:
          r, _, _ = select.select([fd], [], [], 0.2)
          if r:
            link.feed(os.read(fd, 4096))
        except OSError:
          link.gone = True
          return

    threading.Thread(target=reader, daemon=True).start()
    link.send = self.send
    return link

  def close(self):
    if self.fd is not None:
      fd, self.fd, self.link = self.fd, None, None
      time.sleep(0.25)
      os.close(fd)

  def send(self, data):
    try:
      view = memoryview(data)
      while view:
        select.select([], [self.fd], [], 1)
        try:
          n = os.write(self.fd, view)
        except BlockingIOError:
          continue
        view = view[n:]
      return True
    except OSError:
      self.link.gone = True
      return False


# ---- the install (fm1ota.js Updater)
class Updater:
  def __init__(self, names):
    self.midi = CoreMidi() if sys.platform == "darwin" else Alsa()
    self.names = [n.lower() for n in names if n]

  def sleep(self, s):
    end = time.monotonic() + s
    while time.monotonic() < end:
      self.midi.pump(min(0.1, end - time.monotonic()))

  def handshake(self, link, tries):
    link.drain()
    for _ in range(tries):
      if not link.send(HS_QUERY):
        return None
      end = time.monotonic() + 1
      while time.monotonic() < end:
        p = link.read(end - time.monotonic())
        if not p:
          break
        ident = parse_identity(p)
        if ident:
          return ident
    return None

  def find(self, want=None):  # (link, (identity, model), port name) of the first FM-1 that answers
    self.midi.pump(0.05)
    for nm, ep in self.midi.ports():
      if not PORTS.search(nm) and not any(n in nm.lower() for n in self.names):
        continue
      try:
        link = self.midi.open(ep)
      except OSError:
        continue
      ident = self.handshake(link, 2)
      if ident and (not want or want(ident[1])):
        return link, ident, nm
      self.midi.close()
    return None

  def wait_for(self, want, s):
    end = time.monotonic() + s
    while time.monotonic() < end:
      dev = self.find(want)
      if dev:
        return dev
      self.sleep(1)
    return None

  def serve(self, link, image, finish, idle, progress):  # answer read requests until the finish address
    served, last = 0, time.monotonic()
    while True:
      p = link.read(0.2)
      if not p:
        self.midi.pump(0.05)
        if link.lost():
          return served, False, True
        if time.monotonic() - last > idle:
          return served, False, False
        continue
      r = parse_request(p)
      if not r:
        continue
      fl, addr, n = r
      last = time.monotonic()
      if addr in (0xE0000000, 0xF0000000):
        link.send(response(addr, b"success\0"))
        if addr == finish:
          return served, True, False
        continue
      if n > MAXDATA or addr + n > len(image):
        raise SystemExit(f"flash: bad request {addr:x}+{n}")
      time.sleep(0.01)
      if not link.send(response(addr, image[addr : addr + n], fl)):
        return served, False, True
      served += 1
      progress(served, addr)

  def write(self, link, image, total):
    link.send(UPGRADE)
    self.sleep(2)
    r = self.serve(link, image, 0xF0000000, 180, lambda n, a: bar("write", a, total))
    self.midi.close()
    print()
    return r


def bar(step, at, total):  # total 0: a count
  sys.stdout.write(f"\r{step:7} {min(100, at * 100 // total):3d}%" if total else f"\r{step:7} {at}")
  sys.stdout.flush()


def project_name():
  try:
    import json

    return json.load(open("firmware.json")).get("name", "")
  except (OSError, ValueError):
    return ""


def main():
  args = sys.argv[1:]
  up = Updater([project_name()])
  if args == ["--id"]:
    dev = up.find()
    if not dev:
      raise SystemExit("flash: no FM-1 found (USB cable, and no other app using it?)")
    up.midi.close()
    print(f"{dev[1][0]} (on {dev[2]})")
    return
  arg = args[0] if args else ""
  path = arg if arg.endswith(".fwsc") or os.sep in arg else f"build/web/firmware{'-' + arg if arg else ''}.fwsc"
  if not os.path.isfile(path):
    raise SystemExit(f"flash: no {path} (./fm1 build first)")
  fwsc = open(path, "rb").read()
  product, image = product_of(fwsc), logical_image(fwsc)
  print(f"package {path}: {product}")

  ota = up.find(is_ota)  # in update mode already: finish that install
  if ota:
    link, ident, nm = ota
    if not same_model(ident[1], product.split("_")[0]):
      raise SystemExit(f"flash: the device is in the update mode of {ident[0]}, the package is for {product}")
    print(f"{ident[0]} on {nm}: in update mode, finishing the install")
  else:
    dev = up.find(lambda m: not is_ota(m))
    if not dev:
      raise SystemExit("flash: no FM-1 found (USB cable, and no other app using it?)")
    link, ident, nm = dev
    if not same_model(ident[1], product.split("_")[0]):
      raise SystemExit(f"flash: the device is {ident[0]}, the package is for {product}")
    print(f"{ident[0]} on {nm}: starting")
    link.send(UPGRADE)
    up.sleep(2)
    served, done, lost = up.serve(link, image, 0xE0000000, 8, lambda n, a: bar("verify", n, 0))
    up.midi.close()
    print()
    if not done:
      raise SystemExit(f"flash: the device {'was disconnected' if lost else 'stopped'} after {served} requests: nothing was written")
    print("loader  waiting")
    up.sleep(3)
    ota = up.wait_for(is_ota, 30)
    if not ota:
      raise SystemExit("flash: the update loader did not appear. Replug the USB cable and run it again: the FM-1 stays in update mode until it's done")
    link = ota[0]
  served, done, lost = up.write(link, image, len(image))
  if not done:
    raise SystemExit(f"flash: the loader {'was disconnected' if lost else 'stopped'} after {served} requests. Replug and run it again to resume")
  print("reboot  waiting")
  up.sleep(3)
  back = up.wait_for(lambda m: not is_ota(m), 90)  # (two failed boots, then the USB rescue, take a while)
  if not back:
    raise SystemExit("flash: the device did not come back: power-cycle it")
  up.midi.close()
  if back[1][0] != product:
    raise SystemExit(
      f"flash: installed, but the device reports {back[1][0]}" + (" (the USB rescue: the firmware didn't start)" if back[1][0].endswith("_000") else "")
    )
  print(f"done    {back[1][0]}")


if __name__ == "__main__":
  try:
    main()
  except KeyboardInterrupt:
    raise SystemExit(130)
