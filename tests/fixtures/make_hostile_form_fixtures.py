#!/usr/bin/env python3
"""Generates the AcroForm fixtures for Milestone M5 (ListFormFields and the form edit commands).

Usage:   python3 -I make_hostile_form_fixtures.py <output dir>
Needs:   qpdf (object streams + flate keep the repetitive hostile data small on disk)

Every file is tiny on disk but, if its fields were listed naively, would produce far more data than one IPC response
may carry, or would walk structures that are deliberately pathological. The limits quoted below are the ones in
include/engine/FormFieldTypes.h of the M5 slice 1 PR (1,000 fields per page, 1,000 options per field, 32,768 UTF-16
units per value, 1,024 characters per name or option, 16 MiB per page response).

  hostile_form_many_widgets.pdf        page 1: 4,000 text fields (4x the per-page cap); page 2: 3 ordinary fields,
                                       so a test can prove the session still works on a normal page afterwards
  hostile_form_big_values.pdf          700 fields whose values are 30,000 UTF-16 units each (alternating ASCII and
                                       Persian): every value is under the per-value cap, but together they are
                                       about 42 MB, so the 16 MiB response budget stops the list before the count cap.
                                       Every widget has an appearance, so the page loads in about 0.3 s
  hostile_form_slow_load_MANUAL.pdf    the same 700 fields WITHOUT appearances. PDFium builds one per widget while the
                                       page loads: about 17 s and 513 MiB on a 2-core VM, before any listing code runs.
                                       NOT for ctest. It exercises the watchdog (60 s stall limit), the UI-side timeout
                                       of the synchronous calls (10 s) and the 4 GiB address-space limit
  hostile_form_oversize_values.pdf     three fields whose values are over the per-value cap (40,000 units), over the
                                       1 MiB read guard (600,000 units) and 10 MB long (5,000,000 units)
  hostile_form_huge_options.pdf        combo and list boxes with 20,000 short options (cap: 1,000), a combo with 1,200
                                       options of 20,000 characters (cap: 1,024 each), a combo whose 3 options are
                                       2,000,000 characters each, and 5,000 [export, label] option pairs
  hostile_form_deep_hierarchy.pdf      two fields: one with 31 ancestors of 50-character names (the deepest the pinned PDFium
                                       resolves; qualified name 1,585 characters, cap 1,024) and one with 3,000 ancestors,
                                       which PDFium reports as an unknown widget with no name or value
  hostile_form_long_names.pdf          300 fields with 5,000-character names, a two-level name of 6,002 characters,
                                       400 widgets sharing one name, and names with dots, a NUL and bidi marks
  hostile_form_cycles_and_malformed.pdf  /Parent and /Kids cycles, bad /Fields entries, absurd and enormous /Rect, /Ff, /MaxLen,
                                       /Q, /Opt, /I and /V values, orphan fields and orphan widgets (written
                                       without qpdf, which cannot represent integers beyond 64 bits)
  forms_identity_and_topology.pdf      WELL-FORMED, for the identity and replay tests: two sticky notes BEFORE the
                                       widgets in /Annots, nested names (form1.name), a 3-way radio group, a field with
                                       controls on two pages, a read-only checkbox, comb / password / multi-line
                                       fields, a Persian value and a Persian combo, and a /Rotate 90 page

The fixtures are deterministic: running the generator twice produces identical files.
"""
import os
import subprocess
import sys

PAGE_W, PAGE_H = 612, 792


# ---------------------------------------------------------------------------------------------------- PDF writer
class Pdf:
    def __init__(self):
        self.objs = []  # index 0 is object 1

    def reserve(self):
        self.objs.append(None)
        return len(self.objs)

    def add(self, body):
        self.objs.append(body)
        return len(self.objs)

    def set(self, num, body):
        self.objs[num - 1] = body

    def stream(self, dict_body, data):
        return b"<<" + dict_body + b"/Length %d>>\nstream\n" % len(data) + data + b"\nendstream"

    def build(self, root):
        out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
        offs = []
        for i, body in enumerate(self.objs, 1):
            assert body is not None, "object %d was reserved but never set" % i
            offs.append(len(out))
            out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(self.objs) + 1)
        for o in offs:
            out += b"%010d 00000 n \n" % o
        out += b"trailer\n<</Size %d/Root %d 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(self.objs) + 1, root, xref)
        return bytes(out)


