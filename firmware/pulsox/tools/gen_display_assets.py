#!/usr/bin/env python3
"""Generates src/display_assets.h: the fonts and icons of the TFT screens.

The firmware draws the screens from the design in
design-system/Pulsioximetro WiFi · TFT 128x128.html. That design uses Barlow Semi
Condensed and a handful of SVG icons, none of which the Adafruit GFX built-in font
can reproduce. This script turns them into anti-aliased 4-bit alpha masks that
src/display.cpp blends over the frame buffer.

  * Fonts: the woff2 files are embedded in the design file itself, so the result
    matches the mockups and no download is needed. Only the characters the screens
    use are exported (see FONTS). Glyphs are rasterized at 8x and box-filtered down,
    which gives unhinted coverage like the browser's.
  * Icons: the SVG paths of the mockups, rasterized at 16x and box-filtered down.
    Each icon is cropped to its bounding box; the offsets are relative to the
    icon's nominal box (the size of the <svg> element in the mockup).

Requirements (the rest of the firmware does not need them):
    pip install pillow fonttools brotli

Usage, from firmware/pulsox:
    python tools/gen_display_assets.py [--design PATH] [--out PATH] [--preview PNG]

--preview writes a PNG with every font and icon decoded back from the packed
data, magnified 3x: a quick way to check both the rasterization and the packing.
"""

import argparse
import base64
import gzip
import io
import json
import math
import re
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DESIGN = ROOT.parent.parent / "design-system" / "Pulsioxímetro WiFi · TFT 128×128.html"
DEFAULT_OUT = ROOT / "src" / "display_assets.h"

TEXT_SS = 8    # supersampling factor for glyphs
# Light-on-dark text blended in sRGB looks thinner than the mockup, which the browser
# rasterizes at 4x. Raising the coverage to this power (< 1) puts the weight back.
COVERAGE_GAMMA = 0.65
ICON_SS = 16   # supersampling factor for icons
SUB_SIZE = 0.72 * 9  # the "2" of SpO2: .72em of the 9 px label/banner text

# name, weight, size in px, tabular digits, characters to export
FONTS = [
    ("B38", 700, 38.0, True, "0123456789-"),       # SpO2 value
    ("B22", 700, 22.0, True, "0123456789-"),       # BPM value
    ("B20", 700, 20.0, False, "ERO"),              # "ERROR"
    ("B9", 700, 9.0, False, "SpOBAJ¡CRÍTI! "),     # alert banner
    ("B6", 700, SUB_SIZE, False, "2"),             # subscript of the alert banner
    ("S15", 600, 15.0, False, "%"),                # "%" next to the SpO2 value
    ("S13", 600, 13.0, False, "Dedo no encontrado Conectando Colocá el dedo"),  # screen titles
    ("S9", 600, 9.0, False,                        # labels, banner, battery %, status-bar tag, subtitles
     "SpOMIDEN0123456789% COMPLETADO ENVIADO WiFi y nube en el sensor"),
    ("S8", 600, 8.0, False, "BPM"),                # "BPM"
    ("S6", 600, SUB_SIZE, False, "2"),             # subscript of the "SpO2" label
]

HEART_SCALES = 12
HEART_SCALE_MIN, HEART_SCALE_MAX = 0.90, 1.28  # keyframe range of the `beat` animation
HEART_ZONE_W, HEART_ZONE_H = 22, 18            # canvas of one heart frame
HEART_CX, HEART_CY = 11.0, 9.0                 # where the heart's transform origin lands in it


# ---- Design file: bundle unpacking ----------------------------------------------------

def unpack(entry):
    data = base64.b64decode(entry["data"])
    return gzip.decompress(data) if entry.get("compressed") else data


def manifest_of(text):
    m = re.search(r'<script type="__bundler/manifest">(.*?)</script>', text, re.S)
    return json.loads(m.group(1)) if m else None


