#!/usr/bin/env python3
"""Generates the two non-Latin-text fixtures used by Test 20 in test_worker_ipc.cpp.

  unicode_embedded.pdf     DejaVu Sans embedded (subset): Latin, Cyrillic, Greek and Persian lines.
                           Needs no system font, so it shows that text still renders in the Landlock-confined
                           worker (and in the Flatpak sandbox) when the glyphs come from the document.
  unicode_system_font.pdf  Japanese in the NON-embedded CID font HeiseiMin-W3: PDFium has to find a CJK font on
                           the system through fontconfig, which is exactly what the worker's Landlock read
                           allowlist (/usr/share/fonts, /etc/fonts, ...) must keep possible.

Requires: reportlab, arabic-reshaper, python-bidi, and DejaVuSans.ttf. The generated PDFs are committed; CI does
not run this script. Layout contract (the C++ test relies on it): the page is A4 portrait, each line sits in its
own 70 px band starting at y=60 px at 1.0 scale (72 dpi), all inside the first 512x512 tile.
"""
import sys
import arabic_reshaper
from bidi.algorithm import get_display
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

DEJAVU = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "."
PAGE_H = A4[1]
BAND_TOP = 60   # px from the top of the page
BAND_H = 70


def baseline(i):
    return PAGE_H - (BAND_TOP + i * BAND_H + 45)


def persian(text):
    return get_display(arabic_reshaper.reshape(text))


pdfmetrics.registerFont(TTFont("DejaVuSans", DEJAVU))
c = canvas.Canvas(f"{OUT}/unicode_embedded.pdf", pageCompression=0, invariant=1)
c.setTitle("Unicode embedded-font fixture")
c.setFont("DejaVuSans", 32)
lines = [
    "Hello, World 123",
    "Привет, мир",
    "Γειά σου Κόσμε",
    persian("سلام دنیا ۱۲۳"),
]
for i, t in enumerate(lines):
    c.drawString(50, baseline(i), t)
c.save()

pdfmetrics.registerFont(UnicodeCIDFont("HeiseiMin-W3"))
c = canvas.Canvas(f"{OUT}/unicode_system_font.pdf", pageCompression=0, invariant=1)
c.setTitle("Unicode system-font fixture")
c.setFont("HeiseiMin-W3", 32)
c.drawString(50, baseline(0), "日本語のテキスト")
c.drawString(50, baseline(1), "漢字とかな")
c.save()
