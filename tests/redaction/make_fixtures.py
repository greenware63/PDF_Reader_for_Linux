#!/usr/bin/env python3
"""Deterministic generator for the redaction adversarial fixtures.

Writes tests/fixtures/redaction/*.pdf and manifest.txt.  The fixtures are COMMITTED; rerun this
script only when you change them.  Every secret is a "CANARY-..." string so the tests can look for
it in decompressed output, but the real assertions are structural / pixel based (a string search
alone is not accepted as proof, see docs/REDACTION.md).

Coordinates in the manifest are *displayed-page points, origin top-left* (the engine's rect space).
"""
import os, sys, zlib, base64, subprocess, shutil, hashlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures", "redaction")
os.makedirs(OUT, exist_ok=True)


class Pdf:
    def __init__(self):
        self.objs = {}
        self.n = 0

    def reserve(self):
        self.n += 1
        return self.n

    def add(self, body, num=None):
        if num is None:
            num = self.reserve()
        if isinstance(body, str):
            body = body.encode("latin-1")
        self.objs[num] = body
        return num

    def stream(self, d, data, compress=False, num=None, extra_filters=None):
        if isinstance(data, str):
            data = data.encode("latin-1")
        filt = ""
        if compress:
            data = zlib.compress(data, 9)
            filt = " /Filter /FlateDecode"
        body = ("<< %s /Length %d%s >>\nstream\n" % (d, len(data), filt)).encode("latin-1") + data + b"\nendstream"
        return self.add(body, num)

    def serialize(self, root, info=None, trailer_extra="", header=b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n", prev=None, base=b"", only=None):
        out = bytearray(base if base else header)
        offs = {}
        nums = sorted(self.objs) if only is None else sorted(only)
        for k in nums:
            offs[k] = len(out)
            out += b"%d 0 obj\n" % k + self.objs[k] + b"\nendobj\n"
        xref_pos = len(out)
        size = max(self.objs) + 1
        out += b"xref\n"
        if prev is None:
            out += b"0 %d\n" % size
            out += b"0000000000 65535 f \n"
            for k in range(1, size):
                if k in offs:
                    out += b"%010d 00000 n \n" % offs[k]
                else:
                    out += b"0000000000 65535 f \n"
        else:
            out += b"0 1\n0000000000 65535 f \n"
            for k in nums:
                out += b"%d 1\n%010d 00000 n \n" % (k, offs[k])
        tr = "/Size %d /Root %d 0 R" % (size, root)
        if info:
            tr += " /Info %d 0 R" % info
        if prev is not None:
            tr += " /Prev %d" % prev
        tr += " " + trailer_extra
        out += ("trailer\n<< %s >>\nstartxref\n%d\n%%%%EOF\n" % (tr, xref_pos)).encode("latin-1")
        return bytes(out), xref_pos


def txt(x, y, s, size=14, font="F1", extra=""):
    return "BT %s /%s %d Tf %s %s Td (%s) Tj ET\n" % (extra, font, size, x, y, s)


def basic_doc(pdf, content_pages, extra_page_entries=None, resources_extra="", catalog_extra="", media="[0 0 612 792]", with_font=True):
    """content_pages: list of content-stream strings. Returns (catalog_num, page_nums, pages_num)."""
    font = pdf.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    pages_num = pdf.reserve()
    page_nums = []
    for i, c in enumerate(content_pages):
        cs = pdf.stream("", c, compress=True)
        extra = (extra_page_entries[i] if extra_page_entries else "")
        p = pdf.add("<< /Type /Page /Parent %d 0 R /MediaBox %s /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> %s >> %s >>"
                    % (pages_num, media, cs, font, resources_extra, extra))
        page_nums.append(p)
    pdf.add("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join("%d 0 R" % p for p in page_nums), len(page_nums)), pages_num)
    cat = pdf.add("<< /Type /Catalog /Pages %d 0 R %s >>" % (pages_num, catalog_extra))
    return cat, page_nums, pages_num


manifest = []


def write(name, data, **kw):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    lines = ["fixture %s" % name]
    lines.append("password %s" % kw.get("password", "-"))
    lines.append("expect_status %s" % kw.get("status", "Ok"))
    for c in kw.get("canaries", []):
        lines.append("canary %s" % c)
    for page, l, t, r, b in kw.get("rects", []):
        lines.append("rect %d %s %s %s %s" % (page, l, t, r, b))
    if "sigs" in kw:
        lines.append("expect_sigs %d" % kw["sigs"])
    if "atts" in kw:
        lines.append("expect_attachments %d" % kw["atts"])
    for page, l, t, r, b in kw.get("ink", []):
        lines.append("ink %d %s %s %s %s" % (page, l, t, r, b))
    for page, l, t, r, b in kw.get("blank", []):
        lines.append("blank %d %s %s %s %s" % (page, l, t, r, b))
    for page, R, G, B, tol in kw.get("color_absent", []):
        lines.append("color_absent %d %d %d %d %d" % (page, R, G, B, tol))
    for page, R, G, B, tol, mn in kw.get("color_present", []):
        lines.append("color_present %d %d %d %d %d %d" % (page, R, G, B, tol, mn))
    if kw.get("safe_only"):
        lines.append("safe_only 1")
    if kw.get("compare_poppler", True) and kw.get("status", "Ok") == "Ok":
        lines.append("compare_poppler 1")
    manifest.append("\n".join(lines))


H = 792  # page height used by the simple fixtures


def top_rect(l, baseline, r, size=14, pad=4):
    """Displayed-space rect covering a text line whose PDF baseline is `baseline`."""
    return (l, round(H - baseline - size * 0.9 - pad, 1), r, round(H - baseline + size * 0.3 + pad, 1))


# ------------------------------------------------------------------ 1. basic
def f_basic():
    p = Pdf()
    c1 = txt(72, 700, "Public line one KEEP-ONE") + txt(72, 650, "Account TOPSECRET-A1 number")
    c2 = txt(72, 700, "Page two keeps this text visible")
    cat, pages, _ = basic_doc(p, [c1, c2])
    data, _ = p.serialize(cat)
    l, t, r, b = top_rect(60, 650, 400)
    write("basic.pdf", data, canaries=["TOPSECRET-A1"], rects=[(0, l, t, r, b)],
          ink=[(0, 60, 80, 400, 100), (1, 60, 80, 400, 100)], sigs=0, atts=0)
    return data


# ------------------------------------------------------------------ 2. hidden content
def f_hidden():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    ocg = p.add("<< /Type /OCG /Name (CANARY-OCG-NAME) >>")
    form = p.stream("/Type /XObject /Subtype /Form /BBox [0 0 300 40] /Resources << /Font << /F1 %d 0 R >> >>" % font,
                    txt(0, 10, "CANARY-XOBJ visible form text"), compress=True)
    content = (
        txt(72, 720, "Visible text KEEP-VISIBLE") +
        txt(72, 650, "CANARY-VISIBLE line under the box") +
        "/OC /oc1 BDC\n" + txt(72, 600, "CANARY-OCG hidden layer") + "EMC\n" +
        "q 1 1 1 rg\n" + txt(72, 560, "CANARY-WHITE white on white", extra="1 g") + "Q\n" +
        txt(72, 520, "CANARY-INVIS invisible render mode", extra="3 Tr") +
        txt(2000, 400, "CANARY-OFFPAGE far outside") +
        "q 0 0 10 10 re W n\n" + txt(72, 300, "CANARY-CLIP clipped away") + "Q\n" +
        "q 1 0 0 1 72 450 cm /Fm1 Do Q\n"
    )
    cs = p.stream("", content, compress=True)
    pages = p.reserve()
    page = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R "
                 "/Resources << /Font << /F1 %d 0 R >> /Properties << /oc1 %d 0 R >> /XObject << /Fm1 %d 0 R >> >> >>"
                 % (pages, cs, font, ocg, form))
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % page, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R /OCProperties << /OCGs [%d 0 R] /D << /OFF [%d 0 R] /Order [%d 0 R] >> >> >>"
                % (pages, ocg, ocg, ocg))
    data, _ = p.serialize(cat)
    rects = [(0,) + top_rect(60, 650, 400), (0,) + top_rect(60, 450, 380, size=14, pad=14)]
    write("hidden_content.pdf", data,
          canaries=["CANARY-VISIBLE", "CANARY-OCG", "CANARY-WHITE", "CANARY-INVIS", "CANARY-OFFPAGE", "CANARY-CLIP", "CANARY-XOBJ"],
          rects=rects, ink=[(0, 60, 60, 400, 90)], sigs=0)
    return data


