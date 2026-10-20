// SPDX-License-Identifier: GPL-3.0-only
// The page: the emulator (firmware.wasm in worklet.js), the installer and the rescue (fm1ota.js).
import { productOf, logicalImage } from './fm1pkg.js'
import { Updater } from './fm1ota.js'

const $ = (id) => document.getElementById(id)
const DEV = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname)

// ---------------------------------------------------------------- tabs
function showTab() {
  const tab = (location.hash || '#emulator').slice(1)
  for (const s of document.querySelectorAll('.tab')) s.hidden = s.id !== 'tab-' + tab
  for (const a of document.querySelectorAll('nav a')) a.classList.toggle('on', a.dataset.tab === tab)
}
addEventListener('hashchange', showTab)
showTab()

// ---------------------------------------------------------------- the build (build.json)
let build = null
let storeKey = 'fm1-settings'
let busy = false
async function pollBuild() {
  let b
  try {
    const r = await fetch('build.json', { cache: 'no-store' })
    if (!r.ok) throw new Error(r.status)
    b = await r.json()
  } catch {
    return
  }
  if (build && b.id === build.id) return
  const first = !build
  build = b
  $('fwname').textContent = b.name || 'FM-1'
  $('fwver').textContent = b.version ? `${b.version} · ${b.identity}` : ''
  document.title = `${b.name || 'FM-1'} firmware`
  $('buildstate').hidden = !DEV
  $('buildstate').textContent = b.ok ? `built ${new Date(Number(BigInt(b.id) / 1000000n)).toLocaleTimeString()}` : 'build failed'
  $('buildstate').className = 'badge ' + (b.ok ? 'ok' : 'bad')
  $('buildlog').hidden = b.ok || !DEV
  $('buildlog-title').textContent = b.wasm ? 'The emulator runs, but the FM-1 build failed' : 'Build failed'
  $('buildlog-text').textContent = b.log
  $('pkginfo').textContent = b.fwsc ? `${b.name} ${b.version}, identity ${b.identity}` : 'The FM-1 package did not build (see the build log).'
  $('install').disabled = !b.fwsc || busy
  storeKey = `fm1-settings-${b.name}`
  if (b.wasm && !first) reload()
}
$('buildstate').addEventListener('click', () => {
  $('buildlog').hidden = !$('buildlog').hidden
})
pollBuild()
if (DEV) setInterval(() => document.visibilityState === 'visible' && pollBuild(), 1000)

// ---------------------------------------------------------------- the panel, in a 1000 x 640 space
const device = $('device')
const BTN = { 'OCT-': 0, 'OCT+': 1, FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 }
const ENC = { SELECT: 0, ALGORITHM: 1, KNOB1: 2, KNOB2: 3, KNOB3: 4, KNOB4: 5, PRESETS: 6 }
const WHITE_K = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26]
const BLACK_K = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25]
const BLACK_AFTER = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14]

function place(el, x, y, w, h) {
  Object.assign(el.style, { left: `${x / 10}%`, top: `${y / 6.4}%`, width: `${w / 10}%`, height: `${h / 6.4}%` })
  return el
}
function add(cls, x, y, w, h, text = '') {
  const el = document.createElement('div')
  el.className = cls
  el.textContent = text
  device.insertBefore(place(el, x, y, w, h), $('poweron'))
  return el
}

add('tray', 58, 258, 210, 58)
add('tray', 568, 160, 400, 140)
add('bezel', 300, 44, 236, 236)
add('tray', 36, 340, 928, 270)
place($('screen'), 306, 50, 224, 224)

