# Independent check of the files the spike saved: python3 -I verify_outputs.py <form.pdf> <outdir>
import glob, os, subprocess, sys
import pikepdf, pymupdf

form, outdir = sys.argv[1], sys.argv[2]
orig = open(form, "rb").read()

def fields(path):
    res = {}
    with pikepdf.open(path) as p:
        for a in p.pages[0].Annots:
            par = a.get("/Parent")
            def g(k):
                if k in a: return a[k]
                return par.get(k) if par is not None else None
            name = str(g("/T"))
            v = g("/V"); asv = a.get("/AS")
            res.setdefault(name, []).append((str(v) if v is not None else None, str(asv) if asv is not None else None))
        na = p.Root.AcroForm.get("/NeedAppearances")
    return res, na

for path in sorted(glob.glob(os.path.join(outdir, "*.pdf"))):
    data = open(path, "rb").read()
    q = subprocess.run(["qpdf", "--check", path], capture_output=True, text=True)
    qok = "No syntax or stream encoding errors found" in q.stdout
    res, na = fields(path)
    base = os.path.basename(path)
    print(f"== {base}: {len(data)} bytes (+{len(data)-len(orig)}), original is exact prefix={data.startswith(orig)}, qpdf clean={qok}, NeedAppearances={na}")
    interesting = {"type": ["name"], "nokill": ["name"], "toggle": ["agree", "locked", "rg"], "choice": ["country", "langs"],
                   "programmatic": ["name"], "persian": ["persian"], "reset": ["preset_text", "agree"]}
    key = next((k for k in interesting if base.startswith(k)), None)
    for f in interesting.get(key, []):
        print("   ", f, res.get(f))
