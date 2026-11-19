#!/usr/bin/env python3
"""Self-test of check_persian_appearance.py: it must pass a correct appearance and fail every deliberately broken one.

A checker that passes everything is worse than no checker, so this builds small PDFs whose appearance is a picture of
the text, correct or broken in exactly the ways the pinned PDFium breaks Persian, and requires the right check to fail:

    good                  the text shaped correctly                         every check passes
    dropped_letter        the Farsi yeh is missing (PDFium's Windows-1256 loss)   reference fails
    isolated_forms        every letter drawn alone, nothing joined (PDFium)     joined and reference fail
    wrong_word_order      the words in the wrong order                          reference fails
    wrong_value           a correct picture but a different stored value        value fails
    font_text             text in a non-embedded font (PDFium)                  portable fails
    blank                 an empty picture                                      render fails
    no_appearance         no /AP at all                                         portable fails

and, when the file exists, real output of the viewer's FreeText boxes (tests/fixtures/persian_freetext_production_sample.pdf),
which must pass completely.

Usage:   python3 -I selftest_check_persian_appearance.py [--keep DIR]
Exit:    0 every expectation held, 1 an expectation failed, 77 a dependency is missing (CTest: SKIP_RETURN_CODE 77)
Needs:   pikepdf, numpy, scipy, Pillow with raqm, Poppler (pdftoppm), PyMuPDF, and an installed font with Persian letters
"""
import contextlib
import importlib.util
import io
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77