let master = 700
const knobs = []
function knob(x, y, r, label, id) {
  add('label', x - 60, y - r - 34, 120, 22, label)
  const el = add('knob', x - r, y - r, 2 * r, 2 * r)
  el.tabIndex = 0
  el.setAttribute('role', 'slider')
  el.setAttribute('aria-label', label)
  const k = { el, id, angle: id === 'master' ? -135 + (270 * master) / 1023 : 0 }
  el.style.setProperty('--angle', `${k.angle}deg`)
  knobs.push(k)
  return k
}
knob(105, 100, 24, 'MASTER', 'master')
knob(215, 100, 24, 'SELECT', ENC.SELECT)
knob(105, 205, 24, 'PRESETS', ENC.PRESETS)
knob(215, 205, 24, 'ALGORITHM', ENC.ALGORITHM)
for (let i = 0; i < 4; i++) knob(610 + i * 104, 100, 22, `KNOB${i + 1}`, ENC[`KNOB${i + 1}`])

const buttons = []
function button(x, y, w, h, label) {
  const el = add('btn', x, y, w, h, label)
  el.tabIndex = 0
  el.setAttribute('role', 'button')
  buttons[BTN[label]] = el
}
button(72, 268, 82, 38, 'OCT-')
button(170, 268, 82, 38, 'OCT+')
const ROW1 = ['FX', 'SEL', 'ENV', 'LFO', 'EDIT', 'GLO']
const ROW2 = ['HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC']
for (let i = 0; i < 6; i++) {
  button(582 + i * 63, 174, 54, 48, ROW1[i])
  button(582 + i * 63, 236, 54, 48, ROW2[i])
}

const keys = []
{
  const x0 = 36
  const y0 = 340
  const pitch = (928 - 24) / 16
  const kw = pitch * 0.78
  for (let i = 0; i < 16; i++) keys[WHITE_K[i]] = add('key white', x0 + 12 + i * pitch + (pitch - kw) / 2, y0 + 270 * 0.43, kw, 270 * 0.52)
  BLACK_AFTER.forEach((a, k) => {
    keys[BLACK_K[k]] = add('key black', x0 + 12 + (a + 1) * pitch - kw / 2, y0 + 270 * 0.06, kw, 270 * 0.33)
  })
  keys.forEach((el, i) => el.setAttribute('aria-label', `key ${i + 1}`))
}

// ---------------------------------------------------------------- input
let node = null
const send = (m) => node && node.port.postMessage(m)
const heldKeys = new Map()
const heldBtns = new Map()
let latched = 0
let keyMask = 0
let btnMask = 0

function syncKeys() {
  let m = 0
  for (const k of heldKeys.values()) m |= 1 << k
  keys.forEach((el, i) => el.classList.toggle('held', !!(m & (1 << i))))
  if (m !== keyMask) send({ type: 'keys', mask: (keyMask = m) })
}
function syncButtons() {
  let m = latched
  for (const b of heldBtns.values()) m |= 1 << b
  buttons.forEach((el, i) => {
    el.classList.toggle('held', !!(m & (1 << i)))
    el.classList.toggle('latched', !!(latched & (1 << i)))
  })
  if (m !== btnMask) send({ type: 'buttons', mask: (btnMask = m) })
}

keys.forEach((el, i) => {
  el.addEventListener('pointerdown', (e) => {
    e.preventDefault()
    el.setPointerCapture(e.pointerId)
    heldKeys.set('p' + e.pointerId, i)
    syncKeys()
  })
})
buttons.forEach((el, i) => {
  el.addEventListener('contextmenu', (e) => e.preventDefault())
  el.addEventListener('pointerdown', (e) => {
    e.preventDefault()
    if (e.shiftKey || e.button === 2 || latched & (1 << i)) {
      latched ^= 1 << i
      return syncButtons()
    }
    el.setPointerCapture(e.pointerId)
    heldBtns.set('p' + e.pointerId, i)
    syncButtons()
  })
})
function pointerUp(e) {
  const id = 'p' + e.pointerId
  if (heldKeys.delete(id)) syncKeys()
  if (heldBtns.delete(id)) syncButtons()
}
for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) device.addEventListener(ev, pointerUp, true)