# ------------------------------------------------------------------ 3. annotations + forms
def f_annots_forms():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    ap_free = p.stream("/Type /XObject /Subtype /Form /BBox [0 0 300 30] /Resources << /Font << /F1 %d 0 R >> >>" % font,
                       txt(2, 8, "CANARY-ANNOT-VISIBLE freetext"), compress=False)
    ap_keep = p.stream("/Type /XObject /Subtype /Form /BBox [0 0 200 30] /Resources << /Font << /F1 %d 0 R >> >>" % font,
                       "0 0 1 rg 0 0 200 30 re f\n", compress=False)
    ap_field = p.stream("/Type /XObject /Subtype /Form /BBox [0 0 250 24] /Resources << /Font << /F1 %d 0 R >> >>" % font,
                        "/Tx BMC q BT /F1 12 Tf 2 6 Td (CANARY-FORM-VALUE) Tj ET Q EMC", compress=False)
    ap_field2 = p.stream("/Type /XObject /Subtype /Form /BBox [0 0 250 24] /Resources << /Font << /F1 %d 0 R >> >>" % font,
                         "0 0.6 0 rg 0 0 250 24 re f\n", compress=False)
    pg = p.reserve()
    pages = p.reserve()
    a_free = p.add("<< /Type /Annot /Subtype /FreeText /Rect [72 600 372 630] /Contents (CANARY-ANNOT-CONTENTS) /T (CANARY-AUTHOR) "
                   "/DA (/Helv 12 Tf 0 g) /AP << /N %d 0 R >> /F 4 /P %d 0 R >>" % (ap_free, pg))
    a_keep = p.add("<< /Type /Annot /Subtype /Square /Rect [72 300 272 330] /Contents (visible blue box) /AP << /N %d 0 R >> /F 4 /P %d 0 R >>" % (ap_keep, pg))
    popup = p.reserve()
    a_note = p.add("<< /Type /Annot /Subtype /Text /Rect [500 700 520 720] /Contents (CANARY-NOTE) /T (CANARY-NOTE-AUTHOR) /Popup %d 0 R /F 4 /P %d 0 R >>" % (popup, pg))
    p.add("<< /Type /Annot /Subtype /Popup /Rect [400 500 500 600] /Parent %d 0 R /Contents (CANARY-POPUP) >>" % a_note, popup)
    a_link = p.add("<< /Type /Annot /Subtype /Link /Rect [72 200 200 220] /A << /S /URI /URI (https://canary-uri.example/CANARY-URI) >> /P %d 0 R >>" % pg)
    a_hidden = p.add("<< /Type /Annot /Subtype /FreeText /Rect [72 100 300 130] /Contents (CANARY-HIDDEN-ANNOT) /DA (/Helv 12 Tf 0 g) /AP << /N %d 0 R >> /F 2 /P %d 0 R >>" % (ap_free, pg))
    f_sec = p.add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (secretField) /V (CANARY-FORM-VALUE) /DV (CANARY-FORM-DEFAULT) /Rect [72 500 322 524] "
                  "/DA (/Helv 12 Tf 0 g) /F 4 /AP << /N %d 0 R >> /P %d 0 R >>" % (ap_field, pg))
    f_pub = p.add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (publicField) /V (public) /Rect [72 400 322 424] "
                  "/DA (/Helv 12 Tf 0 g) /F 4 /AP << /N %d 0 R >> /P %d 0 R >>" % (ap_field2, pg))
    cs = p.stream("", txt(72, 740, "Page with annotations KEEP-ANN"), compress=True)
    p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> "
          "/Annots [%s] >>" % (pages, cs, font, " ".join("%d 0 R" % x for x in (a_free, a_keep, a_note, popup, a_link, a_hidden, f_sec, f_pub))), pg)
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R /AcroForm << /Fields [%d 0 R %d 0 R] /DA (/Helv 12 Tf 0 g) /NeedAppearances false "
                "/DR << /Font << /Helv %d 0 R >> >> >> >>" % (pages, f_sec, f_pub, font))
    data, _ = p.serialize(cat)
    # displayed rects: freetext (72,600)-(372,630) and field (72,500)-(322,524) -> top-left coordinates
    rects = [(0, 70, H - 632, 374, H - 598), (0, 70, H - 526, 324, H - 498)]
    write("annots_forms.pdf", data,
          canaries=["CANARY-ANNOT-VISIBLE", "CANARY-ANNOT-CONTENTS", "CANARY-AUTHOR", "CANARY-NOTE", "CANARY-POPUP", "CANARY-URI",
                    "CANARY-HIDDEN-ANNOT", "CANARY-FORM-VALUE", "CANARY-FORM-DEFAULT"],
          rects=rects, ink=[(0, 72, H - 330, 272, H - 300)], atts=0, sigs=0,
          color_present=[(0, 0, 153, 0, 30, 3000)])  # widget appearance of the non-redacted field must be burned in
    return data


