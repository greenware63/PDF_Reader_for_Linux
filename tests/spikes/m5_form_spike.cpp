// Milestone M5 spike: what does the pinned PDFium (chromium/8100) do when form events are forwarded to it?
// Developer tool, not part of the product build.
//
// build: g++ -std=c++17 -I<pdfium>/include m5_form_spike.cpp -L<pdfium>/lib -lpdfium -Wl,-rpath,<pdfium>/lib -o m5_form_spike
// usage: m5_form_spike <scenario> <variant: none|inval|full> <form.pdf> <outdir>
// Every scenario runs in its own process so that a crash in one variant cannot hide the others.
#include "fpdf_annot.h"
#include "fpdf_formfill.h"
#include "fpdf_save.h"
#include "fpdfview.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

enum Variant { V_NONE = 0, V_INVAL = 1, V_FULL = 2 };

struct Ctx : FPDF_FORMFILLINFO {
    std::vector<std::array<double, 4>> inval;
    int timers = 0, killTimers = 0, cursor = 0, onChange = 0, textFocus = 0, uri = 0, named = 0;
    std::string lastUri;
};
static Ctx* C(FPDF_FORMFILLINFO* p) { return static_cast<Ctx*>(p); }
static void cbInvalidate(FPDF_FORMFILLINFO* p, FPDF_PAGE, double l, double t, double r, double b) {
    C(p)->inval.push_back({l, t, r, b});
}
static int cbSetTimer(FPDF_FORMFILLINFO* p, int, TimerCallback) { return ++C(p)->timers; }
static void cbKillTimer(FPDF_FORMFILLINFO* p, int) { ++C(p)->killTimers; }
static FPDF_SYSTEMTIME cbLocalTime(FPDF_FORMFILLINFO*) {
    FPDF_SYSTEMTIME t{};
    t.wYear = 2026; t.wMonth = 11; t.wDay = 8;
    return t;
}
static void cbOnChange(FPDF_FORMFILLINFO* p) { ++C(p)->onChange; }
static void cbSetCursor(FPDF_FORMFILLINFO* p, int) { ++C(p)->cursor; }
static void cbDoURI(FPDF_FORMFILLINFO* p, FPDF_BYTESTRING s) { ++C(p)->uri; C(p)->lastUri = s ? s : ""; }
static void cbNamed(FPDF_FORMFILLINFO* p, FPDF_BYTESTRING) { ++C(p)->named; }
static void cbTextFocus(FPDF_FORMFILLINFO* p, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) { ++C(p)->textFocus; }

static std::string utf8(const unsigned short* u, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00); }
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    return o;
}
static std::vector<unsigned short> wide(const std::u16string& s) {
    std::vector<unsigned short> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}
static std::string hexcodes(const std::string& u8) {
    std::string out; char b[16];
    for (size_t i = 0; i < u8.size();) {
        unsigned char c = u8[i]; unsigned cp = c; size_t len = 1;
        if (c >= 0xF0) { cp = c & 7; len = 4; } else if (c >= 0xE0) { cp = c & 15; len = 3; } else if (c >= 0xC0) { cp = c & 31; len = 2; }
        for (size_t k = 1; k < len && i + k < u8.size(); ++k) cp = (cp << 6) | (u8[i + k] & 63);
        std::snprintf(b, sizeof b, "U+%04X ", cp); out += b; i += len;
    }
    return out;
}