def load_barlow(design_path):
    """Returns {weight: TTF bytes} of the latin subset of Barlow Semi Condensed."""
    outer = manifest_of(design_path.read_text(encoding="utf-8"))
    if outer is None:
        sys.exit(f"{design_path}: no __bundler/manifest, is it the exported design file?")
    for entry in outer.values():
        page = unpack(entry).decode("utf-8", errors="replace")
        inner = manifest_of(page)
        template = re.search(r'<script type="__bundler/template">\s*(.*?)\s*</script>', page, re.S)
        if inner is None or template is None:
            continue
        css = json.loads(template.group(1))
        faces = re.findall(r'font-weight:\s*(\d+);[^}]*?src:\s*url\("?([0-9a-f-]{36})"?\)', css)
        if not faces:
            continue
        fonts = {}
        for weight, uid in faces:
            ttf = TTFont(io.BytesIO(unpack(inner[uid])))
            ttf.flavor = None
            buf = io.BytesIO()
            ttf.save(buf)
            fonts[int(weight)] = buf.getvalue()
        return fonts
    sys.exit(f"{design_path}: Barlow Semi Condensed not found in the bundle")


def with_tabular_digits(ttf_bytes):
    """Barlow's default digits are proportional; the design asks for tabular-nums
    (OpenType `tnum`), so point the digit code points at the *.tf glyphs."""
    ttf = TTFont(io.BytesIO(ttf_bytes))
    gsub = ttf["GSUB"].table
    lookups = {i for fr in gsub.FeatureList.FeatureRecord if fr.FeatureTag == "tnum"
               for i in fr.Feature.LookupListIndex}
    mapping = {}
    for i in lookups:
        for st in gsub.LookupList.Lookup[i].SubTable:
            st = getattr(st, "ExtSubTable", st)
            mapping.update(getattr(st, "mapping", {}))
    for table in ttf["cmap"].tables:
        for code, glyph in list(table.cmap.items()):
            if glyph in mapping:
                table.cmap[code] = mapping[glyph]
    buf = io.BytesIO()
    ttf.save(buf)
    return buf.getvalue()


# ---- Packing --------------------------------------------------------------------------

def pack4(img):
    """8-bit alpha image -> (packed bytes, w, h). Two pixels per byte, high nibble first,
    row-major, no row padding."""
    w, h = img.size
    q = [round(15 * (a / 255) ** COVERAGE_GAMMA) for a in img.tobytes()]
    if len(q) % 2:
        q.append(0)
    return bytes((q[i] << 4) | q[i + 1] for i in range(0, len(q), 2)), w, h


def unpack4(data, w, h):
    img = Image.new("L", (w, h))
    px = []
    for i in range(w * h):
        byte = data[i >> 1]
        px.append(((byte & 0xF) if i & 1 else (byte >> 4)) * 17)
    img.putdata(px)
    return img


# ---- Fonts ----------------------------------------------------------------------------

def rasterize_font(name, ttf_bytes, size, chars):
    font = ImageFont.truetype(io.BytesIO(ttf_bytes), size * TEXT_SS)
    glyphs, bitmap = [], bytearray()
    for ch in sorted(set(chars)):
        adv16 = round(font.getlength(ch) / TEXT_SS * 16)
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        x0, y0 = math.floor(left / TEXT_SS), math.floor(top / TEXT_SS)
        x1, y1 = math.ceil(right / TEXT_SS), math.ceil(bottom / TEXT_SS)
        w, h = max(x1 - x0, 0), max(y1 - y0, 0)
        off = len(bitmap)
        if ch.strip() and w and h:
            big = Image.new("L", (w * TEXT_SS, h * TEXT_SS), 0)
            ImageDraw.Draw(big).text((-x0 * TEXT_SS, -y0 * TEXT_SS), ch, font=font, fill=255, anchor="ls")
            data, w, h = pack4(big.reduce(TEXT_SS))
            bitmap += data
        else:
            w = h = 0
        glyphs.append(dict(code=ord(ch), off=off, w=w, h=h, xo=x0, yo=y0, adv=adv16))
    return dict(name=name, size=size, glyphs=glyphs, bitmap=bytes(bitmap))


# ---- SVG path flattening --------------------------------------------------------------

def arc_points(p0, rx, ry, rot, large, sweep, p1):
    """SVG endpoint arc -> list of points (spec F.6.5). Circular arcs only (rot = 0)."""
    (x1, y1), (x2, y2) = p0, p1
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    lam = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx
    den = rx * rx * dy * dy + ry * ry * dx * dx
    coef = math.sqrt(max(num / den, 0)) * (-1 if large == sweep else 1)
    cxp, cyp = coef * rx * dy / ry, -coef * ry * dx / rx
    cx, cy = cxp + (x1 + x2) / 2, cyp + (y1 + y2) / 2
    a1 = math.atan2((y1 - cy) / ry, (x1 - cx) / rx)
    a2 = math.atan2((y2 - cy) / ry, (x2 - cx) / rx)
    delta = a2 - a1
    if sweep and delta < 0:
        delta += 2 * math.pi
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    n = max(12, int(abs(delta) * max(rx, ry) * 6))
    return [(cx + rx * math.cos(a1 + delta * i / n), cy + ry * math.sin(a1 + delta * i / n))
            for i in range(n + 1)]