function turn(k, n) {
  if (!n) return
  if (k.id === 'master') {
    master = Math.max(0, Math.min(1023, master + n * 16))
    k.angle = -135 + (270 * master) / 1023
    send({ type: 'master', value: master })
  } else {
    k.angle = (k.angle + n * 18) % 360
    send({ type: 'encoder', id: k.id, n })
  }
  k.el.style.setProperty('--angle', `${k.angle}deg`)
}
for (const k of knobs) {
  let drag = null
  let wheel = 0
  k.el.addEventListener('pointerdown', (e) => {
    e.preventDefault()
    k.el.setPointerCapture(e.pointerId)
    drag = { y: e.clientY, acc: 0 }
  })
  k.el.addEventListener('pointermove', (e) => {
    if (!drag) return
    drag.acc += drag.y - e.clientY
    drag.y = e.clientY
    const n = Math.trunc(drag.acc / 6)
    if (n) {
      drag.acc -= n * 6
      turn(k, n)
    }
  })
  for (const ev of ['pointerup', 'pointercancel']) k.el.addEventListener(ev, () => (drag = null))
  k.el.addEventListener(
    'wheel',
    (e) => {
      e.preventDefault()
      const dy = e.deltaMode === 1 ? e.deltaY * 33 : e.deltaY
      if (Math.abs(dy) >= 50) return turn(k, Math.sign(-dy) * Math.max(1, Math.round(Math.abs(dy) / 100))) // mouse wheel notches (~100 px each, merged when fast): a detent each
      wheel -= dy // a trackpad: by distance
      const n = Math.trunc(wheel / 25)
      if (n) {
        wheel -= n * 25
        turn(k, n)
      }
    },
    { passive: false }
  )
  k.el.addEventListener('keydown', (e) => {
    const n = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key]
    if (n) {
      e.preventDefault()
      e.stopPropagation()
      turn(k, n)
    }
  })
}

// a tracker's layout: the bottom row from Z is the first octave (F3..), the row from Q the next one;
// the row above each is its black keys, where there is one
const TRACKER = {
  Z: 0,
  S: 1,
  X: 2,
  D: 3,
  C: 4,
  F: 5,
  V: 6,
  B: 7,
  H: 8,
  N: 9,
  J: 10,
  M: 11,
  Comma: 12,
  L: 13,
  Period: 14,
  Semicolon: 15,
  Slash: 16,
  Q: 12,
  2: 13,
  W: 14,
  3: 15,
  E: 16,
  4: 17,
  R: 18,
  T: 19,
  6: 20,
  Y: 21,
  7: 22,
  U: 23,
  I: 24,
  9: 25,
  O: 26
}
const KEYMAP = {}
for (const [k, n] of Object.entries(TRACKER)) KEYMAP[/^\d$/.test(k) ? 'Digit' + k : k.length === 1 ? 'Key' + k : k] = { key: n }
const KEY_CAP = { Comma: ',', Period: '.', Slash: '/', Semicolon: ';' }
for (const [k, n] of Object.entries(TRACKER)) keys[n].textContent += (keys[n].textContent ? ' ' : '') + (KEY_CAP[k] || k)
function onKey(e, down) {
  const m = KEYMAP[e.code]
  if (!m || e.metaKey || e.ctrlKey || e.altKey || $('tab-emulator').hidden) return
  if (e.target.closest && e.target.closest('input, select, textarea, [role=slider]')) return
  e.preventDefault()
  if (e.repeat) return
  if (down) heldKeys.set('k' + e.code, m.key)
  else heldKeys.delete('k' + e.code)
  syncKeys()
}
addEventListener('keydown', (e) => onKey(e, true))
addEventListener('keyup', (e) => onKey(e, false))
addEventListener('blur', () => {
  heldKeys.clear()
  heldBtns.clear()
  syncKeys()
  syncButtons()
})

// ---------------------------------------------------------------- the emulator
const ctx2d = $('screen').getContext('2d')
const img = ctx2d.createImageData(240, 240)
let audioCtx = null