# ------------------------------------------------------------------ 4. attachments
def f_attachments():
    p = Pdf()
    ef = p.stream("/Type /EmbeddedFile /Subtype /text#2Fplain", "CANARY-ATTACH file body " * 20, compress=True)
    ef2 = p.stream("/Type /EmbeddedFile", "CANARY-ATTACH-ANNOT body " * 20, compress=False)
    fs = p.add("<< /Type /Filespec /F (CANARY-FILENAME.txt) /UF (CANARY-FILENAME.txt) /EF << /F %d 0 R /UF %d 0 R >> /Desc (CANARY-ATTACH-DESC) >>" % (ef, ef))
    fs2 = p.add("<< /Type /Filespec /F (CANARY-FILENAME2.txt) /EF << /F %d 0 R >> >>" % ef2)
    pg = p.reserve(); pages = p.reserve()
    annot = p.add("<< /Type /Annot /Subtype /FileAttachment /Rect [500 700 520 720] /FS %d 0 R /Name /PushPin /Contents (CANARY-ATTACH-ANNOT-TEXT) /F 4 /P %d 0 R >>" % (fs2, pg))
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    cs = p.stream("", txt(72, 700, "Document with attachments KEEP-ATT") + txt(72, 650, "CANARY-ATTACH-VISIBLE"), compress=True)
    p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> /Annots [%d 0 R] >>"
          % (pages, cs, font, annot), pg)
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R /Names << /EmbeddedFiles << /Names [(CANARY-FILENAME.txt) %d 0 R] >> >> /AF [%d 0 R] >>" % (pages, fs, fs))
    data, _ = p.serialize(cat)
    write("attachments.pdf", data,
          canaries=["CANARY-ATTACH", "CANARY-FILENAME", "CANARY-ATTACH-DESC", "CANARY-ATTACH-ANNOT", "CANARY-ATTACH-VISIBLE"],
          rects=[(0,) + top_rect(60, 650, 300)], ink=[(0, 60, 60, 400, 90)], atts=1, sigs=0)  # PDFium counts only the name-tree attachment, not the FileAttachment annotation
    return data


