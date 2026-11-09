import sys, os

def build_pdf(out_path):
    objs = {}
    
    # 1: Catalog
    objs[1] = b"<</Type/Catalog/Pages 2 0 R/AcroForm<</Fields[4 0 R 5 0 R 6 0 R 7 0 R 10 0 R 11 0 R]/DR<</Font<</Helv 3 0 R>>>>/NeedAppearances false>>>>"
    # 2: Pages
    objs[2] = b"<</Type/Pages/Kids[8 0 R]/Count 1>>"
    # 3: Font
    objs[3] = b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>"
    # 4: Text field 'name'
    objs[4] = b"<</Type/Annot/Subtype/Widget/FT/Tx/T(name)/V(Alice)/Rect[72 700 250 724]/DA(/Helv 12 Tf 0 g)/F 4/P 8 0 R>>"
    # 5: Read-only text field 'locked' (Ff 1)
    objs[5] = b"<</Type/Annot/Subtype/Widget/FT/Tx/T(locked)/V(CannotEdit)/Ff 1/Rect[72 650 250 674]/DA(/Helv 12 Tf 0 g)/F 4/P 8 0 R>>"
    # 6: Checkbox 'agree'
    objs[6] = b"<</Type/Annot/Subtype/Widget/FT/Btn/T(agree)/V/Off/AS/Off/Rect[72 600 92 620]/F 4/P 8 0 R/AP<</N<</Yes 12 0 R/Off 13 0 R>>>>>>"
    # 7: Radio group 'rg' parent (Ff 49152 = Radio 32768 | NoToggleToOff 16384)
    objs[7] = b"<</FT/Btn/T(rg)/Ff 49152/V/Choice1/Kids[14 0 R 15 0 R]>>"
    # 8: Page
    objs[8] = b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 9 0 R/Resources<</Font<</Helv 3 0 R>>>>/Annots[4 0 R 5 0 R 6 0 R 14 0 R 15 0 R 10 0 R 11 0 R]>>"
    # 9: Page Contents
    content = b"BT /Helv 12 Tf 72 750 Td (M5 Form Fixture) Tj ET"
    objs[9] = b"<</Length %d>>\nstream\n%s\nendstream" % (len(content), content)
    # 10: Single-select combo 'country' (Ff 131072 = Combo)
    objs[10] = b"<</Type/Annot/Subtype/Widget/FT/Ch/Ff 131072/T(country)/V(Germany)/Opt[(Germany)(Iran)(France)]/Rect[72 500 250 524]/DA(/Helv 12 Tf 0 g)/F 4/P 8 0 R>>"
    # 11: Multi-select listbox 'langs' (Ff 2097152 = MultiSelect)
    objs[11] = b"<</Type/Annot/Subtype/Widget/FT/Ch/Ff 2097152/T(langs)/Opt[(English)(Persian)(German)(French)]/I[0]/V[(English)]/Rect[72 400 250 480]/DA(/Helv 12 Tf 0 g)/F 4/P 8 0 R>>"
    # 12, 13: Checkbox AP streams
    ap_stream = b"0 0 1 rg 0 0 20 20 re f"
    objs[12] = b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>/Length %d>>\nstream\n%s\nendstream" % (len(ap_stream), ap_stream)
    objs[13] = b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>/Length 0>>\nstream\n\nendstream"
    # 14: Radio kid 0
    objs[14] = b"<</Type/Annot/Subtype/Widget/Parent 7 0 R/Rect[72 550 92 570]/F 4/P 8 0 R/AS/Choice1/AP<</N<</Choice1 16 0 R/Off 17 0 R>>>>>>"
    # 15: Radio kid 1
    objs[15] = b"<</Type/Annot/Subtype/Widget/Parent 7 0 R/Rect[120 550 140 570]/F 4/P 8 0 R/AS/Off/AP<</N<</Choice2 18 0 R/Off 17 0 R>>>>>>"
    # 16, 17, 18: Radio AP streams
    objs[16] = b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>/Length %d>>\nstream\n%s\nendstream" % (len(ap_stream), ap_stream)
    objs[17] = b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>/Length 0>>\nstream\n\nendstream"
    objs[18] = b"<</Type/XObject/Subtype/Form/BBox[0 0 20 20]/Resources<<>>/Length %d>>\nstream\n%s\nendstream" % (len(ap_stream), ap_stream)

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for num in sorted(objs.keys()):
        offs[num] = len(out)
        out += b"%d 0 obj\n" % num + objs[num] + b"\nendobj\n"
    
    xref_pos = len(out)
    size = max(objs.keys()) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for num in range(1, size):
        if num in offs:
            out += b"%010d 00000 n \n" % offs[num]
        else:
            out += b"%010d 00000 f \n" % 0
            
    out += b"trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (size, xref_pos)
    
    with open(out_path, "wb") as f:
        f.write(out)
    print("Wrote", out_path, len(out), "bytes")

if __name__ == "__main__":
    p = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/form_fields_test.pdf"
    build_pdf(p)