def flatten_path(d):
    """Subset of SVG path data (M L H V C A Z, absolute and relative) -> list of
    (points, closed) subpaths."""
    tokens = re.findall(r"[MmLlHhVvCcAaZz]|-?(?:\d+\.?\d*|\.\d+)", d)
    subpaths, cur, closed = [], [], False
    pos, start, i, cmd = (0.0, 0.0), (0.0, 0.0), 0, None

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    subpaths.append((cur, True))
                cur, pos = [], start
                continue
        rel = cmd.islower()
        ox, oy = pos if rel else (0.0, 0.0)
        c = cmd.upper()
        if c == "M":
            if cur:
                subpaths.append((cur, False))
            pos = (ox + num(), oy + num())
            start, cur = pos, [pos]
            cmd = "l" if rel else "L"   # extra pairs after M are implicit lineto
        elif c == "L":
            pos = (ox + num(), oy + num())
            cur.append(pos)
        elif c == "H":
            pos = (ox + num(), pos[1])
            cur.append(pos)
        elif c == "V":
            pos = (pos[0], oy + num())
            cur.append(pos)
        elif c == "C":
            p1 = (ox + num(), oy + num())
            p2 = (ox + num(), oy + num())
            p3 = (ox + num(), oy + num())
            for k in range(1, 25):
                t = k / 24
                u = 1 - t
                cur.append((u**3 * pos[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                            u**3 * pos[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
            pos = p3
        elif c == "A":
            rx, ry, rot, large, sweep = num(), num(), num(), int(num()), int(num())
            end = (ox + num(), oy + num())
            cur += arc_points(pos, rx, ry, rot, large, sweep, end)[1:]
            pos = end
    if cur:
        subpaths.append((cur, False))
    return subpaths


def dashes(points, on, off):
    """Splits a polyline into dashes of length `on` separated by gaps of `off`."""
    out, cur, drawing, left = [], [points[0]], True, on
    for a, b in zip(points, points[1:]):
        seg = math.dist(a, b)
        pos = 0.0
        while seg - pos > left:
            pos += left
            t = pos / seg
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            if drawing:
                cur.append(p)
                out.append(cur)
                cur = []
            else:
                cur = [p]
            drawing = not drawing
            left = on if drawing else off
        left -= seg - pos
        if drawing:
            cur.append(b)
    if drawing and len(cur) > 1:
        out.append(cur)
    return out


# ---- Icon canvas -----------------------------------------------------------------------

class Canvas:
    """Coverage canvas in icon units, rasterized at ICON_SS. value=255 paints, value=0
    cuts a hole (so one mask can carry a shape with a punched-out symbol)."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("L", (round(w * ICON_SS), round(h * ICON_SS)), 0)
        self.d = ImageDraw.Draw(self.img)

    def _xy(self, pts):
        return [(x * ICON_SS, y * ICON_SS) for x, y in pts]

    def poly(self, pts, value=255):
        self.d.polygon(self._xy(pts), fill=value)

    def circle(self, cx, cy, r, value=255):
        self.d.ellipse([(cx - r) * ICON_SS, (cy - r) * ICON_SS,
                        (cx + r) * ICON_SS - 1, (cy + r) * ICON_SS - 1], fill=value)

    def rrect(self, x, y, w, h, r, value=255):
        self.d.rounded_rectangle([x * ICON_SS, y * ICON_SS, (x + w) * ICON_SS - 1, (y + h) * ICON_SS - 1],
                                 radius=r * ICON_SS, fill=value)

    def rrect_outline(self, x, y, w, h, r, stroke):
        hs = stroke / 2
        self.rrect(x - hs, y - hs, w + stroke, h + stroke, r + hs, 255)
        self.rrect(x + hs, y + hs, w - stroke, h - stroke, max(r - hs, 0), 0)

    def stroke(self, pts, width, value=255, closed=False):
        """Polyline with round caps and joins."""
        if closed:
            pts = pts + [pts[0]]
        px = self._xy(pts)
        self.d.line(px, fill=value, width=round(width * ICON_SS), joint="curve")
        for x, y in (px[0], px[-1]):
            r = width * ICON_SS / 2
            self.d.ellipse([x - r, y - r, x + r - 1, y + r - 1], fill=value)

    def path_stroke(self, d, width, dash=None, value=255, shift=(0.0, 0.0)):
        for pts, closed in flatten_path(d):
            pts = [(x + shift[0], y + shift[1]) for x, y in pts]
            if dash:
                for piece in dashes(pts, *dash):
                    self.stroke(piece, width, value)
            else:
                self.stroke(pts, width, value, closed)

    def path_fill(self, d, value=255, transform=None):
        for pts, _ in flatten_path(d):
            self.poly([transform(p) for p in pts] if transform else pts, value)

    def mask(self):
        """-> dict(data, w, h, xo, yo), cropped to the non-empty bounding box."""
        small = self.img.reduce(ICON_SS)
        box = small.point(lambda a: 255 if a >= 8 else 0).getbbox()  # ignore sub-1/16 coverage specks
        if box is None:
            return dict(data=b"", w=0, h=0, xo=0, yo=0)
        data, w, h = pack4(small.crop(box))
        return dict(data=data, w=w, h=h, xo=box[0], yo=box[1])


HEART_PATH = ("M8 13.5C8 13.5 1 9 1 4.8C1 2.5 2.7 1 4.8 1C6.2 1 7.4 1.7 8 2.9C8.6 1.7 9.8 1 11.2 1"
              "C13.3 1 15 2.5 15 4.8C15 9 8 13.5 8 13.5Z")


def make_icons():
    icons = {}

    # WiFi, 16x11. The <svg> has viewBox "-1 0 16 11", hence the +1 shift in x.
    c = Canvas(16, 11)
    for arc in ("M4.45 7.45A3.6 3.6 0 0 1 9.55 7.45",
                "M2.55 5.55A6.3 6.3 0 0 1 11.45 5.55",
                "M0.64 3.64A9 9 0 0 1 13.36 3.64"):
        c.path_stroke(arc, 1.5, shift=(1, 0))
    c.circle(8, 9.6, 1.1)
    icons["WIFI"] = c.mask()

    # Battery outline + nub, 22x10. The charge level is drawn by the firmware.
    c = Canvas(22, 10)
    c.rrect_outline(0.5, 0.5, 18, 9, 2, 1)
    c.rrect(19.5, 3, 2, 4, 0.8)
    icons["BATTERY"] = c.mask()

    # Caution triangle, 9x8, exclamation mark punched out so the banner color shows through.
    c = Canvas(9, 8)
    c.path_fill("M4.5 .6L8.4 7.4H.6Z")
    c.stroke([(4.5, 3), (4.5, 5.1)], 1, value=0)
    c.circle(4.5, 6.2, 0.55, value=0)
    icons["WARN"] = c.mask()

    # Alarm disc, 9x9, same idea.
    c = Canvas(9, 9)
    c.circle(4.5, 4.5, 4.2)
    c.stroke([(4.5, 2.1), (4.5, 4.8)], 1.1, value=0)
    c.circle(4.5, 6.6, 0.6, value=0)
    icons["ALARM"] = c.mask()

    # "No finger" sensor, 42x40, in four layers: the arc is animated on its own and the
    # X is black over the red badge.
    c = Canvas(42, 40)
    c.rrect_outline(2.5, 28.5, 35, 10, 3, 1.5)
    c.circle(10, 33.5, 1.8)
    c.rrect(17.5, 32, 5, 3, 0.6)
    c.circle(30, 33.5, 1.8)
    icons["NOFINGER_BODY"] = c.mask()
    c = Canvas(42, 40)
    c.path_stroke("M12 28V12a8 8 0 0 1 16 0V28", 1.5, dash=(2.5, 2.5))
    icons["NOFINGER_ARC"] = c.mask()
    c = Canvas(42, 40)
    c.circle(34, 8, 6)
    icons["NOFINGER_BADGE"] = c.mask()
    c = Canvas(42, 40)
    c.stroke([(31.4, 5.4), (36.6, 10.6)], 1.7)
    c.stroke([(36.6, 5.4), (31.4, 10.6)], 1.7)
    icons["NOFINGER_X"] = c.mask()

    # Big WiFi for the "connecting" screen, 48x34: the arcs of the status-bar icon at 3x, each
    # in its own mask so the firmware can light them one after the other.
    for i, arc in enumerate(("M13.35 22.35A10.8 10.8 0 0 1 28.65 22.35",
                             "M7.65 16.65A18.9 18.9 0 0 1 34.35 16.65",
                             "M1.92 10.92A27 27 0 0 1 40.08 10.92"), start=1):
        c = Canvas(48, 34)
        c.path_stroke(arc, 3.2, shift=(3, 0))
        icons[f"WIFIBIG_ARC{i}"] = c.mask()
    c = Canvas(48, 34)
    c.circle(24, 28.8, 3.3)
    icons["WIFIBIG_DOT"] = c.mask()

    # Heart at HEART_SCALES sizes, scaled about the svg's transform-origin (8, 7).
    hearts = []
    for k in range(HEART_SCALES):
        s = HEART_SCALE_MIN + (HEART_SCALE_MAX - HEART_SCALE_MIN) * k / (HEART_SCALES - 1)
        c = Canvas(HEART_ZONE_W, HEART_ZONE_H)
        c.path_fill(HEART_PATH, transform=lambda p, s=s: (HEART_CX + s * (p[0] - 8), HEART_CY + s * (p[1] - 7)))
        hearts.append(c.mask())
    icons["HEART"] = hearts
    return icons


# ---- Output ----------------------------------------------------------------------------

def c_bytes(data):
    if not data:
        return "  0x00"
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return "  " + ",\n  ".join(rows)


def emit(fonts, icons, design_name):
    o = [f"""// GENERATED by tools/gen_display_assets.py from "{design_name}". DO NOT EDIT BY HAND:
// change the script (FONTS, icon paths) and run it again.
//
// Fonts and icons of the TFT screens as 4-bit alpha masks: two pixels per byte, high
// nibble first, row-major, no row padding. 0 = transparent, 15 = opaque.
#pragma once
#include <stdint.h>

// One glyph of a font. The bitmap's top-left corner is at (xo, yo) from the pen position
// on the baseline (yo is negative above the baseline). `adv` is the advance in 1/16 px.
struct DispGlyph {{
  uint16_t code;  // Unicode code point
  uint16_t off;   // byte offset of the bitmap in the font's bitmap array
  uint8_t w, h;   // bitmap size in px (0 for a blank such as the space)
  int8_t xo, yo;
  uint16_t adv;
}};

struct DispFont {{
  const DispGlyph *glyphs;
  uint8_t count;
  const uint8_t *bitmap;
}};

// One icon mask, cropped to its bounding box. (xo, yo) is the bitmap's top-left corner
// relative to the icon's nominal box (the size of the <svg> in the mockup).
struct DispIcon {{
  const uint8_t *bitmap;
  uint8_t w, h;
  int8_t xo, yo;
}};
"""]

    o.append("// ---- Fonts " + "-" * 66)
    for f in fonts:
        n = f["name"]
        o.append(f"\n// {n}: Barlow Semi Condensed, {f['size']:g} px, "
                 f"chars \"{''.join(chr(g['code']) for g in f['glyphs'])}\"")
        o.append(f"static const uint8_t FONT_{n}_BITMAP[] = {{\n{c_bytes(f['bitmap'])}\n}};")
        rows = ",\n".join(
            f"  {{0x{g['code']:04X}, {g['off']}, {g['w']}, {g['h']}, {g['xo']}, {g['yo']}, {g['adv']}}}"
            for g in f["glyphs"])
        o.append(f"static const DispGlyph FONT_{n}_GLYPHS[] = {{\n{rows}\n}};")
        o.append(f"static const DispFont FONT_{n} = {{FONT_{n}_GLYPHS, {len(f['glyphs'])}, FONT_{n}_BITMAP}};")

    o.append("\n// ---- Icons " + "-" * 66)
    for name, ic in icons.items():
        if isinstance(ic, list):
            continue
        o.append(f"\nstatic const uint8_t ICON_{name}_BITMAP[] = {{\n{c_bytes(ic['data'])}\n}};")
        o.append(f"static const DispIcon ICON_{name} = {{ICON_{name}_BITMAP, {ic['w']}, {ic['h']}, {ic['xo']}, {ic['yo']}}};")

    o.append(f"""
// Heart frames, HEART_SCALES sizes from {HEART_SCALE_MIN:g}x to {HEART_SCALE_MAX:g}x (the `beat` keyframe range).
// Each one lives in a {HEART_ZONE_W}x{HEART_ZONE_H} zone whose point ({HEART_CX:g}, {HEART_CY:g}) is the heart's center.
#define HEART_SCALES {HEART_SCALES}
#define HEART_SCALE_MIN {HEART_SCALE_MIN:g}f
#define HEART_SCALE_MAX {HEART_SCALE_MAX:g}f
#define HEART_ZONE_W {HEART_ZONE_W}
#define HEART_ZONE_H {HEART_ZONE_H}
#define HEART_CX {int(HEART_CX)}
#define HEART_CY {int(HEART_CY)}""")
    for k, ic in enumerate(icons["HEART"]):
        o.append(f"static const uint8_t ICON_HEART{k}_BITMAP[] = {{\n{c_bytes(ic['data'])}\n}};")
    rows = ",\n".join(f"  {{ICON_HEART{k}_BITMAP, {ic['w']}, {ic['h']}, {ic['xo']}, {ic['yo']}}}"
                      for k, ic in enumerate(icons["HEART"]))
    o.append(f"static const DispIcon ICON_HEART[HEART_SCALES] = {{\n{rows}\n}};")
    return "\n".join(o) + "\n"


# ---- Preview ---------------------------------------------------------------------------

def preview(fonts, icons, path):
    """Decodes everything back from the packed bytes and lays it out, magnified 3x."""
    z = 3
    cells = []
    for f in fonts:
        w = sum(g["adv"] for g in f["glyphs"]) // 16 + 8
        h = int(f["size"] * 1.5) + 4
        img = Image.new("RGB", (w, h), (0, 0, 0))
        pen, base = 2.0, int(f["size"] * 1.05) + 1
        for g in f["glyphs"]:
            if g["w"]:
                a = unpack4(f["bitmap"][g["off"]:g["off"] + (g["w"] * g["h"] + 1) // 2], g["w"], g["h"])
                img.paste((255, 255, 255), (int(round(pen)) + g["xo"], base + g["yo"]), a)
            pen += g["adv"] / 16
        cells.append(img)
    flat = [(n, ic) for n, ic in icons.items() if not isinstance(ic, list)]
    flat += [(f"HEART{k}", ic) for k, ic in enumerate(icons["HEART"])]
    for n, ic in flat:
        img = Image.new("RGB", (48, 44), (0, 0, 0))
        if ic["w"]:
            a = unpack4(ic["data"], ic["w"], ic["h"])
            img.paste((90, 199, 255), (2 + ic["xo"], 2 + ic["yo"]), a)
        cells.append(img)
    width, x, y, rowh = 760, 0, 0, 0
    sheet = Image.new("RGB", (width, 900), (24, 28, 32))
    for img in cells:
        big = img.resize((img.width * z, img.height * z), Image.NEAREST)
        if x + big.width > width:
            x, y, rowh = 0, y + rowh + 6, 0
        sheet.paste(big, (x, y))
        x += big.width + 6
        rowh = max(rowh, big.height)
    sheet.crop((0, 0, width, y + rowh + 4)).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--design", type=Path, default=DEFAULT_DESIGN)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--preview", type=Path, help="write a PNG preview of the generated fonts and icons")
    args = ap.parse_args()

    barlow = load_barlow(args.design)
    fonts = []
    for name, weight, size, tnum, chars in FONTS:
        ttf = with_tabular_digits(barlow[weight]) if tnum else barlow[weight]
        fonts.append(rasterize_font(name, ttf, size, chars))
    icons = make_icons()

    args.out.write_text(emit(fonts, icons, args.design.name), encoding="utf-8", newline="\n")
    nbytes = sum(len(f["bitmap"]) for f in fonts) + sum(
        len(ic["data"]) for v in icons.values() for ic in (v if isinstance(v, list) else [v]))
    print(f"{args.out}: {nbytes} bytes of bitmaps, {len(fonts)} fonts, "
          f"{len(icons) - 1 + HEART_SCALES} icon masks")
    if args.preview:
        preview(fonts, icons, args.preview)
        print(f"preview: {args.preview}")


if __name__ == "__main__":
    main()
