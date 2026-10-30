import ctypes as C, sys, os, subprocess, json, hashlib
# Milestone M0 spike (developer tool, not built or shipped).
# usage: python3 -I m0_annotation_spike.py <libpdfium.so (chromium/8100)> <input.pdf> <outdir>
# Needs: qpdf, pdftoppm, Pillow. Prints JSON per subtype/mode.
lib = C.CDLL(sys.argv[1])
src = sys.argv[2]; out = sys.argv[3]
P=C.c_void_p
lib.FPDF_InitLibrary()
lib.FPDF_LoadDocument.restype=P; lib.FPDF_LoadDocument.argtypes=[C.c_char_p,C.c_char_p]
lib.FPDF_LoadPage.restype=P; lib.FPDF_LoadPage.argtypes=[P,C.c_int]
lib.FPDFPage_CreateAnnot.restype=P; lib.FPDFPage_CreateAnnot.argtypes=[P,C.c_int]
class RectF(C.Structure): _fields_=[('left',C.c_float),('top',C.c_float),('right',C.c_float),('bottom',C.c_float)]
lib.FPDFAnnot_SetRect.argtypes=[P,C.POINTER(RectF)]
lib.FPDFAnnot_SetAP.argtypes=[P,C.c_int,C.c_char_p]
lib.FPDFAnnot_SetAP.restype=C.c_int
WB=C.CFUNCTYPE(C.c_int,P,C.c_void_p,C.c_ulong)
class FW(C.Structure): _fields_=[('version',C.c_int),('WriteBlock',WB)]
lib.FPDF_SaveAsCopy.argtypes=[P,C.POINTER(FW),C.c_ulong]
lib.FPDF_GetPageHeightF.restype=C.c_float; lib.FPDF_GetPageHeightF.argtypes=[P]
lib.FPDF_GetPageWidthF.restype=C.c_float; lib.FPDF_GetPageWidthF.argtypes=[P]
SUB={'text':1,'link':2,'freetext':3,'square':5,'circle':6,'highlight':9,'underline':10,'squiggly':11,'strikeout':12,'stamp':13,'ink':15,'popup':16,'fileattachment':17}
def utf16(s): return s.encode('utf-16-le')+b'\0\0'
res={}
for name,st in SUB.items():
    for mode,flag in (('inc',1),('full',2)):
        doc=lib.FPDF_LoadDocument(src.encode(),None); page=lib.FPDF_LoadPage(doc,0)
        a=lib.FPDFPage_CreateAnnot(page,st)
        ok_create=bool(a)
        r=RectF(100,560,250,500); lib.FPDFAnnot_SetRect(a,C.byref(r))
        if st in (9,10,11,12):
            class Q(C.Structure): _fields_=[(n,C.c_float) for n in ('x1','y1','x2','y2','x3','y3','x4','y4')]
            lib.FPDFAnnot_AppendAttachmentPoints.argtypes=[P,C.POINTER(Q)]
            lib.FPDFAnnot_AppendAttachmentPoints(a,C.byref(Q(100,560,250,560,100,500,250,500)))
        lib.FPDFAnnot_SetColor.argtypes=[P,C.c_int,C.c_uint,C.c_uint,C.c_uint,C.c_uint]
        lib.FPDFAnnot_SetColor(a,0,255,0,0,255)
        ap=utf16("1 0 0 rg 100 500 150 60 re f")
        ok_ap=lib.FPDFAnnot_SetAP(a,0,ap)
        buf=bytearray()
        def wb(self,data,size,buf=buf):
            buf.extend(C.string_at(data,size)); return 1
        fw=FW(1,WB(wb))
        ok_save=lib.FPDF_SaveAsCopy(doc,C.byref(fw),flag)
        path=f'{out}/{name}_{mode}.pdf'; open(path,'wb').write(buf)
        orig=open(src,'rb').read()
        q=subprocess.run(['qpdf','--check',path],capture_output=True,text=True)
        qok = q.returncode in (0,3) and 'No syntax or stream encoding errors' in q.stdout
        subprocess.run(['pdftoppm','-r','72','-f','1','-l','1','-png','-singlefile',path,path[:-4]],capture_output=True)
        from PIL import Image
        im=Image.open(path[:-4]+'.png').convert('RGB')
        H=im.height; red=0
        # rect x 100..250, PDF y 500..560 -> image y = H - 560 .. H - 500 (page height H at 72dpi)
        for x in range(100,250,5):
            for y in range(int(H-560),int(H-500),5):
                px=im.getpixel((x,y))
                if px[0]>200 and px[1]<60 and px[2]<60: red+=1
        res[f'{name}/{mode}']=dict(create=ok_create,ap=bool(ok_ap),save=bool(ok_save),prefix=bytes(buf[:len(orig)])==orig,qpdf_ok=qok,qpdf_rc=q.returncode,poppler_red_samples=red,size=len(buf),has_AP=b'/AP' in bytes(buf[len(orig):] if mode=='inc' else buf))
        lib.FPDF_ClosePage(page); lib.FPDF_CloseDocument(doc)
print(json.dumps(res,indent=0))
