# Builds a test AcroForm PDF for the M5 spike: python3 -I make_form.py <out.pdf>
import sys, pymupdf, pikepdf
from pikepdf import Name, Dictionary, String, Array

out = sys.argv[1]
doc = pymupdf.open()
page = doc.new_page(width=612, height=792)

def widget(name, ftype, rect, value="", **kw):
    w = pymupdf.Widget()
    w.field_name = name
    w.field_type = ftype
    w.rect = pymupdf.Rect(*rect)
    w.field_value = value
    w.text_fontsize = kw.pop("fontsize", 12)
    for k, v in kw.items():
        setattr(w, k, v)
    page.add_widget(w)

T, C, R, CB, LB, BT = (pymupdf.PDF_WIDGET_TYPE_TEXT, pymupdf.PDF_WIDGET_TYPE_CHECKBOX, pymupdf.PDF_WIDGET_TYPE_RADIOBUTTON,
                       pymupdf.PDF_WIDGET_TYPE_COMBOBOX, pymupdf.PDF_WIDGET_TYPE_LISTBOX, pymupdf.PDF_WIDGET_TYPE_BUTTON)
widget("name",      T,  (72, 72, 300, 96))
widget("notes",     T,  (72, 110, 300, 190), field_flags=pymupdf.PDF_TX_FIELD_IS_MULTILINE)
widget("agree",     C,  (72, 210, 92, 230))
widget("locked",    C,  (120, 210, 140, 230), field_flags=pymupdf.PDF_FIELD_IS_READ_ONLY)
widget("color",     R,  (72, 250, 92, 270))
widget("color",     R,  (120, 250, 140, 270))
widget("country",   CB, (72, 290, 250, 314), "Germany", choice_values=["Germany", "Iran", "France"])
widget("langs",     LB, (72, 330, 250, 390), "English", choice_values=["English", "Persian", "German", "French"])
widget("js_field",  T,  (72, 410, 300, 434))
widget("persian",   T,  (72, 450, 300, 474))
widget("preset_text", T, (72, 490, 300, 514), "preset")
widget("reset_btn", BT, (72, 530, 150, 554), button_caption="Reset")
widget("uri_btn",   BT, (170, 530, 250, 554), button_caption="Open")
widget("submit_btn", BT, (270, 530, 350, 554), button_caption="Send")
tmp = out + ".tmp.pdf"
doc.save(tmp)

# Post-process: JS keystroke action, a ResetForm button action and a default value.
with pikepdf.open(tmp) as pdf:
    for a in pdf.pages[0].Annots:
        t = str(a.get("/T", a.get("/Parent", {}).get("/T", "")))
        if t == "js_field":
            a["/AA"] = Dictionary(K=Dictionary(S=Name.JavaScript, JS=String("event.rc = false;")))
        if t == "preset_text":
            a["/DV"] = String("default")
        if t == "reset_btn":
            a["/A"] = Dictionary(S=Name.ResetForm)
        if t == "uri_btn":
            a["/A"] = Dictionary(S=Name.URI, URI=String("https://example.com/from-form"))
        if t == "submit_btn":
            a["/A"] = Dictionary(S=Name.SubmitForm, F=Dictionary(FS=Name.URL, F=String("https://example.com/submit")), Flags=4)

    # Hand-built radio group "rg" (two kids, export values /A and /B), because the generator does not group radios.
    def ap(content, bbox):
        st = pikepdf.Stream(pdf, content)
        st["/Type"] = Name.XObject; st["/Subtype"] = Name.Form; st["/BBox"] = Array(bbox); st["/Resources"] = Dictionary()
        return st
    parent = pdf.make_indirect(Dictionary(FT=Name.Btn, Ff=32768 | 16384, T=String("rg"), V=Name.Off, Kids=Array([])))
    for rect, on in (([72, 200, 92, 220], "/A"), ([120, 200, 140, 220], "/B")):
        kid = pdf.make_indirect(Dictionary(Type=Name.Annot, Subtype=Name.Widget, Rect=Array(rect), Parent=parent, F=4,
                AS=Name.Off, AP=Dictionary(N=Dictionary({on: ap(b"0 g 4 4 12 12 re f", [0, 0, 20, 20]), "/Off": ap(b"0.8 g 0 0 20 20 re f", [0, 0, 20, 20])}))))
        parent.Kids.append(kid)
        pdf.pages[0].Annots.append(kid)
    pdf.Root.AcroForm.Fields.append(parent)
    pdf.save(out)
print("wrote", out)
