#!/usr/bin/env python3
"""Erzeugt die Schriften fuer das Display als VLW-Daten (Graustufen, geglaettet)
in src/fonts.h. Nur die Zeichen, die das Display wirklich braucht, landen im Flash.

Schrift: Inter (SIL Open Font License) – https://rsms.me/inter/
Aufruf:  python3 tools/make_fonts.py [Pfad-zu-den-Inter-OTF-Dateien]
"""
import struct, sys, os
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter"
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "fonts.h")

TEXT = "".join(chr(c) for c in range(0x20, 0x7F)) + "ÄÖÜäöüß°·µ€§´"
NUM = " 0123456789,.-+°%CHVglmp/"

# Name, Datei, Pixelgroesse, Zeichen
FONTS = [
    ("FONT_S",  "Inter-Medium.otf",          20, TEXT),   # kleine Hinweise
    ("FONT_T",  "Inter-Medium.otf",          26, TEXT),   # Titel, Kopfzeile, Zusatzzeilen
    ("FONT_B",  "Inter-SemiBold.otf",        32, TEXT),   # Tasten, Dialoge
    ("FONT_M",  "Inter-SemiBold.otf",        44, TEXT),   # Status Pumpe/Heizung
    ("FONT_L",  "InterDisplay-SemiBold.otf", 64, NUM),    # Messwerte (lang)
    ("FONT_XL", "InterDisplay-SemiBold.otf", 92, NUM),    # Messwerte (gross)
]


def vlw(path, size, chars):
    f = ImageFont.truetype(path, size)
    glyphs = []
    for ch in sorted(set(chars), key=ord):
        adv = int(round(f.getlength(ch)))
        if ch == " ":
            glyphs.append((ord(ch), 0, 0, adv, 0, 0, b""))
            continue
        pad = size
        img = Image.new("L", (size * 3, size * 3), 0)
        ox, oy = pad, pad * 2
        ImageDraw.Draw(img).text((ox, oy), ch, font=f, fill=255, anchor="ls")
        box = img.getbbox()
        if not box:
            glyphs.append((ord(ch), 0, 0, adv, 0, 0, b""))
            continue
        x0, y0, x1, y1 = box
        bmp = img.crop(box).tobytes()
        glyphs.append((ord(ch), y1 - y0, x1 - x0, adv, oy - y0, x0 - ox, bmp))

    def top(c):
        g = next((g for g in glyphs if g[0] == ord(c)), None)
        return g[4] if g else size
    def bottom(c):
        g = next((g for g in glyphs if g[0] == ord(c)), None)
        return (g[1] - g[4]) if g else 0
    ascent = top("d") if "d" in chars else top("0")
    descent = bottom("p") if "p" in chars else 0

    out = struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)
    for u, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", u, h, w, adv, dy, dx, 0)
    for g in glyphs:
        out += g[6]
    return out


def main():
    parts = ["// Automatisch erzeugt von tools/make_fonts.py – nicht von Hand aendern.",
             "// Schrift: Inter (SIL Open Font License 1.1), https://rsms.me/inter/",
             "#pragma once", "#include <stdint.h>", ""]
    total = 0
    for name, file, size, chars in FONTS:
        data = vlw(os.path.join(FONT_DIR, file), size, chars)
        total += len(data)
        print(f"{name:8s} {file:28s} {size:3d}px {len(set(chars)):3d} Zeichen {len(data)/1024:6.1f} KB")
        parts.append(f"// {file}, {size} px")
        parts.append(f"static const uint8_t {name}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            parts.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
        parts.append("};")
        parts.append("")
    with open(OUT, "w") as fh:
        fh.write("\n".join(parts))
    print(f"gesamt {total/1024:.1f} KB -> {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