function say(text, err) {
  const c = $('console')
  const line = document.createElement('div')
  line.textContent = text
  if (err) line.className = 'err'
  c.append(line)
  while (c.childNodes.length > 500) c.firstChild.remove()
  c.scrollTop = c.scrollHeight
}

function loadStore() {
  try {
    const s = localStorage.getItem(storeKey)
    return s ? Uint8Array.from(atob(s), (c) => c.charCodeAt(0)) : null
  } catch {
    return null
  }
}
function saveStore(bytes) {
  try {
    localStorage.setItem(storeKey, btoa(String.fromCharCode(...bytes)))
  } catch {}
}

function drawScreen(fb) {
  const d = img.data
  for (let i = 0; i < 240 * 240; i++) {
    const c = ((fb[i] & 0xff) << 8) | (fb[i] >> 8) // stored byte-swapped, as the panel takes it
    d[4 * i] = ((c >> 11) * 255) / 31
    d[4 * i + 1] = (((c >> 5) & 63) * 255) / 63
    d[4 * i + 2] = ((c & 31) * 255) / 31
    d[4 * i + 3] = 255
  }
  ctx2d.putImageData(img, 0, 0)
}

function onFrame(m) {
  if (m.fb) drawScreen(m.fb)
  keys.forEach((el, i) => el.classList.toggle('lit', !!(m.keys & (1 << i))))
  buttons.forEach((el, i) => el.classList.toggle('lit', !!(m.buttons & (1 << i))))
  for (const el of [...keys, ...buttons]) el.dataset.backlight = m.backlight || 0
  if (m.store) saveStore(m.store)
  if (m.log) for (const l of m.log.replace(/\n$/, '').split('\n')) say(l)
  if (m.midi && midiOut) for (const p of m.midi) midiOut.send(midiBytes(p))
  const s = Math.floor(m.ms / 1000)
  $('clock').textContent = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
}

async function reload() {
  if (!node) return
  const r = await fetch('firmware.wasm', { cache: 'no-store' })
  if (!r.ok) return say(`firmware.wasm: HTTP ${r.status}`, true)
  const wasm = await r.arrayBuffer()
  heldKeys.clear()
  heldBtns.clear()
  latched = keyMask = btnMask = 0
  syncKeys()
  syncButtons()
  node.port.postMessage({ type: 'load', wasm, store: loadStore(), master }, [wasm])
}

$('power').addEventListener('click', async () => {
  $('power').disabled = true
  try {
    audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' })
    await audioCtx.audioWorklet.addModule('worklet.js')
    node = new AudioWorkletNode(audioCtx, 'fm1', { numberOfInputs: 0, outputChannelCount: [2] })
    node.connect(audioCtx.destination)
    node.port.onmessage = (e) => {
      const m = e.data
      if (m.type === 'frame') onFrame(m)
      else if (m.type === 'ready') say(`— ${build ? build.name + ' ' + build.version : 'firmware'} started —`)
      else if (m.type === 'crash') say(`crashed: ${m.message} (the FM-1 would show its red crash screen and restart)`, true)
    }
    await audioCtx.resume()
    $('poweron').hidden = true
    await reload()
  } catch (err) {
    say(`could not start: ${err.message}`, true)
    $('power').disabled = false
  }
})
$('reset').addEventListener('click', () => reload())
$('wipe').addEventListener('click', () => {
  try {
    localStorage.removeItem(storeKey)
  } catch {}
  reload()
})

