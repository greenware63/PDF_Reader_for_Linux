#!/usr/bin/env python3
"""Generates the hostile-annotation fixtures used by test_worker_ipc (Test 29).

Both files are tiny on disk (object streams compress the repetitive data) but, if listed naively, would produce
an annotation list far larger than the 64 MiB IPC frame limit:

  hostile_annot_big_contents.pdf  1500 Text annotations, /Contents = 60 000 chars each  (~180 MB as UTF-16)
  hostile_annot_many_quads.pdf     400 Highlight annotations, 4 096 quad points each     (~105 MB)

Needs: qpdf. Usage: python3 -I make_hostile_annotation_fixtures.py <output dir>
"""
import os
import subprocess
import sys


def build(path, annots):
    objs = [b"<</Type/Catalog/Pages 2 0 R>>", b"<</Type/Pages/Kids[3 0 R]/Count 1>>"]
    refs = b" ".join(b"%d 0 R" % (4 + i) for i in range(len(annots)))
    objs.append(b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Annots[" + refs + b"]>>")
    objs.extend(annots)
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
    raw = path + ".raw"
    with open(raw, "wb") as f:
        f.write(out)
    subprocess.run(["qpdf", "--object-streams=generate", "--compress-streams=y", "--recompress-flate", raw, path], check=True)
    os.remove(raw)


def main():
    outdir = sys.argv[1]
    body = b"A" * 60000
    text = [b"<</Type/Annot/Subtype/Text/Rect[%d %d %d %d]/Contents(" % (10 + (i % 50) * 10, 10 + (i // 50) * 10,
                                                                         18 + (i % 50) * 10, 18 + (i // 50) * 10) + body + b")>>"
            for i in range(1500)]
    build(os.path.join(outdir, "hostile_annot_big_contents.pdf"), text)

    quads = b" ".join([b"100 560 250 560 100 500 250 500"] * 4096)
    marks = [b"<</Type/Annot/Subtype/Highlight/Rect[100 500 250 560]/C[1 1 0]/QuadPoints[" + quads + b"]>>"
             for _ in range(400)]
    build(os.path.join(outdir, "hostile_annot_many_quads.pdf"), marks)


if __name__ == "__main__":
    main()