def load_checker():
    spec = importlib.util.spec_from_file_location("check_persian_appearance", os.path.join(HERE, "check_persian_appearance.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# the two test texts: "salaam donya" (hello world) and "naam khaanevaadegi" (family name, has gaf and the Farsi yeh)
TEXTS = ["سلام دنیا", "نام خانوادگی"]
FARSI_YEH = "ی"


def pictures(chk, text, font_path):
    """The picture variants of one text (PIL images, dark ink on white)."""
    from PIL import Image, ImageDraw, ImageFont
    size = 120

    def draw(t, direction="rtl"):
        f = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.RAQM)
        l, top, r, b = f.getbbox(t, direction=direction, language="fa")
        im = Image.new("L", (r - l + 60, b - top + 60), 255)
        ImageDraw.Draw(im).text((30 - l, 30 - top), t, font=f, fill=0, direction=direction, language="fa")
        return im

    def isolated(t):
        parts = [draw(ch) if ch.strip() else Image.new("L", (size // 3, 10), 255) for ch in t if ch != "‌"]
        gap = 30
        total = sum(p.width - 2 * gap for p in parts) + 60
        height = max(p.height for p in parts)
        im = Image.new("L", (total, height), 255)
        x = total - 30
        for p in parts:
            x -= p.width - 2 * gap
            im.paste(p, (x - gap, (height - p.height) // 2))
        return im

    words = text.split()
    return {
        "good": draw(text),
        "dropped_letter": draw(text.replace(FARSI_YEH, "", 1)),
        "isolated_forms": isolated(text),
        "wrong_word_order": draw(" ".join(reversed(words))),
    }


def build_pdf(path, value, kind, picture=None):
    """One page with one text field named 'persian' (300 x 40 pt) whose appearance is `picture` or something else."""
    import pikepdf
    from pikepdf import Array, Dictionary, Name, String, Stream
    pdf = pikepdf.new()
    pdf.add_blank_page(page_size=(595, 842))
    rect = [72, 700, 372, 740]
    w_pt, h_pt = rect[2] - rect[0], rect[3] - rect[1]
    widget = pdf.make_indirect(Dictionary(Type=Name.Annot, Subtype=Name.Widget, FT=Name.Tx, T=String("persian"), V=String(value),
                                          Rect=Array(rect), F=4, DA=String("/Helv 12 Tf 0 g")))
    if kind == "image":
        iw, ih = picture.size
        scale = min(w_pt / iw, h_pt * 0.8 / ih)
        dw, dh = iw * scale, ih * scale
        im = Stream(pdf, picture.tobytes())
        im["/Type"], im["/Subtype"] = Name.XObject, Name.Image
        im["/Width"], im["/Height"], im["/ColorSpace"], im["/BitsPerComponent"] = iw, ih, Name.DeviceGray, 8
        content = ("q %.4f 0 0 %.4f %.4f %.4f cm /Im0 Do Q" % (dw, dh, (w_pt - dw) / 2, (h_pt - dh) / 2)).encode()
        ap = Stream(pdf, content)
        ap["/Type"], ap["/Subtype"], ap["/BBox"] = Name.XObject, Name.Form, Array([0, 0, w_pt, h_pt])
        ap["/Resources"] = Dictionary(XObject=Dictionary(Im0=im))
        widget["/AP"] = Dictionary(N=ap)
    elif kind == "font_text":
        font = pdf.make_indirect(Dictionary(Type=Name.Font, Subtype=Name.TrueType, BaseFont=Name("/FreeSerif,Bold"), Encoding=Name.WinAnsiEncoding))
        ap = Stream(pdf, b"BT /F1 14 Tf 10 12 Td (not outlines) Tj ET")
        ap["/Type"], ap["/Subtype"], ap["/BBox"] = Name.XObject, Name.Form, Array([0, 0, w_pt, h_pt])
        ap["/Resources"] = Dictionary(Font=Dictionary(F1=font))
        widget["/AP"] = Dictionary(N=ap)
    elif kind != "none":
        raise ValueError(kind)
    pdf.pages[0]["/Annots"] = Array([widget])
    pdf.Root["/AcroForm"] = Dictionary(Fields=Array([widget]))
    pdf.save(path)


def run_checker(chk, argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        code = chk.main(argv)
    rows = []
    for line in buf.getvalue().splitlines():
        if line.startswith("[") and "]" in line:
            status, rest = line[1:].split("]", 1)
            rest = rest.strip()
            check, _, detail = rest.partition(" ")
            rows.append((status, check, detail.strip()))
    return code, rows, buf.getvalue()


def main():
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None
    try:
        import numpy, pikepdf, scipy  # noqa: F401
        from PIL import features
        import pymupdf  # noqa: F401
    except ImportError as e:
        print("SKIP: missing dependency (%s)" % e)
        return SKIP
    if not (shutil.which("pdftoppm") and features.check("raqm")):
        print("SKIP: needs pdftoppm and Pillow with raqm")
        return SKIP
    chk = load_checker()
    fonts = chk.candidate_fonts(TEXTS[1], ["auto"])
    if not fonts:
        print("SKIP: no installed font has the Persian letters used here")
        return SKIP
    font_path = next((f for f in fonts if os.path.basename(f) in ("DejaVuSans.ttf", "FreeSerif.ttf")), fonts[0])
    work = keep or tempfile.mkdtemp(prefix="persian_selftest_")
    os.makedirs(work, exist_ok=True)

    # sample -> {check: expected status}; every line of that check must have that status
    ALL_PASS = {"value": "PASS", "portable": "PASS", "render": "PASS", "agree": "PASS", "joined": "PASS", "reference": "PASS"}
    expectations = []  # (label, pdf path, extra args, {check: status})
    for ti, text in enumerate(TEXTS):
        pics = pictures(chk, text, font_path)
        samples = [("good", "image", pics["good"], text, ALL_PASS),
                   ("dropped_letter", "image", pics["dropped_letter"], text, {"value": "PASS", "portable": "PASS", "reference": "FAIL"}),
                   ("isolated_forms", "image", pics["isolated_forms"], text, {"value": "PASS", "joined": "FAIL", "reference": "FAIL"}),
                   ("wrong_word_order", "image", pics["wrong_word_order"], text, {"value": "PASS", "joined": "PASS", "reference": "FAIL"}),
                   ("wrong_value", "image", pics["good"], text[:-1] + "م", {"value": "FAIL", "portable": "PASS", "reference": "PASS"}),
                   ("font_text", "font_text", None, text, {"value": "PASS", "portable": "FAIL"}),
                   ("blank", "image", __import__("PIL.Image", fromlist=["Image"]).new("L", (400, 80), 255), text, {"portable": "PASS", "render": "FAIL"}),
                   ("no_appearance", "none", None, text, {"value": "PASS", "portable": "FAIL"})]
        for name, kind, pic, value, expected in samples:
            path = os.path.join(work, "t%d_%s.pdf" % (ti, name))
            # the checker is told the TEXT it should find; wrong_value stores a different one
            build_pdf(path, value, kind, pic)
            expectations.append(("text %d / %s" % (ti + 1, name), path, ["--field", "persian", "--expect", text, "--font", font_path], expected))
    production = os.path.join(HERE, "..", "fixtures", "persian_freetext_production_sample.pdf")
    if os.path.exists(production):
        expectations.append(("production FreeText sample", production, ["--all-freetext"], ALL_PASS))

    failures = 0
    print("%-30s %-9s %-9s %s" % ("sample", "expected", "got", "check"))
    for label, path, extra, expected in expectations:
        code, rows, text_out = run_checker(chk, [path] + extra)
        for check, want in expected.items():
            got = [status for status, c, _ in rows if c == check]
            ok = bool(got) and all(g == want for g in got) if want != "FAIL" else ("FAIL" in got)
            if want == "PASS" and "SKIP" in got and not any(g == "FAIL" for g in got):
                ok = all(g in ("PASS", "SKIP") for g in got) and "PASS" in got
            if not ok:
                failures += 1
            detail = next((d for s, c, d in rows if c == check and s == ("FAIL" if "FAIL" in got else got[0] if got else "")), "")
            print("%-30s %-9s %-9s %-9s %s %s" % (label, want, "/".join(sorted(set(got))) or "none", check, "ok " if ok else "WRONG", detail[:70]))
    if not keep:
        shutil.rmtree(work, ignore_errors=True)
    print("=== %s: %d expectation(s) violated" % ("FAILED" if failures else "OK", failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