// MIDI for the emulator: notes in from a controller, what the firmware sends out to a synth
let midiAccess = null
let midiOut = null
let midiIn = null
const midiBytes = (p) => {
  const st = p & 0xff
  return st >= 0xc0 && st < 0xe0 ? [st, (p >> 8) & 0x7f] : [st, (p >> 8) & 0x7f, (p >> 16) & 0x7f]
}
async function midiPorts() {
  if (midiAccess || !navigator.requestMIDIAccess) return midiAccess
  try {
    midiAccess = await navigator.requestMIDIAccess()
  } catch {
    say('MIDI was not allowed', true)
    return null
  }
  const fill = (sel, ports) => {
    const v = sel.value
    sel.length = 1
    for (const p of ports.values()) sel.add(new Option(p.name, p.id))
    sel.value = v
  }
  const refresh = () => {
    fill($('midiin'), midiAccess.inputs)
    fill($('midiout'), midiAccess.outputs)
  }
  midiAccess.onstatechange = refresh
  refresh()
  return midiAccess
}
for (const id of ['midiin', 'midiout']) $(id).addEventListener('pointerdown', midiPorts, { once: true })
$('midiin').addEventListener('change', async () => {
  const a = await midiPorts()
  if (midiIn) midiIn.onmidimessage = null
  midiIn = a && a.inputs.get($('midiin').value)
  if (midiIn) midiIn.onmidimessage = (e) => (e.data[0] < 0xf0 || [0xf8, 0xfa, 0xfb, 0xfc].includes(e.data[0])) && send({ type: 'midi', data: Array.from(e.data) })
})
$('midiout').addEventListener('change', async () => {
  const a = await midiPorts()
  midiOut = (a && a.outputs.get($('midiout').value)) || null
})

// ---------------------------------------------------------------- install and rescue
const TEXT = {
  start: 'Starting',
  verify: 'Checking the package on the FM-1',
  loader: 'Switching to update mode',
  write: 'Writing',
  reboot: 'Restarting',
  done: 'Done: the FM-1 runs it.',
  stopped: 'Stopped. Replug the USB cable and press the button again: the FM-1 stays in update mode until it is finished.',
  denied: 'MIDI (SysEx) was not allowed. Allow MIDI in the site settings (address bar) and try again.',
  nomidi: 'This browser cannot talk to the FM-1. Use Chrome or Edge.',
  notfound: 'FM-1 not found. Check the USB cable (a data cable), close other apps that use MIDI, and try again.',
  model: 'The connected device is not an FM-1.',
  lost: 'The USB connection was lost. Replug it and try again (an install resumes where it stopped).',
  noloader: 'The FM-1 in update mode was not found. Replug the USB cable and press the button again.',
  noreturn: 'The FM-1 did not come back. Switch it off and on.',
  mismatch: 'Written, but the FM-1 reports another identity: ',
  badreq: 'The FM-1 asked for something outside the package.'
}
const STOCK_SHA256 = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a'
let other = null
let stock = null

function status(text) {
  $('status').textContent = text
}
function log(text) {
  $('log').textContent += text + '\n'
}
addEventListener('beforeunload', (e) => {
  if (busy) e.preventDefault()
})

function setBusy(b) {
  busy = b
  $('install').disabled = b || !(build && build.fwsc)
  $('installother').disabled = $('resumeother').disabled = b || !other
  $('installstock').disabled = b || !stock
  $('resume').disabled = $('check').disabled = b
}

async function midiSysex() {
  if (!navigator.requestMIDIAccess) throw Object.assign(new Error(TEXT.nomidi), { code: 'nomidi' })
  try {
    return await navigator.requestMIDIAccess({ sysex: true })
  } catch {
    throw Object.assign(new Error(TEXT.denied), { code: 'denied' })
  }
}

async function ownPackage() {
  const r = await fetch('firmware.fwsc', { cache: 'no-store' })
  if (!r.ok) throw new Error(`firmware.fwsc: HTTP ${r.status}`)
  return new Uint8Array(await r.arrayBuffer())
}