# ------------------------------------------------------------------ 5. metadata and document-level structures
def f_metadata():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    xmp = p.stream("/Type /Metadata /Subtype /XML",
                   "<?xpacket begin='x'?><x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
                   "<rdf:Description xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:creator>CANARY-XMP-CREATOR</dc:creator></rdf:Description>"
                   "</rdf:RDF></x:xmpmeta><?xpacket end='w'?>", compress=False)
    xmp2 = p.stream("/Type /Metadata /Subtype /XML", "<x:xmpmeta>CANARY-XMP-PAGE</x:xmpmeta>", compress=True)
    thumb = p.stream("/Type /XObject /Subtype /Image /Width 4 /Height 4 /ColorSpace /DeviceRGB /BitsPerComponent 8", bytes([250, 3, 7] * 16), compress=True)
    pg = p.reserve(); pages = p.reserve()
    page_extra = "/PieceInfo << /CANARYApp << /LastModified (D:20200101000000) /Private (CANARY-PIECEINFO) >> >> /Metadata %d 0 R /Thumb %d 0 R /StructParents 0" % (xmp2, thumb)
    cs = p.stream("", txt(72, 700, "Metadata fixture KEEP-META") + txt(72, 650, "CANARY-META-VISIBLE"), compress=True)
    p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> %s >>" % (pages, cs, font, page_extra), pg)
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    o2 = p.reserve()
    o1 = p.add("<< /Title (CANARY-OUTLINE-TITLE) /Parent %d 0 R /Dest [%d 0 R /Fit] /First %d 0 R /Last %d 0 R /Count 1 >>" % (0, pg, o2, o2))
    outlines = p.add("<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count 2 >>" % (o1, o1))
    p.objs[o1] = ("<< /Title (CANARY-OUTLINE-TITLE) /Parent %d 0 R /Dest [%d 0 R /Fit] /First %d 0 R /Last %d 0 R /Count 1 >>" % (outlines, pg, o2, o2)).encode()
    p.add("<< /Title (CANARY-OUTLINE-CHILD) /Parent %d 0 R /Dest [%d 0 R /Fit] >>" % (o1, pg), o2)
    st_elem = p.add("<< /Type /StructElem /S /P /P %d 0 R /Pg %d 0 R /ActualText (CANARY-ACTUALTEXT) /Alt (CANARY-ALT) /K 0 >>" % (0, pg))
    st_root = p.add("<< /Type /StructTreeRoot /K %d 0 R /ParentTree << /Nums [0 [%d 0 R]] >> >>" % (st_elem, st_elem))
    p.objs[st_elem] = ("<< /Type /StructElem /S /P /P %d 0 R /Pg %d 0 R /ActualText (CANARY-ACTUALTEXT) /Alt (CANARY-ALT) /K 0 >>" % (st_root, pg)).encode()
    js = p.add("<< /S /JavaScript /JS (app.alert\\('CANARY-JS'\\);) >>")
    info = p.add("<< /Title (CANARY-INFO-TITLE) /Author (CANARY-INFO-AUTHOR) /Subject (CANARY-INFO-SUBJECT) /Keywords (CANARY-INFO-KEYWORDS) "
                 "/Creator (CANARY-INFO-CREATOR) /Producer (CANARY-INFO-PRODUCER) /CANARY-INFO-CUSTOM (CANARY-INFO-CUSTOM-VALUE) >>")
    cat = p.add("<< /Type /Catalog /Pages %d 0 R /Outlines %d 0 R /Metadata %d 0 R /OpenAction %d 0 R /Lang (CANARY-LANG) "
                "/PageLabels << /Nums [0 << /S /D /P (CANARY-LABEL-) >>] >> /Dests << /CANARYDEST [%d 0 R /Fit] >> "
                "/Names << /Dests << /Names [(CANARY-NAMED-DEST) [%d 0 R /Fit]] >> >> /StructTreeRoot %d 0 R /MarkInfo << /Marked true >> "
                "/ViewerPreferences << /DisplayDocTitle true >> /CANARY-CATALOG-KEY (CANARY-CATALOG-VALUE) >>"
                % (pages, outlines, xmp, js, pg, pg, st_root))
    data, _ = p.serialize(cat, info=info)
    names = ["CANARY-META-VISIBLE", "CANARY-OUTLINE-TITLE", "CANARY-OUTLINE-CHILD", "CANARY-XMP-CREATOR", "CANARY-XMP-PAGE", "CANARY-PIECEINFO",
             "CANARY-ACTUALTEXT", "CANARY-ALT", "CANARY-JS", "CANARY-INFO-TITLE", "CANARY-INFO-AUTHOR", "CANARY-INFO-SUBJECT",
             "CANARY-INFO-KEYWORDS", "CANARY-INFO-CREATOR", "CANARY-INFO-PRODUCER", "CANARY-INFO-CUSTOM", "CANARY-LANG", "CANARY-LABEL",
             "CANARY-NAMED-DEST", "CANARYDEST", "CANARY-CATALOG-KEY", "CANARY-CATALOG-VALUE", "CANARYApp"]
    write("metadata_all.pdf", data, canaries=names, rects=[(0,) + top_rect(60, 650, 300)], ink=[(0, 60, 60, 400, 90)], atts=0, sigs=0)
    # same document with object streams + compression
    if shutil.which("qpdf"):
        tmp = os.path.join(OUT, "_tmp_in.pdf")
        open(tmp, "wb").write(data)
        out = os.path.join(OUT, "metadata_objstm.pdf")
        subprocess.run(["qpdf", "--object-streams=generate", "--compress-streams=y", "--recompress-flate", "--deterministic-id", tmp, out], check=True)
        os.remove(tmp)
        # qpdf keeps CANARY names inside compressed object streams: raw grep must NOT find them (that is the point)
        raw = open(out, "rb").read()
        assert b"CANARY-OUTLINE-TITLE" not in raw, "objstm fixture should hide canaries from raw search"
        write("metadata_objstm.pdf", raw, canaries=names, rects=[(0,) + top_rect(60, 650, 300)], ink=[(0, 60, 60, 400, 90)], atts=0, sigs=0)
    return data