struct Doc {
    FPDF_DOCUMENT doc{}; FPDF_FORMHANDLE form{}; FPDF_PAGE page{}; Ctx ctx; double w{}, h{};
    explicit Doc(const std::string& path, Variant v) {
        doc = FPDF_LoadDocument(path.c_str(), nullptr);
        if (!doc) { std::printf("LOAD FAILED\n"); std::exit(2); }
        std::memset(static_cast<FPDF_FORMFILLINFO*>(&ctx), 0, sizeof(FPDF_FORMFILLINFO));
        ctx.version = 1;
        if (v >= V_INVAL) ctx.FFI_Invalidate = cbInvalidate;
        if (v >= V_FULL) {
            ctx.FFI_SetTimer = cbSetTimer; ctx.FFI_KillTimer = cbKillTimer; ctx.FFI_GetLocalTime = cbLocalTime;
            ctx.FFI_OnChange = cbOnChange; ctx.FFI_SetCursor = cbSetCursor; ctx.FFI_DoURIAction = cbDoURI;
            ctx.FFI_ExecuteNamedAction = cbNamed; ctx.FFI_SetTextFieldFocus = cbTextFocus;
        }
        form = FPDFDOC_InitFormFillEnvironment(doc, &ctx);
        FPDF_SetFormFieldHighlightAlpha(form, 0);
        page = FPDF_LoadPage(doc, 0);
        FORM_OnAfterLoadPage(page, form);
        w = FPDF_GetPageWidthF(page); h = FPDF_GetPageHeightF(page);
    }
    ~Doc() {
        FORM_OnBeforeClosePage(page, form); FPDF_ClosePage(page);
        FPDFDOC_ExitFormFillEnvironment(form); FPDF_CloseDocument(doc);
    }
};

struct Field { int idx; std::string name; int type; int flags; FS_RECTF r; double cx, cy; };

static std::string fieldName(Doc& d, FPDF_ANNOTATION a) {
    unsigned long n = FPDFAnnot_GetFormFieldName(d.form, a, nullptr, 0);
    std::vector<unsigned short> b(n / 2 + 1, 0);
    FPDFAnnot_GetFormFieldName(d.form, a, b.data(), n);
    return utf8(b.data(), n > 2 ? n / 2 - 1 : 0);
}
static std::string fieldValue(Doc& d, FPDF_ANNOTATION a) {
    unsigned long n = FPDFAnnot_GetFormFieldValue(d.form, a, nullptr, 0);
    std::vector<unsigned short> b(n / 2 + 1, 0);
    FPDFAnnot_GetFormFieldValue(d.form, a, b.data(), n);
    return utf8(b.data(), n > 2 ? n / 2 - 1 : 0);
}
static std::vector<Field> fields(Doc& d) {
    std::vector<Field> out;
    int n = FPDFPage_GetAnnotCount(d.page);
    for (int i = 0; i < n; ++i) {
        FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, i);
        if (FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET) {
            Field f{}; f.idx = i; f.name = fieldName(d, a); f.type = FPDFAnnot_GetFormFieldType(d.form, a);
            f.flags = FPDFAnnot_GetFormFieldFlags(d.form, a); FPDFAnnot_GetRect(a, &f.r);
            f.cx = (f.r.left + f.r.right) / 2; f.cy = (f.r.top + f.r.bottom) / 2;
            out.push_back(f);
        }
        FPDFPage_CloseAnnot(a);
    }
    return out;
}
static const Field* byName(const std::vector<Field>& fs, const char* name, int nth = 0) {
    for (const auto& f : fs) if (f.name == name && nth-- == 0) return &f;
    std::printf("NO FIELD %s\n", name); std::exit(3);
}
// Reads a value through a fresh annotation handle (the way a field list would).
static std::string valueOf(Doc& d, const Field& f) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f.idx);
    std::string v = fieldValue(d, a);
    FPDFPage_CloseAnnot(a);
    return v;
}
static bool checkedOf(Doc& d, const Field& f) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f.idx);
    bool c = FPDFAnnot_IsChecked(d.form, a);
    FPDFPage_CloseAnnot(a);
    return c;
}
static void click(Doc& d, double x, double y) {
    FORM_OnLButtonDown(d.form, d.page, 0, x, y);
    FORM_OnLButtonUp(d.form, d.page, 0, x, y);
}
static void typeText(Doc& d, const std::u16string& s) {
    for (char16_t c : s) FORM_OnChar(d.form, d.page, c, 0);
}

struct Writer : FPDF_FILEWRITE { std::string data; };
static int writeBlock(FPDF_FILEWRITE* p, const void* data, unsigned long n) {
    static_cast<Writer*>(p)->data.append(static_cast<const char*>(data), n);
    return 1;
}
static bool saveTo(Doc& d, int flags, const std::string& path) {
    Writer w; w.version = 1; w.WriteBlock = writeBlock;
    if (!FPDF_SaveAsCopy(d.doc, &w, flags)) return false;
    std::ofstream(path, std::ios::binary).write(w.data.data(), std::streamsize(w.data.size()));
    return true;
}