// resumeOnly: only finish an install the FM-1 is in the middle of
async function run(getPkg, { isStock = false, resumeOnly = false } = {}) {
  location.hash = isStock || resumeOnly ? '#rescue' : '#install'
  $('progressbox').hidden = false
  if (isStock || resumeOnly) $('tab-rescue').append($('progressbox'))
  else $('tab-install').append($('progressbox'))
  $('log').textContent = ''
  $('bar').value = 0
  setBusy(true)
  try {
    const pkg = await getPkg()
    const product = productOf(pkg)
    const image = logicalImage(pkg)
    const total = 74 + Math.ceil(image.length / 512)
    const step = (k, a) => {
      log(`${k} ${a ?? ''}`)
      if (TEXT[k]) status(TEXT[k])
      if (k === 'verify') $('bar').value = (100 * a) / total
      if (k === 'write') $('bar').value = (100 * (74 + a)) / total
      if (k === 'done') $('bar').value = 100
    }
    const up = new Updater(await midiSysex(), log)
    status('Looking for the FM-1')
    const ota = await up.find((id) => /^ota-/i.test(id.model))
    if (ota) {
      ota.link.close()
      log('the FM-1 is in update mode: finishing the install')
      status(TEXT.write)
      if (await up.resume(image, step)) {
        $('bar').value = 100
        status(TEXT.done)
      } else status(TEXT.stopped)
    } else if (resumeOnly) {
      status('No FM-1 in update mode found: nothing to finish. (A normal install is in the Install tab.)')
    } else {
      await up.install(image, product, step)
    }
  } catch (e) {
    if (isStock && e.code === 'mismatch') {
      $('bar').value = 100
      status('Done: the FM-1 runs the official firmware again.')
    } else status((TEXT[e.code] || 'Error: ') + (TEXT[e.code] ? e.detail || '' : e.message))
    log(String((e && e.stack) || e))
  }
  setBusy(false)
}

$('install').addEventListener('click', () => run(ownPackage))
$('installother').addEventListener('click', () => other && run(async () => other))
$('installstock').addEventListener('click', () => stock && run(async () => stock, { isStock: true }))
$('resume').addEventListener('click', () => run(ownPackage, { resumeOnly: true }))
$('resumeother').addEventListener('click', () => other && run(async () => other, { resumeOnly: true }))

$('otherfile').addEventListener('change', async (e) => {
  other = null
  const f = e.target.files[0]
  $('otherinfo').textContent = ''
  if (f) {
    try {
      const raw = new Uint8Array(await f.arrayBuffer())
      const id = productOf(raw)
      if (!/^FM-1_\d+$/.test(id)) throw new Error(`its identity is "${id}"`)
      other = raw
      $('otherinfo').textContent = `${f.name}: identity ${id}${new TextDecoder('latin1').decode(raw).includes('FELUCCA-LOADER-1') ? '' : ' (not a Felucca-family package)'}`
    } catch (err) {
      $('otherinfo').textContent = `Not an FM-1 package: ${err.message}`
    }
  }
  setBusy(busy)
})

$('stockfile').addEventListener('change', async (e) => {
  stock = null
  const f = e.target.files[0]
  $('stockinfo').textContent = ''
  if (f) {
    const raw = new Uint8Array(await f.arrayBuffer())
    const hex = [...new Uint8Array(await crypto.subtle.digest('SHA-256', raw))].map((b) => b.toString(16).padStart(2, '0')).join('')
    if (hex === STOCK_SHA256) {
      stock = raw
      $('stockinfo').textContent = "That is M-VAVE's FM-1 V15."
    } else $('stockinfo').textContent = "That is not M-VAVE's FM-1 V15 file: choose the file from the link above."
  }
  setBusy(busy)
})

$('check').addEventListener('click', async () => {
  setBusy(true)
  $('checkinfo').textContent = 'Looking ...'
  try {
    const up = new Updater(await midiSysex())
    const dev = await up.find()
    if (!dev) $('checkinfo').textContent = TEXT.notfound
    else {
      dev.link.close()
      const t = dev.id.text
      $('checkinfo').textContent = /^ota-/i.test(dev.id.model) ? `${t}: in update mode, an install is not finished (Rescue tab).` : t === 'FM-1_000' ? `${t}: in the USB rescue. Install a firmware.` : build && t === build.identity ? `${t}: runs this project (${build.name} ${build.version}).` : `${t} (on "${dev.name}")`
    }
  } catch (e) {
    $('checkinfo').textContent = e.message
  }
  setBusy(false)
})