# ------------------------------------------------------------------ 6. incremental revisions
def f_incremental():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    cs_old = p.stream("", txt(72, 700, "CANARY-OLDREV original confidential text") + txt(72, 650, "Second line CANARY-OLDREV-2"), compress=False)
    pg = p.reserve(); pages = p.reserve()
    p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, cs_old, font), pg)
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    rev1, xref1 = p.serialize(cat)
    # revision 2: page now points to a new content stream; the old stream becomes unreferenced but stays in the file
    p2 = Pdf(); p2.n = p.n
    cs_new = p2.stream("", txt(72, 700, "Replacement text KEEP-NEW") + txt(72, 650, "Visible line under the box CANARY-NEWREV"), compress=False)
    p2.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, cs_new, font), pg)
    orphan = p2.add("<< /Orphan (CANARY-ORPHAN) >>")
    rev2, xref2 = p2.serialize(cat, prev=xref1, base=rev1, only=[cs_new, pg, orphan])
    # revision 3: delete-ish (free the orphan) while the bytes remain
    write("incremental.pdf", rev2, canaries=["CANARY-OLDREV", "CANARY-OLDREV-2", "CANARY-ORPHAN", "CANARY-NEWREV"],
          rects=[(0,) + top_rect(60, 650, 400)], ink=[(0, 60, 60, 400, 90)], atts=0, sigs=0)
    return rev2