def ref(n):
    return b"%d 0 R" % n


def refs(ns):
    return b"[" + b" ".join(ref(n) for n in ns) + b"]"


def hexs(text):
    """PDF text string as UTF-16BE with a BOM (hex string): any Unicode, no escaping problems."""
    return b"<FEFF" + text.encode("utf-16-be").hex().upper().encode() + b">"


def asc(text):
    """Plain ASCII/PDFDocEncoding text as a hex string."""
    return b"<" + text.encode("latin-1").hex().upper().encode() + b">"


def rect(x0, y0, x1, y1):
    return b"[%g %g %g %g]" % (x0, y0, x1, y1)


def repeat_to(pattern, n):
    return (pattern * (n // len(pattern) + 1))[:n]


FA_WORD = "\u0633\u0644\u0627\u0645 \u062f\u0646\u06cc\u0627 \u06a9\u0627\u0631 \u06af\u0644 \u0686\u0627\u067e \u0698\u0644\u0647 "  # salam donya kar gol chap zhale


class Form:
    """A one-page-at-a-time AcroForm document builder."""

    def __init__(self):
        self.pdf = Pdf()
        self.catalog = self.pdf.reserve()
        self.pages_obj = self.pdf.reserve()
        self.helv = self.pdf.add(b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>")
        self.pages = []      # (page object number, [annotation object numbers], extra page dict bytes)
        self.fields = []     # top-level field object numbers for /AcroForm /Fields
        self.acro_extra = b""

    def new_page(self, extra=b""):
        num = self.pdf.reserve()
        self.pages.append([num, [], extra])
        return num

    def annot_on(self, page_num, annot_num):
        for p in self.pages:
            if p[0] == page_num:
                p[1].append(annot_num)
                return
        raise KeyError(page_num)

    def finish(self, fields_override=None):
        kids = [p[0] for p in self.pages]
        for num, annots, extra in self.pages:
            self.pdf.set(num, b"<</Type/Page/Parent %s/MediaBox[0 0 %d %d]/Annots %s%s>>"
                         % (ref(self.pages_obj), PAGE_W, PAGE_H, refs(annots), extra))
        self.pdf.set(self.pages_obj, b"<</Type/Pages/Kids %s/Count %d>>" % (refs(kids), len(kids)))
        fields = fields_override if fields_override is not None else refs(self.fields)
        self.pdf.set(self.catalog, b"<</Type/Catalog/Pages %s/AcroForm<</Fields %s/DA(/Helv 12 Tf 0 g)"
                     b"/DR<</Font<</Helv %s>>>>%s>>>>" % (ref(self.pages_obj), fields, ref(self.helv), self.acro_extra))
        return self.pdf.build(self.catalog)

    # -- field helpers --------------------------------------------------------------------------------------
    def blank_ap(self):
        """One tiny appearance stream, shared by every widget that should NOT make PDFium build its own on load."""
        if getattr(self, "_blank_ap", None) is None:
            self._blank_ap = self.pdf.add(self.pdf.stream(b"/Type/XObject/Subtype/Form/BBox[0 0 100 20]/Resources<<>>",
                                                          b"0.95 g 0 0 100 20 re f"))
        return self._blank_ap

    def text_widget(self, page_num, name, value, r, extra=b"", top_level=True, default_da=True, default_f=True, ap=False):
        da = b"/DA(/Helv 12 Tf 0 g)" if default_da else b""
        fl = b"/F 4" if default_f else b""
        if ap:
            extra = extra + b"/AP<</N %s>>" % ref(self.blank_ap())
        num = self.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Tx/T %s/V %s%s/Rect %s%s/P %s%s>>"
                           % (name, value, da, r, fl, ref(page_num), extra))
        self.annot_on(page_num, num)
        if top_level:
            self.fields.append(num)
        return num


def qpdf_compress(raw, path):
    tmp = path + ".raw"
    with open(tmp, "wb") as f:
        f.write(raw)
    subprocess.run(["qpdf", "--object-streams=generate", "--compress-streams=y", "--recompress-flate",
                    "--deterministic-id", tmp, path], check=True)
    os.remove(tmp)


# ---------------------------------------------------------------------------------------------------- fixtures
def many_widgets():
    f = Form()
    p1 = f.new_page()
    for i in range(4000):
        x, y = 10 + 9 * (i % 60), 20 + 9 * (i // 60)
        f.text_widget(p1, asc("w%04d" % i), asc("v%04d" % i), rect(x, y, x + 8, y + 8))
    p2 = f.new_page()
    for i, nm in enumerate(("ok1", "ok2", "ok3")):
        f.text_widget(p2, asc(nm), asc("fine %d" % i), rect(72, 700 - 40 * i, 300, 724 - 40 * i))
    return f.finish()


def big_values(with_appearance=True):
    f = Form()
    p1 = f.new_page()
    ascii_v = asc(repeat_to("The quick brown fox jumps over the lazy dog. ", 30000))
    persian_v = hexs(repeat_to(FA_WORD, 30000))
    for i in range(700):
        x, y = 10 + 40 * (i % 14), 20 + 12 * (i // 14)
        f.text_widget(p1, asc("big%03d" % i), ascii_v if i % 2 == 0 else persian_v, rect(x, y, x + 36, y + 10),
                      extra=b"/Ff 4096", ap=with_appearance)  # multi-line
    return f.finish()


def slow_load():
    """The same page WITHOUT appearances: PDFium builds one per widget while the page loads (about 17 s and 500 MiB)."""
    return big_values(with_appearance=False)


def oversize_values():
    f = Form()
    p1 = f.new_page()
    f.text_widget(p1, asc("over_value_cap"), asc(repeat_to("0123456789", 40000)), rect(72, 700, 300, 724), ap=True)
    f.text_widget(p1, asc("over_read_guard"), hexs(repeat_to(FA_WORD, 600000)), rect(72, 650, 300, 674), ap=True)
    f.text_widget(p1, asc("ten_megabytes"), asc(repeat_to("abcdefghij", 5000000)), rect(72, 600, 300, 624), ap=True)
    f.text_widget(p1, asc("normal"), asc("a normal value"), rect(72, 550, 300, 574))
    return f.finish()


def choice_field(f, page_num, name, flags, options_body, selected=b"", r=b"[72 700 300 724]", extra=b""):
    num = f.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Ch/T %s/Ff %d/Opt %s%s/DA(/Helv 12 Tf 0 g)/Rect %s/F 4/P %s%s>>"
                    % (name, flags, options_body, selected, r, ref(page_num), extra))
    f.annot_on(page_num, num)
    f.fields.append(num)
    return num


def huge_options():
    COMBO, LIST_EDIT = 1 << 17, 0
    f = Form()
    p1 = f.new_page()
    short = b"[" + b"".join(asc("Option %05d" % i) for i in range(20000)) + b"]"
    choice_field(f, p1, asc("many_opts_combo"), COMBO, short, b"/V " + asc("Option 00007"), b"[72 740 300 764]")
    choice_field(f, p1, asc("many_opts_list"), LIST_EDIT, short, b"/V " + asc("Option 00042") + b"/I [42]", b"[72 600 300 700]")
    long_label = repeat_to("abcdefghijklmnopqrstuvwxyz0123456789", 20000)
    longs = b"[" + b"".join(asc("%04d" % i + long_label) for i in range(1200)) + b"]"
    choice_field(f, p1, asc("long_opts_combo"), COMBO, longs, b"", b"[72 560 300 584]")
    giant = repeat_to("Z", 2000000)
    choice_field(f, p1, asc("giant_opts_combo"), COMBO, b"[" + b"".join(asc(giant) for _ in range(3)) + b"]", b"",
                 b"[72 520 300 544]")
    pairs = b"[" + b"".join(b"[" + asc("export%05d" % i) + asc("Label %05d" % i) + b"]" for i in range(5000)) + b"]"
    choice_field(f, p1, asc("pair_opts_combo"), COMBO, pairs, b"", b"[72 480 300 504]")
    return f.finish()


def field_chain(f, page_num, depth, partial_name, leaf_name, rect_, value):
    """A field `depth` levels deep: `depth` ancestors (each named `partial_name`) above one widget named `leaf_name`."""
    chain = [f.pdf.reserve() for _ in range(depth)]
    leaf = f.pdf.reserve()
    for i, num in enumerate(chain):
        child = chain[i + 1] if i + 1 < depth else leaf
        parent = b"/Parent %s" % ref(chain[i - 1]) if i else b""
        f.pdf.set(num, b"<</T %s/Kids [%s]%s>>" % (asc(partial_name), ref(child), parent))
    f.pdf.set(leaf, b"<</Type/Annot/Subtype/Widget/FT/Tx/T %s/V %s/DA(/Helv 12 Tf 0 g)/Rect %s/F 4/P %s/Parent %s>>"
              % (asc(leaf_name), asc(value), rect_, ref(page_num), ref(chain[-1])))
    f.annot_on(page_num, leaf)
    f.fields.append(chain[0])


def deep_hierarchy():
    # The pinned PDFium (chromium/8100) resolves a field at most 31 ancestors deep; from 32 on the widget is reported as
    # type "unknown" with an empty name and value (measured with tests/spikes/m5_fixture_probe.cpp).
    f = Form()
    p1 = f.new_page()
    # 31 ancestors with 50-character names: resolvable, and its fully qualified name is 1,585 characters (cap: 1,024)
    field_chain(f, p1, 31, "a" * 50, "leaf", rect(72, 700, 300, 724), "deep but resolvable")
    # 3,000 ancestors: PDFium gives up on it, so it must come back as an unknown widget (or be skipped), never a crash
    field_chain(f, p1, 3000, "b", "too_deep", rect(72, 650, 300, 674), "unreachable")
    f.text_widget(p1, asc("after_deep"), asc("still works"), rect(72, 600, 300, 624))
    return f.finish()


def long_names():
    f = Form()
    p1 = f.new_page()
    for i in range(300):
        x, y = 10 + 90 * (i % 6), 20 + 14 * (i // 6)
        f.text_widget(p1, asc("%04d" % i + repeat_to("N", 5000)), asc("v"), rect(x, y, x + 80, y + 12))
    kid = f.pdf.reserve()
    parent = f.pdf.add(b"<</T %s/Kids [%s]>>" % (asc(repeat_to("P", 3000)), ref(kid)))
    f.pdf.set(kid, b"<</Type/Annot/Subtype/Widget/FT/Tx/T %s/V(x)/DA(/Helv 12 Tf 0 g)/Rect [72 700 300 724]/F 4/P %s/Parent %s>>"
              % (asc(repeat_to("K", 3001)), ref(p1), ref(parent)))
    f.annot_on(p1, kid)
    f.fields.append(parent)
    for i in range(400):  # 400 widgets, one name
        f.text_widget(p1, asc("dup"), asc("copy %d" % i), rect(10 + (i % 60) * 9, 400 + 9 * (i // 60), 18 + (i % 60) * 9, 408 + 9 * (i // 60)))
    f.text_widget(p1, hexs("a.b.c"), asc("dotted"), rect(72, 300, 300, 324))
    f.text_widget(p1, hexs("nul\u0000inside"), asc("nul"), rect(72, 260, 300, 284))
    f.text_widget(p1, hexs("\u202eevil\u202c\u200f\u0645\u0631\u062d\u0628\u0627"), asc("bidi"), rect(72, 220, 300, 244))
    return f.finish()


def cycles_and_malformed():
    f = Form()
    p1 = f.new_page()
    # --- cycles --------------------------------------------------------------------------------------------
    a, b, c, w_cyc, w_self = (f.pdf.reserve() for _ in range(5))
    f.pdf.set(a, b"<</T(cycA)/Parent %s/Kids [%s]>>" % (ref(b), ref(w_cyc)))
    f.pdf.set(b, b"<</T(cycB)/Parent %s/Kids [%s]>>" % (ref(a), ref(a)))
    f.pdf.set(w_cyc, b"<</Type/Annot/Subtype/Widget/FT/Tx/T(in_cycle)/V(x)/DA(/Helv 12 Tf 0 g)/Rect [10 740 100 760]/F 4"
                     b"/P %s/Parent %s>>" % (ref(p1), ref(a)))
    f.pdf.set(c, b"<</T(selfkid)/Kids [%s]>>" % ref(c))
    f.pdf.set(w_self, b"<</Type/Annot/Subtype/Widget/FT/Tx/T(self_parent)/V(x)/DA(/Helv 12 Tf 0 g)/Rect [110 740 200 760]"
                      b"/F 4/P %s/Parent %s>>" % (ref(p1), ref(w_self)))
    for n in (w_cyc, w_self):
        f.annot_on(p1, n)
    f.fields += [a, c, w_self]
    # --- absurd geometry (the numbers are legal PDF reals/integers) -------------------------------------------
    # Finite but enormous rectangles are legal and survive PDFium's parser (integers beyond 32 bits become 0, these do not):
    # a field whose rectangle covers the whole plane would capture every click if a hit test trusted it.
    for name, r in ((b"rect_huge_int", b"[-2000000000 -2000000000 2000000000 2000000000]"),
                    (b"rect_huge_real", b"[-3000000000.5 -3000000000.5 3000000000.5 3000000000.5]"),
                    (b"rect_off_page", b"[100000 100000 100100 100100]")):
        f.text_widget(p1, asc(name.decode()), asc("x"), r)
    rects = [b"[0 0 0 0]", b"[100 100 0 0]", b"[99999999999999999999 1 2 3]", b"[-34" + b"0" * 37 + b" 0 0 0]",
             b"[1 2 3]", b"[(a) (b) (c) (d)]", b"null", b"[0.0000000001 0.0000000001 0.0000000002 0.0000000002]"]
    for i, r in enumerate(rects):
        f.text_widget(p1, asc("rect%d" % i), asc("x"), r)
    # --- absurd /Ff /MaxLen /Q /DA /V -------------------------------------------------------------------------
    # (key, replaces the default /DA, replaces the default /F)
    odd = [(b"/Ff (x)", False, False), (b"/Ff -1", False, False), (b"/Ff 99999999999", False, False), (b"/Ff 1.5", False, False),
           (b"/MaxLen -5", False, False), (b"/MaxLen 99999999999", False, False), (b"/Q 99", False, False), (b"/Q (x)", False, False),
           (b"/DA " + asc(repeat_to("0 g ", 100000)), True, False), (b"/DA 12", True, False), (b"/AP 5", False, False),
           (b"/MK (x)", False, False), (b"/BS [1]", False, False), (b"/F (x)", False, True)]
    for i, (e, drop_da, drop_f) in enumerate(odd):
        f.text_widget(p1, asc("odd%d" % i), asc("x"), rect(10, 600 - 14 * i, 100, 612 - 14 * i), extra=e,
                      default_da=not drop_da, default_f=not drop_f)
    # /V of strange types
    stream_v = f.pdf.add(f.pdf.stream(b"", b"rich text as a stream"))
    for i, v in enumerate((b"[1 2 3]", b"<</a 1>>", b"/NameValue", ref(stream_v), b"null", b"true", b"-0", b"999999999999999999999")):
        f.text_widget(p1, asc("vtype%d" % i), v, rect(120, 600 - 14 * i, 220, 612 - 14 * i))
    # --- /Opt and /I abuse on choice fields ---------------------------------------------------------------------
    nest = b"[" * 60 + asc("deep") + b"]" * 60
    weird_opts = b"[%s 12 /Name <</a 1>> null [] [%s] [%s %s %s] true %s]" % (asc("ok"), asc("one"), asc("a"), asc("b"), asc("c"), nest)
    COMBO = 1 << 17
    choice_field(f, p1, asc("opt_abuse_combo"), COMBO, weird_opts, b"/V (not an option)/I [999999 -1 (x) 3.5]", b"[230 700 400 724]")
    choice_field(f, p1, asc("opt_notarray_list"), 0, b"12345", b"", b"[230 660 400 684]")
    choice_field(f, p1, asc("opt_missing_list"), 0, b"null", b"", b"[230 620 400 644]")
    # --- buttons without appearances / with dangling states -----------------------------------------------------------
    for body in (b"<</Type/Annot/Subtype/Widget/FT/Btn/Ff 49152/T(radio_no_ap)/AS/Missing/Rect [230 560 250 580]/F 4/P %s>>" % ref(p1),
                 b"<</Type/Annot/Subtype/Widget/FT/Btn/T(check_bad_as)/V/Yes/AS/Nope/AP<</N<</Yes %s>>>>/Rect [260 560 280 580]/F 4/P %s>>"
                 % (ref(f.catalog), ref(p1)),  # the "appearance stream" is the catalog dictionary
                 b"<</Type/Annot/Subtype/Widget/FT/Sig/T(sig_bogus)/V<</Type/Sig/Contents <00>/ByteRange [0 1 2 3]>>/Rect [290 560 400 600]/F 4/P %s>>" % ref(p1)):
        n = f.pdf.add(body)
        f.annot_on(p1, n)
        f.fields.append(n)
    # --- orphans ------------------------------------------------------------------------------------------------
    orphan_widget = f.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Tx/T(orphan_widget)/V(not in /Fields)/Rect [10 20 100 40]/F 4/P %s>>" % ref(p1))
    f.annot_on(p1, orphan_widget)
    f.fields.append(f.pdf.add(b"<</FT/Tx/T(orphan_field)/V(on no page)>>"))  # in /Fields, but on no page
    f.annot_on(p1, 9999)                   # a dangling reference in /Annots
    f.annot_on(p1, f.catalog)              # a non-annotation object in /Annots
    # /Fields itself is malformed: numbers, null, a dangling reference, the same field twice, a non-dictionary
    fields = b"[%s 17 null 9999 0 R %s %s (text)]" % (refs(f.fields)[1:-1], ref(f.fields[0]), ref(f.fields[0]))
    return f.finish(fields_override=fields)


def identity_and_topology():
    f = Form()
    p1 = f.new_page()
    p2 = f.new_page()
    p3 = f.new_page(extra=b"/Rotate 90")
    # two sticky notes FIRST in /Annots: deleting one shifts the /Annots position of every widget after it
    for nm, txt, y in ((b"note-a", b"first note", 750), (b"note-b", b"second note", 720)):
        note = f.pdf.add(b"<</Type/Annot/Subtype/Text/NM(%s)/Contents(%s)/Rect [20 %d 44 %d]/F 4/P %s>>" % (nm, txt, y, y + 24, ref(p1)))
        f.annot_on(p1, note)
    # nested names: form1.name and form1.city
    form1 = f.pdf.reserve()
    name_w = f.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Tx/T(name)/V(Ada)/DA(/Helv 12 Tf 0 g)/Rect [72 690 300 714]/F 4/P %s/Parent %s>>"
                       % (ref(p1), ref(form1)))
    city_w = f.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Tx/T(city)/V(Berlin)/DA(/Helv 12 Tf 0 g)/Rect [72 650 300 674]/F 4/P %s/Parent %s>>"
                       % (ref(p1), ref(form1)))
    f.pdf.set(form1, b"<</T(form1)/Kids [%s %s]>>" % (ref(name_w), ref(city_w)))
    f.annot_on(p1, name_w)
    f.annot_on(p1, city_w)
    f.fields.append(form1)
    # a 3-way radio group with appearances
    on = f.pdf.add(f.pdf.stream(b"/Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>", b"0 g 5 5 10 10 re f"))
    off = f.pdf.add(f.pdf.stream(b"/Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>", b"0.85 g 0 0 20 20 re f"))
    radio = f.pdf.reserve()
    kids = []
    for i, state in enumerate((b"A", b"B", b"C")):
        k = f.pdf.add(b"<</Type/Annot/Subtype/Widget/Parent %s/Rect [%d 600 %d 620]/F 4/P %s/AS/%s/AP<</N<</%s %s/Off %s>>>>>>"
                      % (ref(radio), 72 + 40 * i, 92 + 40 * i, ref(p1), b"B" if state == b"B" else b"Off", state, ref(on), ref(off)))
        kids.append(k)
        f.annot_on(p1, k)
    f.pdf.set(radio, b"<</FT/Btn/Ff 49152/T(choice)/V/B/Kids %s>>" % refs(kids))
    f.fields.append(radio)
    # one field, two controls on two pages: a single shared value
    shared = f.pdf.reserve()
    s1 = f.pdf.add(b"<</Type/Annot/Subtype/Widget/Parent %s/Rect [72 560 300 584]/F 4/P %s>>" % (ref(shared), ref(p1)))
    s2 = f.pdf.add(b"<</Type/Annot/Subtype/Widget/Parent %s/Rect [72 700 300 724]/F 4/P %s>>" % (ref(shared), ref(p2)))
    f.pdf.set(shared, b"<</FT/Tx/T(shared)/V(one value, two controls)/DA(/Helv 12 Tf 0 g)/Kids [%s %s]>>" % (ref(s1), ref(s2)))
    f.annot_on(p1, s1)
    f.annot_on(p2, s2)
    f.fields.append(shared)
    # read-only checkbox, already checked
    yes = f.pdf.add(f.pdf.stream(b"/Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>", b"0 g 4 4 12 12 re f"))
    locked = f.pdf.add(b"<</Type/Annot/Subtype/Widget/FT/Btn/Ff 1/T(locked)/V/Yes/AS/Yes/AP<</N<</Yes %s/Off %s>>>>/Rect [72 520 92 540]/F 4/P %s>>"
                       % (ref(yes), ref(off), ref(p1)))
    f.annot_on(p1, locked)
    f.fields.append(locked)
    # Persian value and a Persian combo
    f.text_widget(p1, asc("persian_name"), hexs("\u0646\u0627\u0645 \u062e\u0627\u0646\u0648\u0627\u062f\u06af\u06cc \u06a9\u0648\u0686\u06a9"), rect(72, 480, 300, 504))
    choice_field(f, p1, asc("persian_combo"), 1 << 17,
                 b"[" + b"".join(hexs(s) for s in ("\u062a\u0647\u0631\u0627\u0646", "\u0627\u0635\u0641\u0647\u0627\u0646", "\u0634\u06cc\u0631\u0627\u0632")) + b"]",
                 b"/V " + hexs("\u0627\u0635\u0641\u0647\u0627\u0646"), b"[72 440 300 464]")
    # comb, password and multi-line text fields on page 2
    f.text_widget(p2, asc("comb8"), asc("12345678"), rect(72, 650, 300, 674), extra=b"/Ff %d/MaxLen 8" % (1 << 24))
    f.text_widget(p2, asc("secret"), asc("hunter2"), rect(72, 610, 300, 634), extra=b"/Ff %d" % (1 << 13))
    f.text_widget(p2, asc("notes"), asc("line one\rline two"), rect(72, 500, 300, 590), extra=b"/Ff %d" % (1 << 12))
    # a field on a rotated page
    f.text_widget(p3, asc("rotated_field"), asc("sideways"), rect(100, 100, 300, 130))
    return f.finish()


# (file name, builder, compress with qpdf). The malformed fixture is written as it is: qpdf cannot represent integers
# beyond 64 bits and would silently turn those objects into null, which would defeat the point of the fixture.
FIXTURES = [
    ("hostile_form_many_widgets.pdf", many_widgets, True),
    ("hostile_form_big_values.pdf", big_values, True),
    ("hostile_form_slow_load_MANUAL.pdf", slow_load, True),
    ("hostile_form_oversize_values.pdf", oversize_values, True),
    ("hostile_form_huge_options.pdf", huge_options, True),
    ("hostile_form_deep_hierarchy.pdf", deep_hierarchy, True),
    ("hostile_form_long_names.pdf", long_names, True),
    ("hostile_form_cycles_and_malformed.pdf", cycles_and_malformed, False),
    ("forms_identity_and_topology.pdf", identity_and_topology, True),
]


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name, build, compress in FIXTURES:
        path = os.path.join(outdir, name)
        if compress:
            qpdf_compress(build(), path)
        else:
            with open(path, "wb") as fh:
                fh.write(build())
        print("%-42s %9d bytes" % (name, os.path.getsize(path)))


if __name__ == "__main__":
    main()