struct Img { int w{}, h{}; std::vector<unsigned char> rgb; };
static Img render(Doc& d, bool withForms = true) {
    int W = int(d.w), H = int(d.h);
    FPDF_BITMAP bm = FPDFBitmap_Create(W, H, 0);
    FPDFBitmap_FillRect(bm, 0, 0, W, H, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bm, d.page, 0, 0, W, H, 0, FPDF_ANNOT);
    if (withForms) FPDF_FFLDraw(d.form, bm, d.page, 0, 0, W, H, 0, FPDF_ANNOT);
    Img im; im.w = W; im.h = H; im.rgb.resize(size_t(W) * H * 3);
    const unsigned char* src = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bm));
    int stride = FPDFBitmap_GetStride(bm);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const unsigned char* p = src + size_t(y) * stride + size_t(x) * 4;
        unsigned char* o = &im.rgb[(size_t(y) * W + x) * 3];
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
    }
    FPDFBitmap_Destroy(bm);
    return im;
}
static void writePpm(const Img& im, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << im.w << " " << im.h << "\n255\n";
    f.write(reinterpret_cast<const char*>(im.rgb.data()), std::streamsize(im.rgb.size()));
}
// Number of differing pixels and their bounding box in PAGE coordinates (y up).
static void diff(const char* label, const Img& a, const Img& b, double pageH) {
    long n = 0; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = 0; y < a.h; ++y) for (int x = 0; x < a.w; ++x) {
        const unsigned char* p = &a.rgb[(size_t(y) * a.w + x) * 3]; const unsigned char* q = &b.rgb[(size_t(y) * a.w + x) * 3];
        if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2]) { ++n; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    }
    if (!n) { std::printf("DIFF %-34s: identical\n", label); return; }
    std::printf("DIFF %-34s: %ld px differ, page-space box x[%d..%d] y[%.0f..%.0f]\n", label, n, x0, x1, pageH - y1 - 1, pageH - y0);
}
static void printInval(const char* label, Doc& d, const Field* f = nullptr) {
    std::printf("INVAL %-30s: %zu call(s)", label, d.ctx.inval.size());
    for (size_t i = 0; i < d.ctx.inval.size() && i < 3; ++i) {
        const auto& r = d.ctx.inval[i];
        std::printf("  [l=%.1f t=%.1f r=%.1f b=%.1f]", r[0], r[1], r[2], r[3]);
    }
    if (f) std::printf("   widget rect [l=%.1f t=%.1f r=%.1f b=%.1f]", f->r.left, f->r.top, f->r.right, f->r.bottom);
    std::printf("\n");
    d.ctx.inval.clear();
}