# ------------------------------------------------------------------ 7. multi-filter and odd encodings
def f_multifilter():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    raw = (txt(72, 700, "Multi filter KEEP-MF") + txt(72, 650, "CANARY-MULTI secret inside layered filters")).encode()
    hexed = (zlib.compress(raw).hex() + ">").encode()
    cs = p.add(b"<< /Length %d /Filter [/ASCIIHexDecode /FlateDecode] >>\nstream\n" % len(hexed) + hexed + b"\nendstream")
    a85 = base64.a85encode(zlib.compress(raw)) + b"~>"
    cs2 = p.add(b"<< /Length %d /Filter [/ASCII85Decode /FlateDecode] >>\nstream\n" % len(a85) + a85 + b"\nendstream")
    pages = p.reserve()
    pg1 = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, cs, font))
    pg2 = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents [%d 0 R] /Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, cs2, font))
    p.add("<< /Type /Pages /Kids [%d 0 R %d 0 R] /Count 2 >>" % (pg1, pg2), pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    data, _ = p.serialize(cat)
    rr = top_rect(60, 650, 420)
    write("multifilter.pdf", data, canaries=["CANARY-MULTI"], rects=[(0,) + rr, (1,) + rr], ink=[(0, 60, 60, 400, 90), (1, 60, 60, 400, 90)], atts=0, sigs=0)
    return data


# ------------------------------------------------------------------ 8. rotation / cropbox geometry
def f_geometry():
    """Each page: a RED square (to be redacted) and a BLUE square (must survive)."""
    p = Pdf()
    pages = p.reserve()
    kids = []
    spec = []  # (rotate, cropbox or None, red_xy, blue_xy)

    def mk(rot, crop, red, blue):
        content = "1 0 0 rg %d %d 40 40 re f\n0 0 1 rg %d %d 40 40 re f\n" % (red + blue)
        cs = p.stream("", content, compress=True)
        extra = ("/Rotate %d " % rot if rot else "") + ("/CropBox [%s] " % " ".join(map(str, crop)) if crop else "")
        pg = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << >> %s>>" % (pages, cs, extra))
        kids.append(pg)

    def disp(rot, crop, x, y, w=612, h=792):
        # returns displayed (left, top) of the user-space point (x,y) top-left corner of a 40x40 square => square spans x..x+40, y..y+40
        # compute the displayed bbox of the square
        def pt(px, py):
            if crop:
                cx0, cy0, cx1, cy1 = crop
                px, py, w_, h_ = px - cx0, py - cy0, cx1 - cx0, cy1 - cy0
            else:
                w_, h_ = w, h
            if rot == 0:
                return px, h_ - py
            if rot == 90:
                return py, px
            if rot == 180:
                return w_ - px, py
            if rot == 270:
                return h_ - py, w_ - px
        xs, ys = zip(*[pt(x + dx, y + dy) for dx in (0, 40) for dy in (0, 40)])
        return min(xs), min(ys), max(xs), max(ys)

    cases = [(0, None, (100, 600), (300, 600)), (90, None, (100, 600), (300, 600)), (180, None, (100, 600), (300, 600)),
             (270, None, (100, 600), (300, 600)), (0, [50, 100, 550, 700], (100, 600), (300, 300)),
             (90, [50, 100, 550, 700], (100, 600), (300, 300))]
    rects, colors = [], []
    for i, (rot, crop, red, blue) in enumerate(cases):
        mk(rot, crop, red, blue)
        l, t, r, b = disp(rot, crop, *red)
        rects.append((i, l - 2, t - 2, r + 2, b + 2))
        colors.append((i, 255, 0, 0, 90))
    p.add("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join("%d 0 R" % k for k in kids), len(kids)), pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    data, _ = p.serialize(cat)
    present = [(i, 0, 0, 255, 90, 700) for i in range(len(cases))]  # >=700px of blue left on every page (40pt@150dpi ~ 83x83=6900 px)
    write("geometry.pdf", data, rects=rects, color_absent=colors, color_present=present, atts=0, sigs=0)
    return data


# ------------------------------------------------------------------ 9. images / smask
def f_images():
    p = Pdf()
    sec = p.stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask %d 0 R" % (p.n + 2),
                   bytes([250, 3, 7] * 64), compress=True)
    smask = p.stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /BitsPerComponent 8", bytes([255] * 64), compress=True)
    keep = p.stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceRGB /BitsPerComponent 8", bytes([3, 250, 7] * 64), compress=True)
    pages = p.reserve()
    cs = p.stream("", "q 100 0 0 100 100 600 cm /Im1 Do Q\nq 100 0 0 100 300 600 cm /Im2 Do Q\n", compress=True)
    pg = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /XObject << /Im1 %d 0 R /Im2 %d 0 R >> >> >>" % (pages, cs, sec, keep))
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    data, _ = p.serialize(cat)
    write("images.pdf", data, rects=[(0, 96, H - 704, 204, H - 596)], color_absent=[(0, 250, 3, 7, 40)],
          color_present=[(0, 3, 250, 7, 40, 5000)], atts=0, sigs=0)
    return data


# ------------------------------------------------------------------ 10. signature (structure only, NOT cryptographically valid)
def f_signed():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    pg = p.reserve(); pages = p.reserve()
    sigv = p.add("<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 100 200 300] /Contents <%s> "
                 "/Reason (CANARY-SIGREASON) /Name (CANARY-SIGNER) /M (D:20240101000000Z) >>" % ("00" * 64))
    sigf = p.add("<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) /V %d 0 R /Rect [72 100 272 140] /F 4 /P %d 0 R >>" % (sigv, pg))
    cs = p.stream("", txt(72, 700, "Signed contract KEEP-SIGNED") + txt(72, 650, "CANARY-SIGNED-VISIBLE amount"), compress=True)
    p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> /Annots [%d 0 R] >>"
          % (pages, cs, font, sigf), pg)
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R /AcroForm << /Fields [%d 0 R] /SigFlags 3 >> "
                "/Perms << /DocMDP %d 0 R >> >>" % (pages, sigf, sigv))
    data, _ = p.serialize(cat)
    write("signed.pdf", data, canaries=["CANARY-SIGREASON", "CANARY-SIGNER", "CANARY-SIGNED-VISIBLE"],
          rects=[(0,) + top_rect(60, 650, 400)], ink=[(0, 60, 60, 400, 90)], sigs=1, atts=0)
    return data


