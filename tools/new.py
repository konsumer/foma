#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Turn this copy of foma into your own firmware project (once, right after you copy the template).

  ./fm1 new "MY SYNTH" [--repo https://github.com/you/my-synth] [--identity FM-1_96]

Renames the firmware (firmware.json, the starter's title screen), points the page's Source link at
your repo (default: the git remote "origin"), writes a README for your project, adds you to
LICENSING.md, and removes what only foma itself needs: the Dockerfile and its workflow (the
toolchain comes from Docker Hub) and this script.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOMA = "https://github.com/konsumer/foma"
TEMPLATE_ONLY = ["Dockerfile", ".github/workflows/toolchain.yml", "tools/new.py"]


def origin():
  """the git remote "origin" as an https URL, or None"""
  cfg = ROOT / ".git" / "config"
  if not cfg.exists():
    return None
  m = re.search(r'\[remote "origin"\][^\[]*?url\s*=\s*(\S+)', cfg.read_text())
  if not m:
    return None
  url = re.sub(r"^git@([^:]+):", r"https://\1/", m[1])
  return re.sub(r"\.git$", "", url)


def edit(path, old, new, count=1):
  p = ROOT / path
  s = p.read_text()
  if old not in s:
    print(f"  (skipped {path}: already changed)")
    return
  p.write_text(s.replace(old, new, count))
  print(f"  {path}")


def pages_url(repo):
  m = re.fullmatch(r"https://github\.com/([^/]+)/([^/]+)", repo or "")
  return f"https://{m[1].lower()}.github.io/{m[2]}/" if m else None


def readme(name, slug, repo):
  try_it = f"\nTry it in the browser, and install it on the FM-1 from Chrome or Edge: <{pages_url(repo)}>\n" if pages_url(repo) else ""
  return f"""# {name}

Firmware for the M-VAVE FM-1, made with [foma]({FOMA}).
{try_it}
## Develop

Only Docker is needed:

```sh
./fm1 dev          # http://localhost:8000: the emulator, the installer, the rescue; rebuilds when you save
./fm1 build        # build/{slug}-VERSION.fwsc
```

The firmware is in `src/`; what it can call is in [platform/fm1.h](platform/fm1.h). Name, version and package
identity: [firmware.json](firmware.json). The rules of the chip (no floats, keep `audio()` short, ...) are in
[foma's README]({FOMA}#the-rules-of-the-chip).

Every push to `main` publishes the emulator and the installer to GitHub Pages (Settings → Pages → Source: GitHub
Actions); a tag (`v0.2`) also makes a release with the `.fwsc`.

Installing firmware is at your own risk. If the FM-1 does not start, hold OCT- while switching it on (the USB rescue),
then install from the page.

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
"""


def main():
  ap = argparse.ArgumentParser(description="make this copy of foma your own firmware")
  ap.add_argument("name", help='the firmware\'s name, e.g. "MY SYNTH" (up to 16 capitals, digits, spaces, _.+-)')
  ap.add_argument("--repo", help="your repo's URL, for the page's Source link (default: the git remote origin)")
  ap.add_argument("--identity", help="your package identity, FM-1_ and two digits (default: keep the current one)")
  a = ap.parse_args()

  name = re.sub(r"[^A-Z0-9 _.+-]", "", a.name.upper()).strip()[:16]
  if not name:
    raise SystemExit("new: the name needs letters or digits")
  if a.identity and not re.fullmatch(r"FM-1_\d\d", a.identity):
    raise SystemExit("new: --identity looks like FM-1_96 (the third digit is your minor version)")
  slug = re.sub(r"[^a-z0-9.-]+", "-", name.lower()).strip("-")
  repo = (a.repo or origin() or "").rstrip("/") or None
  if repo == FOMA:
    repo = None  # a fresh clone of foma itself: there is no repo of yours yet

  print(f"making {name} ...")
  cfg = json.loads((ROOT / "firmware.json").read_text())
  cfg["name"] = name
  cfg["version"] = "0.1"
  if a.identity:
    cfg["identity"] = a.identity
  (ROOT / "firmware.json").write_text(json.dumps(cfg, indent=2) + "\n")
  print("  firmware.json")

  edit("src/main.c", 'draw_centered(20, "FOMA", ACCENT, FONT_BIG);', f"draw_centered(20, {json.dumps(name)}, ACCENT, FONT_BIG);")

  link = f'<a class="source" href="{FOMA}" target="_blank" rel="noopener">Source</a>'
  if repo:
    edit("web/index.html", link, f'<a class="source" href="{repo}" target="_blank" rel="noopener">Source</a>')
  else:
    edit("web/index.html", f"\n        {link}", "")

  edit(
    "LICENSING.md",
    "\n## Third-party material",
    f"\n{name}: `src/`, `firmware.json`, and its changes to the rest, by its authors, on [foma]({FOMA}).\n\n## Third-party material",
  )

  (ROOT / "README.md").write_text(readme(name, slug, repo))
  print("  README.md")

  for f in TEMPLATE_ONLY:
    p = ROOT / f
    if p.exists():
      p.unlink()
      print(f"  removed {f}")

  print(f"\n{name} is yours. Next:")
  if not a.identity:
    print(f"  - pick your own package identity in firmware.json (now {cfg['identity']}x); see the FM-1 Firmware Hub for taken ones")
  if not repo:
    print("  - no repo found (no --repo, no git remote origin), so the page has no Source link: add one in web/index.html")
  print("  - ./fm1 dev")
  return 0


if __name__ == "__main__":
  sys.exit(main())