int main(int argc, char** argv) {
    if (argc < 5) { std::printf("usage: %s scenario variant form.pdf outdir\n", argv[0]); return 1; }
    const std::string sc = argv[1], var = argv[2], form = argv[3], out = argv[4];
    const Variant v = var == "none" ? V_NONE : var == "inval" ? V_INVAL : V_FULL;
    FPDF_InitLibrary();
    {
        Doc d(form, v);
        const auto fs = fields(d);
        const std::string tag = sc + "_" + var;

        if (sc == "list") {  // what a "list form fields" call could return without any event
            for (const auto& f : fs) {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f.idx);
                std::printf("FIELD idx=%2d %-12s type=%d flags=0x%05x rect=[%.0f %.0f %.0f %.0f] value='%s'", f.idx, f.name.c_str(), f.type, f.flags,
                            f.r.left, f.r.bottom, f.r.right, f.r.top, fieldValue(d, a).c_str());
                if (f.type == FPDF_FORMFIELD_CHECKBOX || f.type == FPDF_FORMFIELD_RADIOBUTTON) std::printf(" checked=%d", FPDFAnnot_IsChecked(d.form, a));
                int oc = FPDFAnnot_GetOptionCount(d.form, a);
                if (oc > 0) std::printf(" options=%d", oc);
                std::printf("\n");
                FPDFPage_CloseAnnot(a);
            }
        } else if (sc == "type") {  // click, type, commit by killing focus, save incrementally
            const Field* f = byName(fs, "name");
            click(d, f->cx, f->cy);
            printInval("click on text field", d, f);
            typeText(d, u"Hello World");
            printInval("typing 11 chars", d, f);
            std::printf("VALUE before kill-focus: '%s'\n", valueOf(d, *f).c_str());
            FORM_ForceToKillFocus(d.form);
            std::printf("VALUE after  kill-focus: '%s'\n", valueOf(d, *f).c_str());
            printInval("kill focus", d, f);
            std::printf("CALLBACKS timers=%d killTimers=%d cursor=%d onChange=%d textFocus=%d\n", d.ctx.timers, d.ctx.killTimers, d.ctx.cursor, d.ctx.onChange, d.ctx.textFocus);
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "nokill") {  // save without committing the focused field
            const Field* f = byName(fs, "name");
            click(d, f->cx, f->cy); typeText(d, u"Unsaved");
            std::printf("VALUE read back while editing: '%s'\n", valueOf(d, *f).c_str());
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "toggle") {
            const Field* ag = byName(fs, "agree"); const Field* lk = byName(fs, "locked");
            std::printf("agree  checked before=%d flags=0x%x\n", checkedOf(d, *ag), ag->flags);
            click(d, ag->cx, ag->cy); printInval("click checkbox", d, ag);
            std::printf("agree  checked after click=%d\n", checkedOf(d, *ag));
            std::printf("locked checked before=%d flags=0x%x (READONLY=%d)\n", checkedOf(d, *lk), lk->flags, lk->flags & FPDF_FORMFLAG_READONLY);
            click(d, lk->cx, lk->cy);
            std::printf("locked checked after click=%d  -> %s\n", checkedOf(d, *lk), checkedOf(d, *lk) ? "(unchanged, read-only is honoured or it started unchecked)" : "TOGGLED despite read-only");
            const Field* r1 = byName(fs, "rg", 0); const Field* r2 = byName(fs, "rg", 1);
            std::printf("radio rg: A=%d B=%d (before)\n", checkedOf(d, *r1), checkedOf(d, *r2));
            click(d, r2->cx, r2->cy);
            std::printf("radio rg: A=%d B=%d after clicking B\n", checkedOf(d, *r1), checkedOf(d, *r2));
            click(d, r1->cx, r1->cy);
            std::printf("radio rg: A=%d B=%d after clicking A\n", checkedOf(d, *r1), checkedOf(d, *r2));
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "choice") {  // combo and list through FORM_SetIndexSelected (no mouse needed)
            for (const char* nm : {"country", "langs"}) {
                const Field* f = byName(fs, nm);
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f->idx);
                int oc = FPDFAnnot_GetOptionCount(d.form, a);
                std::printf("%s: %d options, value before='%s'\n", nm, oc, valueOf(d, *f).c_str());
                std::printf("%s: FORM_SetFocusedAnnot=%d\n", nm, FORM_SetFocusedAnnot(d.form, a));
                std::printf("%s: FORM_SetIndexSelected(1)=%d\n", nm, FORM_SetIndexSelected(d.form, d.page, 1, true));
                FORM_ForceToKillFocus(d.form);
                FPDFPage_CloseAnnot(a);
                a = FPDFPage_GetAnnot(d.page, f->idx);
                std::printf("%s: value after='%s' selected:", nm, fieldValue(d, a).c_str());
                for (int i = 0; i < oc; ++i) std::printf(" %d", FPDFAnnot_IsOptionSelected(d.form, a, i));
                std::printf("\n");
                FPDFPage_CloseAnnot(a);
            }
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "programmatic") {  // set a value without mouse or keys: focus + select all + replace + commit
            const Field* f = byName(fs, "name");
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f->idx);
            std::printf("FORM_SetFocusedAnnot=%d\n", FORM_SetFocusedAnnot(d.form, a));
            FORM_SelectAllText(d.form, d.page);
            auto w = wide(u"Programmatic value");
            FORM_ReplaceSelection(d.form, d.page, w.data());
            std::printf("CanUndo while editing=%d\n", FORM_CanUndo(d.form, d.page));
            FORM_ForceToKillFocus(d.form);
            std::printf("CanUndo after commit=%d\n", FORM_CanUndo(d.form, d.page));
            FPDFPage_CloseAnnot(a);
            std::printf("VALUE after commit: '%s'\n", valueOf(d, *f).c_str());
            // Replace an existing value (second edit of the same field), as an undo or redo would.
            a = FPDFPage_GetAnnot(d.page, f->idx);
            FORM_SetFocusedAnnot(d.form, a); FORM_SelectAllText(d.form, d.page);
            auto w2 = wide(u"");
            FORM_ReplaceSelection(d.form, d.page, w2.data()); FORM_ForceToKillFocus(d.form);
            FPDFPage_CloseAnnot(a);
            std::printf("VALUE after replacing with empty string (undo to empty): '%s'\n", valueOf(d, *f).c_str());
            a = FPDFPage_GetAnnot(d.page, f->idx);
            FORM_SetFocusedAnnot(d.form, a); FORM_SelectAllText(d.form, d.page);
            auto w3 = wide(u"Restored after undo");
            FORM_ReplaceSelection(d.form, d.page, w3.data()); FORM_ForceToKillFocus(d.form);
            FPDFPage_CloseAnnot(a);
            std::printf("VALUE after restoring: '%s'\n", valueOf(d, *f).c_str());
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "persian_chars" || sc == "persian_replace") {
            const Field* f = byName(fs, "persian");
            const std::u16string fa = u"سلام دنیا";  // "salaam donya"
            if (sc == "persian_chars") { click(d, f->cx, f->cy); typeText(d, fa); FORM_ForceToKillFocus(d.form); }
            else {
                FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f->idx);
                FORM_SetFocusedAnnot(d.form, a); FORM_SelectAllText(d.form, d.page);
                auto w = wide(fa); FORM_ReplaceSelection(d.form, d.page, w.data()); FORM_ForceToKillFocus(d.form);
                FPDFPage_CloseAnnot(a);
            }
            std::string got = valueOf(d, *f);
            std::printf("VALUE read back: '%s'\nCODEPOINTS: %s\n", got.c_str(), hexcodes(got).c_str());
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "reset") {  // does a click on a ResetForm button change field values behind our back?
            const Field* p = byName(fs, "preset_text"); const Field* b = byName(fs, "reset_btn");
            std::printf("preset_text before: '%s'\n", valueOf(d, *p).c_str());
            click(d, b->cx, b->cy);
            std::printf("preset_text after clicking the ResetForm button: '%s'\n", valueOf(d, *p).c_str());
            printInval("reset button click", d);
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "actions") {  // URI and submit buttons
            const Field* u = byName(fs, "uri_btn"); const Field* s = byName(fs, "submit_btn");
            click(d, u->cx, u->cy);
            std::printf("after URI button: uri callbacks=%d lastUri='%s'\n", d.ctx.uri, d.ctx.lastUri.c_str());
            click(d, s->cx, s->cy);
            std::printf("after Submit button: uri callbacks=%d lastUri='%s' named=%d\n", d.ctx.uri, d.ctx.lastUri.c_str(), d.ctx.named);
        } else if (sc == "js") {
            const Field* f = byName(fs, "js_field");
            click(d, f->cx, f->cy); typeText(d, u"abc"); FORM_ForceToKillFocus(d.form);
            std::printf("js_field has a keystroke script 'event.rc = false;' -> value after typing abc: '%s' (%s)\n", valueOf(d, *f).c_str(),
                        valueOf(d, *f) == "abc" ? "script did NOT run" : "script ran");
        } else if (sc == "render") {
            const Field* f = byName(fs, "name"); const Field* c = byName(fs, "country");
            Img base = render(d); writePpm(base, out + "/r_base.ppm");
            click(d, f->cx, f->cy); typeText(d, u"Typing");
            Img editing = render(d); writePpm(editing, out + "/r_editing.ppm");
            diff("base -> editing (uncommitted)", base, editing, d.h);
            printInval("click+type", d, f);
            FORM_ForceToKillFocus(d.form);
            Img committed = render(d); writePpm(committed, out + "/r_committed.ppm");
            diff("editing -> committed", editing, committed, d.h);
            Img noForms = render(d, false);
            diff("committed with vs without FFLDraw", committed, noForms, d.h);
            click(d, c->r.right - 6, c->cy);
            Img open = render(d); writePpm(open, out + "/r_combo_open.ppm");
            diff("committed -> combo dropdown open", committed, open, d.h);
            printInval("click combo arrow", d, c);
            FORM_ForceToKillFocus(d.form);
        } else if (sc == "radio2") {  // radio switching in both directions, with and without a mouse move first
            const Field* r1 = byName(fs, "rg", 0); const Field* r2 = byName(fs, "rg", 1);
            auto st = [&](const char* what) { std::printf("%-34s A=%d B=%d\n", what, checkedOf(d, *r1), checkedOf(d, *r2)); };
            st("start");
            click(d, r1->cx, r1->cy); st("click A");
            click(d, r2->cx, r2->cy); st("click B");
            click(d, r1->cx, r1->cy); st("click A again");
            FORM_OnMouseMove(d.form, d.page, 0, r2->cx, r2->cy); click(d, r2->cx, r2->cy); st("mouse-move + click B");
            FORM_OnMouseMove(d.form, d.page, 0, r1->cx, r1->cy); click(d, r1->cx, r1->cy); st("mouse-move + click A");
            FORM_ForceToKillFocus(d.form); st("after kill focus");
        } else if (sc == "setap") {  // replace the appearance of a text widget with our own content stream
            const Field* f = byName(fs, "persian");
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(d.page, f->idx);
            FORM_SetFocusedAnnot(d.form, a); FORM_SelectAllText(d.form, d.page);
            auto w = wide(u"سلام دنیا"); FORM_ReplaceSelection(d.form, d.page, w.data());
            FORM_ForceToKillFocus(d.form);
            FPDFPage_CloseAnnot(a);
            a = FPDFPage_GetAnnot(d.page, f->idx);
            // page coordinates, because the appearance /BBox is the annotation /Rect: light blue field, black bar inside
            auto ap = wide(u"0.8 0.9 1 rg 72 318 228 24 re f 0 0 0 rg 80 324 60 12 re f");
            std::printf("FPDFAnnot_SetAP(normal)=%d\n", FPDFAnnot_SetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, ap.data()));
            FPDFPage_CloseAnnot(a);
            std::printf("VALUE after SetAP: '%s'\n", hexcodes(valueOf(d, *f)).c_str());
            Img im = render(d); writePpm(im, out + "/r_setap.ppm");
            auto px = [&](double x, double y) { const unsigned char* q = &im.rgb[(size_t(int(d.h - y)) * im.w + size_t(x)) * 3]; std::printf("(%.0f,%.0f)=rgb(%d,%d,%d) ", x, y, q[0], q[1], q[2]); };
            std::printf("in-process FFLDraw pixels: "); px(100, 330); px(250, 320); std::printf("  (expect black bar, then light blue)\n");
            std::printf("SAVE incremental: %d\n", saveTo(d, FPDF_INCREMENTAL, out + "/" + tag + ".pdf"));
        } else if (sc == "actions2") {  // same buttons, but with a mouse move first (what a real UI sends)
            const Field* u = byName(fs, "uri_btn"); const Field* sb = byName(fs, "submit_btn");
            FORM_OnMouseMove(d.form, d.page, 0, u->cx, u->cy); click(d, u->cx, u->cy);
            std::printf("hover+click URI button   : uri callbacks=%d lastUri='%s'\n", d.ctx.uri, d.ctx.lastUri.c_str());
            d.ctx.lastUri.clear();
            FORM_OnMouseMove(d.form, d.page, 0, sb->cx, sb->cy); click(d, sb->cx, sb->cy);
            std::printf("hover+click Submit button: uri callbacks=%d lastUri='%s'\n", d.ctx.uri, d.ctx.lastUri.c_str());
        } else { std::printf("unknown scenario\n"); return 1; }
    }
    FPDF_DestroyLibrary();
    return 0;
}
