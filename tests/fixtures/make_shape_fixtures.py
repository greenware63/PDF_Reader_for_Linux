#!/usr/bin/env python3
"""Generates tests/fixtures/shapes_filled_and_empty.pdf, used by test_render (Test 40).

One page (612 x 792) with third-party style shape annotations. Names, rectangles and what decides "filled":

  filled-square      Square, /IC yellow, no /AP               rect 100 600 200 700   filled
  empty-square       Square, no /IC, no /AP                   rect 300 600 400 700   not filled
  filled-circle      Circle, /IC green, no /AP                rect 100 400 200 500   filled
  empty-circle       Circle, no /IC, no /AP                   rect 300 400 400 500   not filled
  clear-fill-square  Square, /IC yellow but /CA 0, no /AP     rect 100 200 200 300   not filled (invisible)
  black-fill-square  Square, /IC [0 0 0], no /AP              rect 300 200 400 300   filled (black is a colour)
  empty-ic-square    Square, /IC [] (transparent), no /AP     rect 450 200 550 300   not filled
  ap-filled-square   Square with its own /AP that fills it    rect 450 600 550 700   filled
  ap-outline-square  Square with its own /AP, stroke only     rect 450 400 550 500   not filled

Usage: python3 -I make_shape_fixtures.py <output file>
"""
import sys


def build(objs_after_page, annot_count):
    objs = [b"<</Type/Catalog/Pages 2 0 R>>", b"<</Type/Pages/Kids[3 0 R]/Count 1>>"]
    refs = b" ".join(b"%d 0 R" % (4 + i) for i in range(annot_count))
    objs.append(b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[" + refs + b"]>>")
    objs.extend(objs_after_page)
    out = bytearray(b"%PDF-1.7\n")
    offs = []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)


def shape(subtype, name, rect, interior=None, ca=None, ap_ref=None):
    extra = b""
    if interior is not None:
        extra += b"/IC[%s]" % interior
    if ca is not None:
        extra += b"/CA %s" % ca
    if ap_ref is not None:
        extra += b"/AP<</N %d 0 R>>" % ap_ref
    return (b"<</Type/Annot/Subtype/%s/NM(%s)/F 4/Rect[%s]/C[1 0 0]/BS<</W 2>>%s>>"
            % (subtype, name, rect, extra))


def form(rect, content):
    return (b"<</Type/XObject/Subtype/Form/BBox[%s]/Resources<<>>/Length %d>>\nstream\n%s\nendstream"
            % (rect, len(content), content))


def main():
    # 9 annotations = objects 4..12, then the two appearance streams = objects 13 and 14.
    annots = [
        shape(b"Square", b"filled-square", b"100 600 200 700", b"1 1 0"),
        shape(b"Square", b"empty-square", b"300 600 400 700"),
        shape(b"Circle", b"filled-circle", b"100 400 200 500", b"0 1 0"),
        shape(b"Circle", b"empty-circle", b"300 400 400 500"),
        shape(b"Square", b"clear-fill-square", b"100 200 200 300", b"1 1 0", b"0"),
        shape(b"Square", b"black-fill-square", b"300 200 400 300", b"0 0 0"),
        shape(b"Square", b"empty-ic-square", b"450 200 550 300", b""),
        shape(b"Square", b"ap-filled-square", b"450 600 550 700", b"1 1 0", None, 13),
        shape(b"Square", b"ap-outline-square", b"450 400 550 500", None, None, 14),
    ]
    aps = [
        form(b"450 600 550 700", b"1 1 0 rg 1 0 0 RG 2 w 451 601 98 98 re B"),
        form(b"450 400 550 500", b"1 0 0 RG 2 w 451 401 98 98 re S"),
    ]
    with open(sys.argv[1], "wb") as f:
        f.write(build(annots + aps, len(annots)))


if __name__ == "__main__":
    main()