# ------------------------------------------------------------------ 11. many pages
def f_multipage():
    p = Pdf()
    font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    pages = p.reserve(); kids = []
    for i in range(30):
        cs = p.stream("", "q 0 0 1 rg 100 600 100 50 re f Q\n" + txt(72, 700, "PAGE-%02d" % i) + txt(72, 300, "CANARY-PAGE-%02d" % i), compress=True)
        kids.append(p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>" % (pages, cs, font)))
    p.add("<< /Type /Pages /Kids [%s] /Count 30 >>" % " ".join("%d 0 R" % k for k in kids), pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    data, _ = p.serialize(cat)
    sel = (0, 17, 29)
    rects = [(i, 90, H - 660, 210, H - 590) for i in sel]
    present = [(i, 0, 0, 255, 60, 1000) for i in range(30) if i not in sel]
    absent = [(i, 0, 0, 255, 60) for i in sel]
    write("multipage.pdf", data, canaries=["CANARY-PAGE-%02d" % i for i in (0, 17, 29)], rects=rects, color_present=present, color_absent=absent, atts=0, sigs=0)
    return data


# ------------------------------------------------------------------ 12. encrypted
def f_encrypted(basic_bytes):
    if not shutil.which("qpdf"):
        return
    tmp = os.path.join(OUT, "_tmp_basic.pdf")
    open(tmp, "wb").write(basic_bytes)
    out = os.path.join(OUT, "encrypted_aes256.pdf")
    subprocess.run(["qpdf", "--encrypt", "userpw", "ownerpw", "256", "--", tmp, out], check=True)
    os.remove(tmp)
    raw = open(out, "rb").read()
    l, t, r, b = top_rect(60, 650, 400)
    write("encrypted_aes256.pdf", raw, password="userpw", canaries=["TOPSECRET-A1"], rects=[(0, l, t, r, b)], ink=[(0, 60, 80, 400, 100)], atts=0, sigs=0)
    write_variant("encrypted_aes256.pdf", "encrypted_nopw", raw, password="-", status="PasswordRequired", rects=[(0, l, t, r, b)])
    write_variant("encrypted_aes256.pdf", "encrypted_badpw", raw, password="wrong", status="BadPassword", rects=[(0, l, t, r, b)])


def write_variant(fname, label, data, **kw):
    lines = ["fixture %s" % fname, "label %s" % label, "password %s" % kw.get("password", "-"), "expect_status %s" % kw["status"]]
    for page, l, t, r, b in kw.get("rects", []):
        lines.append("rect %d %s %s %s %s" % (page, l, t, r, b))
    manifest.append("\n".join(lines))


# ------------------------------------------------------------------ 13. hostile / malformed
def f_hostile(basic_bytes):
    r = (0, 60, 124, 400, 152)
    cut = basic_bytes[: int(len(basic_bytes) * 0.6)]
    write("trunc.pdf", cut, rects=[r], status="Any", safe_only=True, compare_poppler=False)
    write("garbage.pdf", bytes(range(256)) * 40, rects=[r], status="InvalidFormat", compare_poppler=False)
    write("empty_pdf_header.pdf", b"%PDF-1.7\n", rects=[r], status="InvalidFormat", compare_poppler=False)

    # page tree loop
    p = Pdf(); pages = p.reserve()
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pages, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    write("pagetree_loop.pdf", p.serialize(cat)[0], rects=[r], status="Any", safe_only=True, compare_poppler=False)

    # self-referencing form xobject + absurd size declaration
    p = Pdf(); pages = p.reserve()
    xo = p.reserve()
    p.stream("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /Fm %d 0 R >> >>" % xo, "/Fm Do", compress=False, num=xo)
    cs = p.stream("", "/Fm Do\n", compress=False)
    pg = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /XObject << /Fm %d 0 R >> >> >>" % (pages, cs, xo))
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    write("form_recursion.pdf", p.serialize(cat)[0], rects=[r], status="Any", safe_only=True, compare_poppler=False)

    # enormous page (14400pt = PDF max) => must hit the pixel limit, not allocate
    p = Pdf(); pages = p.reserve()
    cs = p.stream("", "0 0 1 rg 0 0 100 100 re f", compress=False)
    pg = p.add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 14400 14400] /Contents %d 0 R /Resources << >> >>" % (pages, cs))
    p.add("<< /Type /Pages /Kids [%d 0 R] /Count 1 >>" % pg, pages)
    cat = p.add("<< /Type /Catalog /Pages %d 0 R >>" % pages)
    write("huge_page.pdf", p.serialize(cat)[0], rects=[(0, 10, 10, 50, 50)], status="LimitExceeded", compare_poppler=False)

    # lying stream length + bogus Size
    d = bytearray(basic_bytes)
    d = d.replace(b"/Length", b"/Length 99999999 /X")
    write("lying_length.pdf", bytes(d), rects=[r], status="Any", safe_only=True, compare_poppler=False)


def main():
    b = f_basic()
    f_hidden(); f_annots_forms(); f_attachments(); f_metadata(); f_incremental(); f_multifilter()
    f_geometry(); f_images(); f_signed(); f_multipage(); f_encrypted(b); f_hostile(b)
    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        f.write("# generated by tests/redaction/make_fixtures.py -- do not edit by hand\n\n")
        f.write("\n\n".join(manifest) + "\n")
    print("wrote", len(manifest), "manifest entries to", OUT)


if __name__ == "__main__":
    main()
