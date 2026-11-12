#!/usr/bin/env python3
"""Checks that the STORED APPEARANCE of a Persian (Arabic-script) text field or text box is correct and portable.

Why this exists: the pinned PDFium builds a form field's appearance itself, and for Persian it gets it wrong: it
drops letters that Windows-1256 lacks (the Farsi yeh), draws every letter unjoined, and refers to a system font by
name without embedding it (MuPDF shows garbage for the same file). The value (/V) is fine, only the picture is not, so
a test that only reads the value cannot see it. This tool looks at the picture, in several independent PDF readers.

Usage
    check_persian_appearance.py FILE.pdf --field NAME      --expect "TEXT"   [--page N] [--control N]
    check_persian_appearance.py FILE.pdf --annot-name NM   --expect "TEXT"   (a FreeText box, by its /NM)
    check_persian_appearance.py FILE.pdf --all-freetext                       (every FreeText box; expects its /Contents)

Checks (each prints PASS, FAIL, WARN or SKIP):
    value      the stored value (/V, or /Contents for a text box) equals the expected text exactly
    portable   the appearance draws outlines (or an image): no text operators and no font resources, so it cannot
               depend on a font that is installed on one machine only
    render     the appearance is drawn, not blank and not clipped, in every reader that is installed
    agree      the readers draw the same picture (same outlines in, same ink out)
    joined     the letters are joined the way the script requires: the number of large ink blobs per word matches
               what Unicode's joining rules predict (isolated letters would produce many more). Needs no font
    reference  the picture matches an independent shaping of the expected text (Pillow with HarfBuzz and FriBiDi)
               in an installed font. This is what catches a dropped letter or a wrong order. Needs the font: with
               --font auto every installed font that has all the letters is tried and the best match counts

Readers used when installed: Poppler (pdftoppm), MuPDF (PyMuPDF), PDFium (pypdfium2). Needs pikepdf, numpy, scipy and
Pillow; the reference check also needs Pillow built with raqm.

Exit codes: 0 every check passed (SKIP and WARN do not fail), 1 at least one check failed, 2 usage error or a missing
required dependency.

Limits: single-line text, dark ink on a lighter background, pages without /Rotate. Not tested against Acrobat, Foxit
or Okular.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

EXIT_PASS, EXIT_FAIL, EXIT_USAGE = 0, 1, 2

DPI = 400
MARGIN_PT = 2.0
NORM_W, NORM_H = 640, 64          # masks are stretched to this size before they are compared
DILATE = 4                        # pixels at that size
INK_DELTA = 80                    # a pixel this much different from the background is ink
DOT_AREA = 0.012                  # components smaller than this share of (text height)^2 are dots, not letter runs
MIN_AGREE = 0.90                  # readers must cover each other's ink at least this much
MIN_REFERENCE = 0.88              # the picture must cover an independent shaping of the text at least this much

ZWNJ = "\u200c"
# Arabic-script letters by Unicode joining type (the part of ArabicShaping.txt that matters for Persian):
# R joins only to the letter before it, D joins on both sides, U never joins.
RIGHT_JOINING = set("\u0622\u0623\u0624\u0625\u0627\u062f\u0630\u0631\u0632\u0648\u0629\u0698\u06c0\u0671")
NON_JOINING = set("\u0621")
DUAL_JOINING = set("\u0626\u0628\u062a\u062b\u062c\u062d\u062e\u0633\u0634\u0635\u0636\u0637\u0638\u0639\u063a\u0641\u0642\u0643\u0644\u0645"
                   "\u0646\u0647\u064a\u067e\u0686\u06a9\u06af\u06cc\u06be")
MARKS = set(chr(c) for c in range(0x064B, 0x0660)) | {"\u0670"}
# Letters that some fonts draw in two separate pieces (the upper bars of gaf and kaf): each may add one blob.
MULTI_STROKE = set("\u06af\u06a9\u0643")


class Result:
    def __init__(self):
        self.rows = []

    def add(self, status, check, detail):
        self.rows.append({"status": status, "check": check, "detail": detail})
        print("[%s] %-9s %s" % (status, check, detail))

    @property
    def failed(self):
        return any(r["status"] == "FAIL" for r in self.rows)


# ---------------------------------------------------------------------------------------------- text helpers
def allowed_extra_blobs(text):
    return sum(1 for ch in text if ch in MULTI_STROKE)


def expected_runs(text):
    """Number of connected letter runs the script requires for `text`, or None when the text is not plain Arabic-script
    words (digits, Latin letters or punctuation make the count font dependent)."""
    total = 0
    for word in text.split():
        connected = False
        for ch in word:
            if ch == ZWNJ:
                connected = False
                continue
            if ch in MARKS:
                continue
            if ch in DUAL_JOINING:
                dual = True
            elif ch in RIGHT_JOINING or ch in NON_JOINING:
                dual = False
            else:
                return None
            if not connected:
                total += 1
            connected = dual
    return total


# ---------------------------------------------------------------------------------------------- PDF access
def load_pdf(path):
    import pikepdf
    return pikepdf.open(path)


def qualified_name(annot):
    parts, node = [], annot
    for _ in range(64):
        if node is None:
            break
        t = node.get("/T")
        if t is not None:
            parts.append(str(t))
        node = node.get("/Parent")
    return ".".join(reversed(parts))


def inherited(annot, key):
    node = annot
    for _ in range(64):
        if node is None:
            return None
        v = node.get(key)
        if v is not None:
            return v
        node = node.get("/Parent")
    return None


def find_targets(pdf, args):
    """Yields (page index, annotation) for what the command line selected."""
    import pikepdf
    pages = list(enumerate(pdf.pages))
    if args.page is not None:
        pages = [pages[args.page]]
    seen = 0
    for pi, page in pages:
        for a in page.get("/Annots", []):
            if a is None or not isinstance(a, pikepdf.Dictionary):
                continue
            subtype = str(a.get("/Subtype", ""))
            if args.all_freetext:
                if subtype == "/FreeText":
                    yield pi, a
            elif args.annot_name is not None:
                nm = a.get("/NM")
                if nm is not None and str(nm) == args.annot_name:
                    yield pi, a
            elif subtype == "/Widget" and qualified_name(a) == args.field:
                if seen == (args.control or 0):
                    yield pi, a
                seen += 1


def rect_of(annot):
    r = [float(x) for x in annot.Rect]
    return min(r[0], r[2]), min(r[1], r[3]), max(r[0], r[2]), max(r[1], r[3])


def stored_text(annot):
    if str(annot.get("/Subtype", "")) == "/FreeText":
        c = annot.get("/Contents")
        return None if c is None else str(c)
    v = inherited(annot, "/V")
    return None if v is None else str(v)


def appearance_stream(annot):
    import pikepdf
    ap = annot.get("/AP")
    n = ap.get("/N") if ap is not None else None
    if n is None:
        return None
    if isinstance(n, pikepdf.Stream):
        return n
    state = annot.get("/AS")
    if state is not None and state in n:
        return n[state]
    return None


def analyse_appearance(stream, depth=0):
    """Counts what an appearance stream draws: text operators, font resources, images, path operators."""
    import pikepdf
    info = {"text_ops": 0, "fonts": [], "images": 0, "path_ops": 0, "unparseable": False}
    if depth > 8:
        return info
    resources = stream.get("/Resources")
    fonts = resources.get("/Font") if resources is not None else None
    if fonts is not None:
        for name, font in fonts.items():
            base = str(font.get("/BaseFont", name))
            desc = font.get("/FontDescriptor")
            if desc is None and "/DescendantFonts" in font:
                desc = font.DescendantFonts[0].get("/FontDescriptor")
            embedded = desc is not None and any(k in desc for k in ("/FontFile", "/FontFile2", "/FontFile3"))
            info["fonts"].append("%s (%s)" % (base.lstrip("/"), "embedded" if embedded else "NOT embedded"))
    try:
        ops = pikepdf.parse_content_stream(stream)
    except Exception:  # noqa: BLE001 - a damaged stream is reported, not raised
        info["unparseable"] = True
        return info
    xobjects = resources.get("/XObject") if resources is not None else None
    for operands, op in ops:
        o = str(op)
        if o in ("Tj", "TJ", "'", '"'):
            info["text_ops"] += 1
        elif o in ("m", "l", "c", "v", "y", "re", "h"):
            info["path_ops"] += 1
        elif o == "Do" and xobjects is not None and operands and operands[0] in xobjects:
            xo = xobjects[operands[0]]
            if str(xo.get("/Subtype", "")) == "/Image":
                info["images"] += 1
            elif str(xo.get("/Subtype", "")) == "/Form":
                sub = analyse_appearance(xo, depth + 1)
                for k in ("text_ops", "images", "path_ops"):
                    info[k] += sub[k]
                info["fonts"] += sub["fonts"]
                info["unparseable"] |= sub["unparseable"]
    return info


# ---------------------------------------------------------------------------------------------- readers
def page_geometry(pdf, page_index):
    page = pdf.pages[page_index]
    box = [float(x) for x in page.MediaBox]
    if int(page.get("/Rotate", 0)) % 360 != 0:
        raise ValueError("rotated pages are not supported by this checker")
    return box  # x0, y0, x1, y1


def crop_box(rect, box):
    l, b, r, t = rect
    return max(box[0], l - MARGIN_PT), max(box[1], b - MARGIN_PT), min(box[2], r + MARGIN_PT), min(box[3], t + MARGIN_PT)


def render_poppler(path, page_index, rect, box, tmp):
    exe = shutil.which("pdftoppm")
    if not exe:
        return None
    l, b, r, t = crop_box(rect, box)
    s = DPI / 72.0
    x, y = int((l - box[0]) * s), int((box[3] - t) * s)
    w, h = int(math.ceil((r - l) * s)), int(math.ceil((t - b) * s))
    prefix = os.path.join(tmp, "poppler")
    subprocess.run([exe, "-r", str(DPI), "-f", str(page_index + 1), "-l", str(page_index + 1), "-x", str(x), "-y", str(y),
                    "-W", str(w), "-H", str(h), "-png", "-singlefile", path, prefix], check=True, capture_output=True)
    from PIL import Image
    return Image.open(prefix + ".png").convert("L")


def render_mupdf(path, page_index, rect, box, tmp):
    try:
        import pymupdf
    except ImportError:
        return None
    l, b, r, t = crop_box(rect, box)
    doc = pymupdf.open(path)
    page = doc[page_index]
    height = page.rect.height
    clip = pymupdf.Rect(l - box[0], height - (t - box[1]), r - box[0], height - (b - box[1]))
    pix = page.get_pixmap(matrix=pymupdf.Matrix(DPI / 72.0, DPI / 72.0), clip=clip, alpha=False)
    from PIL import Image
    return Image.frombytes("RGB", (pix.width, pix.height), pix.samples).convert("L")


def render_pdfium(path, page_index, rect, box, tmp):
    try:
        import pypdfium2 as pdfium
    except ImportError:
        return None
    l, b, r, t = crop_box(rect, box)
    pdf = pdfium.PdfDocument(path)
    pdf.init_forms()
    page = pdf[page_index]
    bitmap = page.render(scale=DPI / 72.0, crop=(l - box[0], b - box[1], box[2] - r, box[3] - t), may_draw_forms=True)
    return bitmap.to_pil().convert("L")


READERS = {"poppler": render_poppler, "mupdf": render_mupdf, "pdfium": render_pdfium}


# ---------------------------------------------------------------------------------------------- image metrics
def ink_mask(img):
    import numpy as np
    a = np.asarray(img.convert("L"), dtype=np.int16)
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    return np.abs(a - int(np.median(border))) > INK_DELTA


def touches_edge(mask):
    return bool(mask[0].any() or mask[-1].any() or mask[:, 0].any() or mask[:, -1].any())


def trim(mask):
    import numpy as np
    ys, xs = np.where(mask)
    if len(ys) == 0:
        return None
    return mask[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def normalised(mask):
    import numpy as np
    from PIL import Image
    t = trim(mask)
    if t is None:
        return None
    im = Image.fromarray((t * 255).astype(np.uint8)).resize((NORM_W, NORM_H), Image.LANCZOS)
    return np.asarray(im) > 100


def coverage(a, b):
    from scipy import ndimage as ndi
    return float((a & ndi.binary_dilation(b, iterations=DILATE)).sum() / max(1, a.sum()))


def similarity(a, b):
    """How much of each picture's ink lies on the other's ink (the smaller of the two shares)."""
    return min(coverage(a, b), coverage(b, a))


def letter_run_count(mask):
    """Large connected ink blobs: the letter runs. Dots and marks are smaller than DOT_AREA of the text height squared."""
    import numpy as np
    from scipy import ndimage as ndi
    t = trim(mask)
    if t is None:
        return 0
    labels, n = ndi.label(t, structure=np.ones((3, 3)))
    if n == 0:
        return 0
    areas = ndi.sum(t, labels, range(1, n + 1))
    limit = DOT_AREA * t.shape[0] ** 2
    return int((areas >= limit).sum())


def render_reference(text, font_path, size=96):
    from PIL import Image, ImageDraw, ImageFont, features
    if not features.check("raqm"):
        return None
    font = ImageFont.truetype(font_path, size, layout_engine=ImageFont.Layout.RAQM)
    l, t, r, b = font.getbbox(text, direction="rtl", language="fa")
    im = Image.new("L", (r - l + 40, b - t + 40), 255)
    ImageDraw.Draw(im).text((20 - l, 20 - t), text, font=font, fill=0, direction="rtl", language="fa")
    return im


def candidate_fonts(text, requested):
    if requested and requested != ["auto"]:
        return requested
    out = subprocess.run(["fc-list", ":lang=fa", "file"], capture_output=True, text=True).stdout if shutil.which("fc-list") else ""
    paths = sorted({line.split(":")[0].strip() for line in out.splitlines() if line.strip()})
    paths = [p for p in paths if p.lower().endswith((".ttf", ".otf")) and "unifont" not in p.lower()]
    try:
        from fontTools.ttLib import TTFont
        wanted = {ord(c) for c in text if not c.isspace() and c != ZWNJ}
        keep = []
        for p in paths:
            try:
                cmap = TTFont(p, fontNumber=0, lazy=True).getBestCmap()
            except Exception:  # noqa: BLE001
                continue
            if wanted <= set(cmap):
                keep.append(p)
        paths = keep
    except ImportError:
        pass
    return paths[:12]


# ---------------------------------------------------------------------------------------------- the checks
def check_target(path, pdf, page_index, annot, args, res, label):
    box = page_geometry(pdf, page_index)
    rect = rect_of(annot)
    expect = args.expect if args.expect is not None else (stored_text(annot) if args.all_freetext else None)
    print("--- %s (page %d, rect %.1f %.1f %.1f %.1f)" % (label, page_index + 1, *rect))

    # value
    got = stored_text(annot)
    if expect is None:
        res.add("SKIP", "value", "no --expect given")
    elif got == expect:
        res.add("PASS", "value", "stored text equals the expected text (%d characters)" % len(expect))
    else:
        res.add("FAIL", "value", "stored %r, expected %r" % (None if got is None else got[:60], expect[:60]))

    # portable
    stream = appearance_stream(annot)
    if stream is None:
        res.add("FAIL", "portable", "no stored appearance (/AP /N): every reader will draw its own")
        return
    info = analyse_appearance(stream)
    if info["unparseable"]:
        res.add("FAIL", "portable", "the appearance stream cannot be parsed")
    elif (info["text_ops"] or info["fonts"]) and not args.allow_fonts:
        res.add("FAIL", "portable", "the appearance is text in a font, not outlines: %d text operator(s), fonts %s"
                % (info["text_ops"], info["fonts"] or "none declared"))
    elif info["path_ops"] == 0 and info["images"] == 0:
        res.add("FAIL", "portable", "the appearance draws no shapes at all")
    else:
        res.add("PASS", "portable", "no fonts, no text operators (%d path operators, %d image(s))" % (info["path_ops"], info["images"]))

    # render
    masks, tmp = {}, tempfile.mkdtemp(prefix="persian_check_")
    try:
        for name, fn in READERS.items():
            if args.readers and name not in args.readers:
                continue
            try:
                img = fn(path, page_index, rect, box, tmp)
            except Exception as e:  # noqa: BLE001 - a reader that fails is reported, not fatal
                res.add("WARN", "render", "%s could not render it: %s" % (name, str(e)[:100]))
                continue
            if img is None:
                res.add("SKIP", "render", "%s is not installed" % name)
                continue
            m = ink_mask(img)
            share = float(m.mean())
            if share < 0.002:
                res.add("FAIL", "render", "%s: the appearance is blank (ink on %.3f%% of the area)" % (name, share * 100))
            elif touches_edge(m):
                res.add("WARN", "render", "%s: ink reaches the edge of the field, it may be clipped" % name)
                masks[name] = m
            else:
                res.add("PASS", "render", "%s: ink on %.1f%% of the area" % (name, share * 100))
                masks[name] = m
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    norm = {n: normalised(m) for n, m in masks.items()}
    norm = {n: v for n, v in norm.items() if v is not None}

    # agree
    names = sorted(norm)
    if len(names) < 2:
        res.add("SKIP", "agree", "needs at least two readers (have: %s)" % (", ".join(names) or "none"))
    else:
        worst = (2.0, "")
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                s = similarity(norm[names[i]], norm[names[j]])
                worst = min(worst, (s, "%s/%s" % (names[i], names[j])))
        status = "PASS" if worst[0] >= MIN_AGREE else "FAIL"
        res.add(status, "agree", "lowest pairwise match %.3f (%s), needs %.2f" % (worst[0], worst[1], MIN_AGREE))

    # joined and reference need single-line text
    if expect is None or "\n" in expect or "\r" in expect:
        res.add("SKIP", "joined", "needs single-line --expect text")
        res.add("SKIP", "reference", "needs single-line --expect text")
        return
    runs = expected_runs(expect)
    if runs is None:
        res.add("SKIP", "joined", "the text has digits, punctuation or non-Arabic letters, so the run count depends on the font")
    elif not masks:
        res.add("SKIP", "joined", "nothing was rendered")
    else:
        extra = allowed_extra_blobs(expect)
        for name in sorted(masks):
            got_runs = letter_run_count(masks[name])
            high = runs + extra + args.run_tolerance
            low = runs - args.run_tolerance
            note = " (+%d allowed for letters drawn in two pieces)" % extra if extra else ""
            if low <= got_runs <= high:
                res.add("PASS", "joined", "%s: %d letter runs, the script requires %d%s" % (name, got_runs, runs, note))
            elif got_runs > high:
                res.add("FAIL", "joined", "%s: %d letter runs but the script requires %d%s: the letters are not joined" % (name, got_runs, runs, note))
            else:
                res.add("FAIL", "joined", "%s: only %d letter runs, the script requires %d: letters are missing or merged" % (name, got_runs, runs))

    if args.no_reference:
        res.add("SKIP", "reference", "--no-reference")
        return
    fonts = candidate_fonts(expect, args.font)
    if not fonts:
        res.add("SKIP", "reference", "no installed font has all the letters (give one with --font PATH)")
        return
    # All readers draw the same picture when "agree" passed, so one of them represents the others here.
    rep = "poppler" if "poppler" in norm else (sorted(norm)[0] if norm else None)
    if rep is None:
        res.add("SKIP", "reference", "nothing was rendered")
        return
    best = None
    for fp in fonts:
        ref = render_reference(expect, fp)
        if ref is None:
            res.add("SKIP", "reference", "Pillow has no raqm (HarfBuzz/FriBiDi) support")
            return
        rm = normalised(ink_mask(ref))
        if rm is None:
            continue
        s = similarity(norm[rep], rm)
        if best is None or s > best[0]:
            best = (s, os.path.basename(fp))
    if best is None:
        res.add("SKIP", "reference", "nothing to compare")
    else:
        status = "PASS" if best[0] >= args.min_reference else "FAIL"
        res.add(status, "reference", "best match %.3f (%s against %s), needs %.2f%s" % (
            best[0], rep, best[1], args.min_reference,
            "" if status == "PASS" else ": a letter is missing, the order is wrong, or this font is not the one used"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pdf")
    sel = ap.add_mutually_exclusive_group(required=True)
    sel.add_argument("--field", help="fully qualified field name of a widget")
    sel.add_argument("--annot-name", help="/NM of a FreeText annotation")
    sel.add_argument("--all-freetext", action="store_true", help="every FreeText annotation (expects its /Contents)")
    ap.add_argument("--expect", help="the text the field must hold and show")
    ap.add_argument("--page", type=int, default=None, help="0-based page index (default: all pages)")
    ap.add_argument("--control", type=int, default=0, help="which widget of the field (default 0)")
    ap.add_argument("--font", action="append", help="reference font file; repeat, or 'auto' (default) to try the installed ones")
    ap.add_argument("--readers", help="comma separated subset of: poppler,mupdf,pdfium")
    ap.add_argument("--allow-fonts", action="store_true", help="do not fail text appearances that use fonts")
    ap.add_argument("--no-reference", action="store_true", help="skip the comparison with an independent shaping")
    ap.add_argument("--run-tolerance", type=int, default=0, help="allowed difference in the number of letter runs")
    ap.add_argument("--min-reference", type=float, default=MIN_REFERENCE)
    ap.add_argument("--json", action="store_true", help="print a JSON summary at the end")
    args = ap.parse_args(argv)
    args.readers = [r.strip() for r in args.readers.split(",")] if args.readers else None
    if args.font is None:
        args.font = ["auto"]
    if args.field is not None and args.expect is None:
        ap.error("--field needs --expect")
    try:
        import numpy  # noqa: F401
        import pikepdf  # noqa: F401
        import PIL  # noqa: F401
        import scipy  # noqa: F401
    except ImportError as e:
        print("missing dependency: %s (needs pikepdf, numpy, scipy, Pillow)" % e, file=sys.stderr)
        return EXIT_USAGE

    pdf = load_pdf(args.pdf)
    targets = list(find_targets(pdf, args))
    if not targets:
        print("no matching annotation found", file=sys.stderr)
        return EXIT_USAGE
    res = Result()
    for pi, annot in targets:
        label = args.field or (str(annot.get("/NM")) if annot.get("/NM") is not None else "FreeText")
        try:
            check_target(args.pdf, pdf, pi, annot, args, res, label)
        except ValueError as e:
            print("cannot check %s: %s" % (label, e), file=sys.stderr)
            return EXIT_USAGE
    print("=== %s: %d failed, %d passed, %d warnings, %d skipped" % (
        "FAILED" if res.failed else "OK", sum(r["status"] == "FAIL" for r in res.rows), sum(r["status"] == "PASS" for r in res.rows),
        sum(r["status"] == "WARN" for r in res.rows), sum(r["status"] == "SKIP" for r in res.rows)))
    if args.json:
        print(json.dumps(res.rows, ensure_ascii=False))
    return EXIT_FAIL if res.failed else EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
