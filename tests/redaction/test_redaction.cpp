// Redaction engine test-suite.  What these tests do and do NOT prove: docs/REDACTION_TEST_REPORT.md.
//
// Usage: test_redaction <fixtures-dir>
//
// A literal-string search is NEVER treated as sufficient proof.  Evidence per fixture:
//   E1  independent parse + inflate + allowlist of every object/name/operator (PdfInspector.h)
//   E2  canary search in raw bytes AND in decompressed objects (+ qpdf --qdf expansion)
//   E3  text extraction by PDFium (in-engine) and by Poppler's pdftotext (external)
//   E4  pixel check of the STORED image: every pixel of every box is 0,0,0, and the boxes cover
//       the requested rectangle (independently computed)
//   E5  residual metadata / attachments / signatures / outlines
//   E6  visual: content outside the boxes still present (ink/colour) and matches Poppler's render
//   E7  adversarial/malformed inputs never yield bytes unless verified
//   E8  negative controls: naive overlay and plain rewrite must FAIL the detectors
#include "redact/Redaction.h"
#include "PdfInspector.h"

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

using namespace pdf::redact;
using inspect::Bytes;

namespace {

int g_fail = 0, g_pass = 0;
std::string g_dir;
std::string g_tmp;
std::vector<std::string> g_skipped;

void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; return; }
    ++g_fail;
    std::cerr << "  FAIL: " << what << "\n";
}

Bytes readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void writeFile(const std::string& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

int runCmd(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
bool haveTool(const std::string& t) { std::string o; return runCmd("command -v " + t, o) == 0; }

// ---------------------------------------------------------------- manifest
struct Rc { int page; double l, t, r, b; };
struct Entry {
    std::string file, label, password = "-", status = "Ok";
    std::vector<std::string> canaries;
    std::vector<Rc> rects, ink, blank;
    struct Col { int page, R, G, B, tol, minCount; };
    std::vector<Col> absent, present;
    int sigs = -1, atts = -1;
    bool safeOnly = false, poppler = false;
};

std::vector<Entry> loadManifest() {
    std::vector<Entry> out;
    std::ifstream f(g_dir + "/manifest.txt");
    std::string line;
    Entry* cur = nullptr;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string k; ss >> k;
        if (k == "fixture") { out.emplace_back(); cur = &out.back(); ss >> cur->file; continue; }
        if (!cur) continue;
        if (k == "label") ss >> cur->label;
        else if (k == "password") ss >> cur->password;
        else if (k == "expect_status") ss >> cur->status;
        else if (k == "canary") { std::string c; ss >> c; cur->canaries.push_back(c); }
        else if (k == "rect" || k == "ink" || k == "blank") {
            Rc r; ss >> r.page >> r.l >> r.t >> r.r >> r.b;
            (k == "rect" ? cur->rects : k == "ink" ? cur->ink : cur->blank).push_back(r);
        }
        else if (k == "expect_sigs") ss >> cur->sigs;
        else if (k == "expect_attachments") ss >> cur->atts;
        else if (k == "color_absent") { Entry::Col c{}; ss >> c.page >> c.R >> c.G >> c.B >> c.tol; cur->absent.push_back(c); }
        else if (k == "color_present") { Entry::Col c{}; ss >> c.page >> c.R >> c.G >> c.B >> c.tol >> c.minCount; cur->present.push_back(c); }
        else if (k == "safe_only") cur->safeOnly = true;
        else if (k == "compare_poppler") cur->poppler = true;
    }
    return out;
}

std::vector<PageRedactions> toRedactions(const std::vector<Rc>& rects) {
    std::map<int, PageRedactions> m;
    for (const Rc& r : rects) {
        PageRedactions& pr = m[r.page];
        pr.pageIndex = r.page;
        pr.rects.push_back({r.l, r.t, r.r, r.b});
    }
    std::vector<PageRedactions> v;
    for (auto& kv : m) v.push_back(kv.second);
    return v;
}

// ---------------------------------------------------------------- external witnesses
bool g_haveQpdf = false, g_havePdftotext = false, g_havePdftoppm = false, g_havePdfsig = false;

std::string pdftotextOf(const std::string& path, const std::string& pw = "") {
    std::string out;
    runCmd("pdftotext " + (pw.empty() || pw == "-" ? std::string() : "-upw " + pw + " ") + "'" + path + "' -", out);
    return out;
}
bool hasNonWs(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c) && c != '\f'; });
}

struct Ppm { int w = 0, h = 0; Bytes d; };
bool readPpm(const std::string& p, Ppm& out) {
    Bytes b = readFile(p);
    if (b.size() < 10 || b[0] != 'P' || b[1] != '6') return false;
    size_t i = 2; int vals[3]; int got = 0;
    while (got < 3 && i < b.size()) {
        while (i < b.size() && std::isspace(b[i])) ++i;
        if (b[i] == '#') { while (i < b.size() && b[i] != '\n') ++i; continue; }
        int v = 0;
        while (i < b.size() && std::isdigit(b[i])) v = v * 10 + (b[i++] - '0');
        vals[got++] = v;
    }
    ++i;
    out.w = vals[0]; out.h = vals[1];
    out.d.assign(b.begin() + static_cast<long>(i), b.end());
    return out.d.size() >= static_cast<size_t>(out.w) * out.h * 3;
}

// ---------------------------------------------------------------- PDFium helpers for the CONTROLS only
struct MemW : FPDF_FILEWRITE { Bytes b; };
int memWrite(FPDF_FILEWRITE* s, const void* d, unsigned long n) { auto* w = static_cast<MemW*>(s); auto* p = static_cast<const std::uint8_t*>(d); w->b.insert(w->b.end(), p, p + n); return 1; }

// "Naive overlay": what a careless redactor does -- draw a black rectangle over the content and save.
Bytes naiveOverlay(const Bytes& src, const std::vector<Rc>& rects, double pageHeightPt, bool draw) {
    FPDF_InitLibrary();
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(src.data(), src.size(), nullptr);
    if (!doc) return {};
    if (draw) {
        for (const Rc& r : rects) {
            FPDF_PAGE page = FPDF_LoadPage(doc, r.page);
            FPDF_PAGEOBJECT o = FPDFPageObj_CreateNewRect(static_cast<float>(r.l), static_cast<float>(pageHeightPt - r.b),
                                                          static_cast<float>(r.r - r.l), static_cast<float>(r.b - r.t));
            FPDFPageObj_SetFillColor(o, 0, 0, 0, 255);
            FPDFPath_SetDrawMode(o, FPDF_FILLMODE_ALTERNATE, 0);
            FPDFPage_InsertObject(page, o);
            FPDFPage_GenerateContent(page);
            FPDF_ClosePage(page);
        }
    }
    MemW w; w.version = 1; w.WriteBlock = &memWrite;
    FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL);   // a "full rewrite": old revisions are gone
    FPDF_CloseDocument(doc);
    return w.b;
}

std::string pdfiumTextOf(const Bytes& pdf) {
    std::string all;
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(pdf.data(), pdf.size(), nullptr);
    if (!doc) return all;
    int n = FPDF_GetPageCount(doc);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGE p = FPDF_LoadPage(doc, i);
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(p);
        int c = FPDFText_CountChars(tp);
        std::vector<unsigned short> buf(static_cast<size_t>(c) + 1);
        if (c > 0) FPDFText_GetText(tp, 0, c, buf.data());
        for (int k = 0; k < c; ++k) all.push_back(static_cast<char>(buf[static_cast<size_t>(k)] & 0x7f));
        FPDFText_ClosePage(tp);
        FPDF_ClosePage(p);
    }
    FPDF_CloseDocument(doc);
    return all;
}

// Detectors: each returns true when it FINDS a problem (= the redaction is bad).
struct Detections { bool literalRaw = false, decoded = false, allowlist = false, pdftotext = false, pdfiumText = false; };

Detections detect(const Bytes& out, const std::vector<std::string>& canaries, const std::string& tmpName) {
    Detections d;
    for (const auto& c : canaries) if (inspect::containsBytes(out, c)) d.literalRaw = true;
    inspect::Inspection ins = inspect::inspectOutput(out);
    for (const auto& c : canaries) if (ins.everything.find(c) != std::string::npos) d.decoded = true;
    d.allowlist = !ins.problems.empty();
    std::string pt = pdfiumTextOf(out);
    for (const auto& c : canaries) if (pt.find(c) != std::string::npos) d.pdfiumText = true;
    if (g_havePdftotext) {
        const std::string p = g_tmp + "/" + tmpName;
        writeFile(p, out);
        std::string t = pdftotextOf(p);
        for (const auto& c : canaries) if (t.find(c) != std::string::npos) d.pdftotext = true;
        unlink(p.c_str());
    }
    return d;
}

// ---------------------------------------------------------------- per-fixture verification
void verifyOk(const Entry& e, const Bytes& src, const Result& res, const std::string& name) {
    const Report& rep = res.report;
    check(!res.pdf.empty(), name + ": output present");
    check(res.pdf.size() == rep.outputBytes, name + ": report.outputBytes matches");
    check(!rep.verification.empty(), name + ": verification list present");

    // E1 independent inspection
    inspect::Inspection ins = inspect::inspectOutput(res.pdf);
    for (const auto& p : ins.problems) check(false, name + ": inspector: " + p);
    check(ins.mappedAllPages, name + ": every page maps to exactly one image");
    check(ins.pageCount == rep.pageCount, name + ": page count preserved");
    check(!ins.hasObjStm, name + ": no object streams");

    // E2 canaries: raw + decoded + qpdf expansion
    for (const auto& c : e.canaries) {
        check(!inspect::containsBytes(res.pdf, c), name + ": raw bytes contain " + c);
        check(ins.everything.find(c) == std::string::npos, name + ": decompressed objects contain " + c);
    }
    const std::string tp = g_tmp + "/out_" + std::to_string(::getpid()) + ".pdf";
    writeFile(tp, res.pdf);
    if (g_haveQpdf) {
        std::string o;
        int rc = runCmd("qpdf --check '" + tp + "'", o);
        check(rc == 0, name + ": qpdf --check passes: " + o.substr(0, 200));
        runCmd("qpdf --qdf --object-streams=disable '" + tp + "' '" + tp + ".qdf'", o);
        Bytes q = readFile(tp + ".qdf");
        for (const auto& c : e.canaries) check(!inspect::containsBytes(q, c), name + ": qpdf --qdf expansion contains " + c);
        for (const char* bad : {"/Font", "/Annots", "/AcroForm", "/EmbeddedFile", "/JavaScript", "/Outlines", "/Metadata", "/StructTreeRoot", "/Prev", "BT\n", " Tj", " TJ"})
            check(!inspect::containsBytes(q, bad), name + std::string(": qdf expansion contains ") + bad);
        unlink((tp + ".qdf").c_str());
    }
    // E3 text extraction
    check(pdfiumTextOf(res.pdf).find_first_not_of(" \r\n\t") == std::string::npos, name + ": PDFium text extraction is empty");
    if (g_havePdftotext) check(!hasNonWs(pdftotextOf(tp)), name + ": pdftotext output is empty");
    if (g_havePdfsig) {
        std::string o; runCmd("pdfsig '" + tp + "'", o);
        check(o.find("does not contain any signatures") != std::string::npos, name + ": pdfsig sees no signature");
    }

    // E5 residual metadata: the only /Info content allowed is PDFium's own
    {
        for (const auto& o : ins.objects) {
            if (o.dict.find("/Creator") != std::string::npos) {
                check(o.dict.find("(PDFium)") != std::string::npos, name + ": /Info /Creator is not PDFium's");
                check(o.dict.find("/Title") == std::string::npos && o.dict.find("/Author") == std::string::npos &&
                      o.dict.find("/Subject") == std::string::npos && o.dict.find("/Keywords") == std::string::npos, name + ": /Info carries source fields");
            }
        }
    }
    if (e.sigs >= 0) check(rep.sourceSignatureCount == e.sigs, name + ": source signature count " + std::to_string(rep.sourceSignatureCount));
    if (e.atts >= 0) check(rep.sourceAttachmentCount == e.atts, name + ": source attachment count " + std::to_string(rep.sourceAttachmentCount));
    if (rep.sourceSignatureCount > 0) {
        bool w = false;
        for (const auto& s : rep.warnings) if (s.find("signature") != std::string::npos && s.find("invalidated") != std::string::npos) w = true;
        check(w, name + ": signature-invalidation warning present");
    }

    // E4 pixels of the STORED image, independently decoded
    const double s = rep.dpi / 72.0;
    for (size_t pi = 0; pi < ins.pageImages.size() && pi < rep.pages.size(); ++pi) {
        const auto& img = ins.pageImages[pi];
        const auto& pr = rep.pages[pi];
        check(img.width == pr.widthPx && img.height == pr.heightPx, name + ": stored image size equals report");
        for (const auto& b : pr.boxes) {
            std::string why;
            check(inspect::regionAllBlack(img, b.x0, b.y0, b.x1, b.y1, &why), name + ": page " + std::to_string(pi) + " box not black: " + why);
        }
    }
    // requested rectangles (displayed coordinates) are fully inside black, using inward rounding
    for (const Rc& r : e.rects) {
        if (r.page >= static_cast<int>(ins.pageImages.size())) continue;
        const auto& img = ins.pageImages[static_cast<size_t>(r.page)];
        const double sx = img.width / rep.pages[static_cast<size_t>(r.page)].widthPt;
        const double sy = img.height / rep.pages[static_cast<size_t>(r.page)].heightPt;
        const int x0 = static_cast<int>(std::ceil(std::max(0.0, r.l) * sx)), y0 = static_cast<int>(std::ceil(std::max(0.0, r.t) * sy));
        const int x1 = static_cast<int>(std::floor(r.r * sx)), y1 = static_cast<int>(std::floor(r.b * sy));
        std::string why;
        check(inspect::regionAllBlack(img, x0, y0, x1, y1, &why), name + ": requested rect not fully black on page " + std::to_string(r.page) + ": " + why);
        (void)s;
    }

    // E6 visual
    for (const Rc& r : e.ink) {
        if (r.page >= static_cast<int>(ins.pageImages.size())) continue;
        const auto& img = ins.pageImages[static_cast<size_t>(r.page)];
        const double sx = img.width / rep.pages[static_cast<size_t>(r.page)].widthPt, sy = img.height / rep.pages[static_cast<size_t>(r.page)].heightPt;
        check(inspect::countInk(img, static_cast<int>(r.l * sx), static_cast<int>(r.t * sy), static_cast<int>(r.r * sx), static_cast<int>(r.b * sy)) > 20,
              name + ": expected visible content outside boxes on page " + std::to_string(r.page));
    }
    for (const auto& c : e.absent) {
        const auto& img = ins.pageImages[static_cast<size_t>(c.page)];
        check(inspect::countColor(img, c.R, c.G, c.B, c.tol) == 0, name + ": colour that was redacted is still present on page " + std::to_string(c.page));
    }
    for (const auto& c : e.present) {
        const auto& img = ins.pageImages[static_cast<size_t>(c.page)];
        const long long n = inspect::countColor(img, c.R, c.G, c.B, c.tol);
        check(n >= c.minCount, name + ": expected colour missing on page " + std::to_string(c.page) + " (" + std::to_string(n) + " px)");
    }
    if (e.poppler && g_havePdftoppm) {
        // Cross-engine: render source and output with Poppler and compare outside the (dilated) boxes.
        std::string o;
        const std::string sp = g_tmp + "/src_" + std::to_string(::getpid()) + ".pdf";
        writeFile(sp, src);
        const std::string pw = e.password == "-" ? "" : "-upw " + e.password + " ";
        const std::string dpi = std::to_string(static_cast<int>(rep.dpi));
        const int pagesToCheck = std::min(rep.pageCount, 3);
        for (int pg = 1; pg <= pagesToCheck; ++pg) {
            const std::string a = g_tmp + "/ra_" + std::to_string(::getpid()), b = g_tmp + "/rb_" + std::to_string(::getpid());
            runCmd("pdftoppm -r " + dpi + " -f " + std::to_string(pg) + " -l " + std::to_string(pg) + " -singlefile " + pw + "'" + sp + "' '" + a + "'", o);
            runCmd("pdftoppm -r " + dpi + " -f " + std::to_string(pg) + " -l " + std::to_string(pg) + " -singlefile '" + tp + "' '" + b + "'", o);
            Ppm A, B;
            if (!readPpm(a + ".ppm", A) || !readPpm(b + ".ppm", B)) { check(false, name + ": cannot read pdftoppm output"); continue; }
            unlink((a + ".ppm").c_str()); unlink((b + ".ppm").c_str());
            check(std::abs(A.w - B.w) <= 1 && std::abs(A.h - B.h) <= 1, name + ": poppler render sizes differ");
            const int W = std::min(A.w, B.w), Hh = std::min(A.h, B.h);
            std::vector<char> masked(static_cast<size_t>(W) * Hh, 0);
            for (const auto& bx : rep.pages[static_cast<size_t>(pg - 1)].boxes)
                for (int y = std::max(0, bx.y0 - 6); y < std::min(Hh, bx.y1 + 6); ++y)
                    for (int x = std::max(0, bx.x0 - 6); x < std::min(W, bx.x1 + 6); ++x) masked[static_cast<size_t>(y) * W + x] = 1;
            double sum = 0; long long cnt = 0, big = 0;
            for (int y = 0; y < Hh; ++y)
                for (int x = 0; x < W; ++x) {
                    if (masked[static_cast<size_t>(y) * W + x]) continue;
                    const std::uint8_t* pa = &A.d[(static_cast<size_t>(y) * A.w + x) * 3];
                    const std::uint8_t* pb = &B.d[(static_cast<size_t>(y) * B.w + x) * 3];
                    int d = (std::abs(pa[0] - pb[0]) + std::abs(pa[1] - pb[1]) + std::abs(pa[2] - pb[2])) / 3;
                    sum += d; ++cnt; if (d > 96) ++big;
                }
            const double mean = cnt ? sum / static_cast<double>(cnt) : 0.0;
            const double bigFrac = cnt ? static_cast<double>(big) / static_cast<double>(cnt) : 0.0;
            check(mean < 4.0, name + ": page " + std::to_string(pg) + " outside-box mean diff vs Poppler too high: " + std::to_string(mean));
            check(bigFrac < 0.015, name + ": page " + std::to_string(pg) + " outside-box strong-diff fraction too high: " + std::to_string(bigFrac));
        }
        unlink(sp.c_str());
    }
    unlink(tp.c_str());
}

void runFixtures() {
    std::cout << "[fixtures]\n";
    for (const Entry& e : loadManifest()) {
        const std::string name = e.file + (e.label.empty() ? "" : "#" + e.label);
        const Bytes src = readFile(g_dir + "/" + e.file);
        check(!src.empty(), name + ": fixture readable");
        Options opt;
        const std::string pw = e.password == "-" ? "" : e.password;
        Result res = redactDocument(src.data(), src.size(), pw, toRedactions(e.rects), opt);
        if (e.status == "Any") {
            // hostile input: either a clean refusal with no bytes, or a verified output.
            if (res.ok()) verifyOk(e, src, res, name);
            else check(res.pdf.empty(), name + ": failure must not return bytes");
            std::cout << "  " << name << ": " << toString(res.status) << " (safe either way)\n";
            continue;
        }
        check(std::string(toString(res.status)) == e.status, name + ": status " + toString(res.status) + " expected " + e.status + " (" + res.message + ")");
        if (e.status != "Ok") { check(res.pdf.empty(), name + ": failure must not return bytes"); std::cout << "  " << name << ": " << toString(res.status) << "\n"; continue; }
        verifyOk(e, src, res, name);
        std::cout << "  " << name << ": ok, " << res.pdf.size() << " bytes\n";
    }
}

// ---------------------------------------------------------------- API abuse
void runApiAbuse() {
    std::cout << "[api abuse]\n";
    const Bytes basic = readFile(g_dir + "/basic.pdf");
    const std::vector<PageRedactions> good = {{0, {{60, 124, 400, 152}}}};
    auto expect = [&](const char* label, Result r, Status s) {
        check(r.status == s, std::string(label) + ": got " + toString(r.status) + " expected " + toString(s) + " (" + r.message + ")");
        if (!r.ok()) check(r.pdf.empty(), std::string(label) + ": failure returned bytes");
    };
    expect("null data", redactDocument(nullptr, 10, "", good), Status::InvalidInput);
    expect("zero size", redactDocument(basic.data(), 0, "", good), Status::InvalidInput);
    { Options o; o.limits.maxInputBytes = 100; expect("input over limit", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    for (double d : std::initializer_list<double>{0.0, 71.9, 300.1, -5.0, std::nan(""), static_cast<double>(INFINITY)}) { Options o; o.dpi = d; expect("bad dpi", redactDocument(basic.data(), basic.size(), "", good, o), Status::InvalidInput); }
    for (int g : {-1, 17, 1 << 30}) { Options o; o.edgeGrowPx = g; expect("bad edgeGrow", redactDocument(basic.data(), basic.size(), "", good, o), Status::InvalidInput); }
    expect("no redactions", redactDocument(basic.data(), basic.size(), "", {}), Status::NothingToRedact);
    expect("empty rect list", redactDocument(basic.data(), basic.size(), "", {{0, {}}}), Status::NothingToRedact);
    expect("negative page", redactDocument(basic.data(), basic.size(), "", {{-1, {{1, 1, 5, 5}}}}), Status::PageOutOfRange);
    expect("page == count", redactDocument(basic.data(), basic.size(), "", {{2, {{1, 1, 5, 5}}}}), Status::PageOutOfRange);
    expect("page INT_MAX", redactDocument(basic.data(), basic.size(), "", {{2147483647, {{1, 1, 5, 5}}}}), Status::PageOutOfRange);
    const double nan = std::nan(""), inf = INFINITY;
    for (const Rect& r : {Rect{nan, 1, 5, 5}, Rect{1, nan, 5, 5}, Rect{1, 1, nan, 5}, Rect{1, 1, 5, nan}, Rect{-inf, 1, 5, 5}, Rect{1, 1, inf, 5},
                          Rect{10, 10, 5, 20}, Rect{10, 10, 20, 5}, Rect{10, 10, 10, 20}, Rect{10, 10, 20, 10}, Rect{0, 0, 1e9, 1e9}, Rect{-1e9, 0, 5, 5}})
        expect("bad rect", redactDocument(basic.data(), basic.size(), "", {{0, {r}}}), Status::InvalidRect);
    expect("fully off page (right)", redactDocument(basic.data(), basic.size(), "", {{0, {{700, 10, 800, 50}}}}), Status::InvalidRect);
    expect("fully off page (above)", redactDocument(basic.data(), basic.size(), "", {{0, {{10, -90, 50, -10}}}}), Status::InvalidRect);
    { Options o; o.limits.maxPages = 1; expect("too many pages", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    { Options o; o.limits.maxPixelsPerPage = 1000; expect("pixel limit", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    { Options o; o.limits.maxTotalPixels = 1000; expect("total pixel limit", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    { Options o; o.limits.maxDimensionPx = 100; expect("dimension limit", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    { Options o; o.limits.maxOutputBytes = 1000; expect("output limit", redactDocument(basic.data(), basic.size(), "", good, o), Status::LimitExceeded); }
    { Options o; o.limits.maxRectsPerPage = 1; expect("rect limit", redactDocument(basic.data(), basic.size(), "", {{0, {{1, 1, 5, 5}, {6, 6, 9, 9}}}}, o), Status::LimitExceeded); }
    { Options o; o.limits.maxTotalRects = 1; expect("total rect limit", redactDocument(basic.data(), basic.size(), "", {{0, {{1, 1, 5, 5}}}, {1, {{1, 1, 5, 5}}}}, o), Status::LimitExceeded); }

    // Valid edge cases must still be safe and correct.
    auto okAndBlack = [&](const char* label, const std::vector<PageRedactions>& rr, Options o = {}) {
        Result r = redactDocument(basic.data(), basic.size(), "", rr, o);
        check(r.ok(), std::string(label) + ": expected Ok, got " + toString(r.status) + " " + r.message);
        if (!r.ok()) return;
        inspect::Inspection ins = inspect::inspectOutput(r.pdf);
        check(ins.problems.empty() && ins.mappedAllPages, std::string(label) + ": inspector clean");
        for (size_t i = 0; i < r.report.pages.size() && i < ins.pageImages.size(); ++i)
            for (const auto& b : r.report.pages[i].boxes) check(inspect::regionAllBlack(ins.pageImages[i], b.x0, b.y0, b.x1, b.y1), std::string(label) + ": box black");
    };
    okAndBlack("partially off page", {{0, {{-50, -50, 100, 100}, {500, 700, 900, 900}}}});
    okAndBlack("whole page", {{0, {{0, 0, 612, 792}}}, {1, {{-10, -10, 700, 900}}}});
    okAndBlack("tiny rect", {{0, {{100, 100, 100.001, 100.001}}}});
    okAndBlack("overlapping", {{0, {{10, 10, 200, 200}, {100, 100, 300, 300}}}});
    okAndBlack("same page twice", {{0, {{10, 10, 20, 20}}}, {0, {{300, 300, 400, 400}}}});
    { Options o; o.dpi = 72; okAndBlack("dpi 72", good, o); }
    { Options o; o.dpi = 300; okAndBlack("dpi 300", good, o); }
    { Options o; o.edgeGrowPx = 0; okAndBlack("grow 0", good, o); }
    { Options o; o.edgeGrowPx = 16; okAndBlack("grow 16", good, o); }

    // the tiny rect must still yield a box that is at least 1px + 2*grow wide
    {
        Result r = redactDocument(basic.data(), basic.size(), "", {{0, {{100, 100, 100.001, 100.001}}}});
        if (r.ok()) { const auto& b = r.report.pages[0].boxes[0]; check(b.x1 - b.x0 >= 3 && b.y1 - b.y0 >= 3, "tiny rect: box at least 3px"); }
    }
    // box grows outward: with edgeGrowPx the box strictly contains the rounded rect
    {
        Options o; o.edgeGrowPx = 2;
        Result r = redactDocument(basic.data(), basic.size(), "", {{0, {{72, 72, 144, 144}}}}, o);   // exactly 150,150 .. 300,300 px
        check(r.ok() && r.report.pages[0].boxes[0].x0 == 148 && r.report.pages[0].boxes[0].x1 == 302, "edge grow arithmetic (148..302)");
    }
    // second pass over our own output
    {
        Result r1 = redactDocument(basic.data(), basic.size(), "", good);
        check(r1.ok(), "first pass ok");
        if (r1.ok()) {
            Result r2 = redactDocument(r1.pdf.data(), r1.pdf.size(), "", {{0, {{300, 300, 400, 400}}}});
            check(r2.ok(), "re-redacting an output works");
            if (r2.ok()) {
                inspect::Inspection a = inspect::inspectOutput(r1.pdf), b = inspect::inspectOutput(r2.pdf);
                check(inspect::regionAllBlack(a.pageImages[0], 125, 258, 800, 316) , "first-pass box black");   // 60..400 x 124..152 pt @150dpi inward
                check(b.problems.empty(), "second-pass output clean");
            }
        }
    }
    // determinism of the pixel content (PDFium adds a timestamp, so compare images not bytes)
    {
        Result a = redactDocument(basic.data(), basic.size(), "", good), b = redactDocument(basic.data(), basic.size(), "", good);
        if (a.ok() && b.ok()) {
            auto ia = inspect::inspectOutput(a.pdf), ib = inspect::inspectOutput(b.pdf);
            check(ia.pageImages.size() == ib.pageImages.size() && ia.pageImages[0].rgb == ib.pageImages[0].rgb && ia.pageImages[1].rgb == ib.pageImages[1].rgb, "pixel output deterministic");
        } else check(false, "determinism: runs failed");
    }
    // concurrency through the process-wide PDFium mutex
    {
        std::vector<std::thread> ts; std::vector<int> okc(4, 0);
        for (int i = 0; i < 4; ++i) ts.emplace_back([&, i] { Options o; o.dpi = 100; Result r = redactDocument(basic.data(), basic.size(), "", good, o); okc[static_cast<size_t>(i)] = r.ok(); });
        for (auto& t : ts) t.join();
        check(std::all_of(okc.begin(), okc.end(), [](int v) { return v == 1; }), "4 concurrent redactions all succeed");
    }
}

// ---------------------------------------------------------------- negative controls and detector teeth
void runControls() {
    std::cout << "[negative controls]\n";
    for (const char* f : {"basic.pdf", "hidden_content.pdf", "annots_forms.pdf", "attachments.pdf", "metadata_all.pdf", "metadata_objstm.pdf", "incremental.pdf"}) {
        Entry e;
        for (const Entry& m : loadManifest()) if (m.file == f && m.label.empty()) e = m;
        const Bytes src = readFile(g_dir + "/" + f);
        // CONTROL 1: naive overlay (black rectangle drawn on top, content kept) + full rewrite
        Bytes naive = naiveOverlay(src, e.rects, 792, true);
        // CONTROL 2: plain full rewrite without any overlay
        Bytes rewrite = naiveOverlay(src, e.rects, 792, false);
        Detections dn = detect(naive, e.canaries, "ctl_naive.pdf");
        Detections dr = detect(rewrite, e.canaries, "ctl_rewrite.pdf");
        std::cout << "  " << f << ": naive-overlay detectors  literalRaw=" << dn.literalRaw << " decoded=" << dn.decoded << " allowlist=" << dn.allowlist
                  << " pdfium=" << dn.pdfiumText << " pdftotext=" << dn.pdftotext << "\n";
        std::cout << "  " << f << ": plain-rewrite detectors  literalRaw=" << dr.literalRaw << " decoded=" << dr.decoded << " allowlist=" << dr.allowlist
                  << " pdfium=" << dr.pdfiumText << " pdftotext=" << dr.pdftotext << "\n";
        // A literal grep would have PASSED a failed redaction for these fixtures (content is compressed):
        if (std::string(f) == "basic.pdf" || std::string(f) == "incremental.pdf") {
            check(!dn.literalRaw && dn.decoded, std::string(f) + ": naive overlay passes a literal grep but the canary is recoverable from decoded streams");
        }
        // The structural detectors (allowlist) and decoded search must flag every weak method.
        check(dn.allowlist, std::string(f) + ": allowlist detector flags naive overlay");
        check(dr.allowlist, std::string(f) + ": allowlist detector flags plain rewrite");
        check(dn.decoded, std::string(f) + ": decoded-search flags naive overlay (canaries survive under the box)");
        // A "rewrite" is NOT redaction: text is still extractable (where the fixture has rendered canary text).
        if (std::string(f) == "basic.pdf" || std::string(f) == "attachments.pdf") {
            check(dn.pdfiumText && dr.pdfiumText, std::string(f) + ": PDFium text extraction flags overlay and rewrite");
            if (g_havePdftotext) check(dn.pdftotext && dr.pdftotext, std::string(f) + ": pdftotext flags overlay and rewrite");
        }
        // The engine output for the same input must trip NONE of them.
        Result r = redactDocument(src.data(), src.size(), "", toRedactions(e.rects));
        check(r.ok(), std::string(f) + ": engine ok");
        if (r.ok()) {
            Detections de = detect(r.pdf, e.canaries, "ctl_engine.pdf");
            check(!de.literalRaw && !de.decoded && !de.allowlist && !de.pdfiumText && !de.pdftotext, std::string(f) + ": engine output trips no detector");
        }
    }

    // A literal grep is demonstrably insufficient: on the object-stream fixture the canaries are
    // invisible to a raw search yet fully recoverable.
    {
        const Bytes src = readFile(g_dir + "/metadata_objstm.pdf");
        bool rawHit = inspect::containsBytes(src, "CANARY-OUTLINE-TITLE");
        FPDF_InitLibrary();
        Bytes rewritten = naiveOverlay(src, {{0, 60, 120, 300, 150}}, 792, true);
        Detections d = detect(rewritten, {"CANARY-META-VISIBLE", "CANARY-OUTLINE-TITLE", "CANARY-INFO-TITLE"}, "ctl_objstm.pdf");
        std::cout << "  grep-insufficiency (objstm source): raw-grep-hit=" << rawHit << "; after naive rewrite literalRaw=" << d.literalRaw << " decoded=" << d.decoded << " pdfium=" << d.pdfiumText << "\n";
        check(!rawHit, "objstm source hides canaries from a literal grep (precondition)");
        check(d.decoded || d.pdfiumText || d.allowlist, "a structural/decoded detector still flags the naive rewrite that a literal grep would pass");
        if (!d.literalRaw) check(d.allowlist, "literal grep passes the naive rewrite but allowlist catches it");
    }

    // Progress heartbeat: a host (the sandboxed worker) uses Options::onProgress to prove a long redaction
    // is alive. It must fire per page in every phase, stay monotonic, and keep firing during serialisation.
    std::cout << "[progress heartbeat]\n";
    {
        const Bytes multi = readFile(g_dir + "/multipage.pdf");
        std::vector<std::pair<int, int>> calls;
        Options o;
        o.onProgress = [&](int done, int total) { calls.emplace_back(done, total); };
        Result r = redactDocument(multi.data(), multi.size(), "", {{0, {{90, 132, 210, 202}}}}, o);
        check(r.ok(), "progress: redaction with a callback succeeds");
        const int pages = r.ok() ? r.report.pageCount : 30;
        check(pages == 30, "progress: fixture page count is 30");
        bool monotonic = !calls.empty();
        bool constantTotal = !calls.empty();
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (i > 0 && calls[i].first < calls[i - 1].first) monotonic = false;
            if (calls[i].second != calls.front().second) constantTotal = false;
        }
        int distinctSteps = 0;
        for (std::size_t i = 0; i < calls.size(); ++i)
            if (i == 0 || calls[i].first != calls[i - 1].first) ++distinctSteps;
        check(monotonic, "progress: `done` never decreases");
        check(constantTotal && !calls.empty() && calls.front().second == 3 * pages, "progress: total is 3 x pageCount (plan, render, verify)");
        check(distinctSteps == 3 * pages && !calls.empty() && calls.back().first == 3 * pages, "progress: exactly one step per page per phase, ending at total");
        check(static_cast<int>(calls.size()) > 3 * pages, "progress: callback also fires during serialisation (more calls than steps)");

        // A throwing callback must fail closed (no output), never escape or yield a half-built file.
        Options t;
        t.onProgress = [](int done, int) { if (done >= 5) throw std::runtime_error("boom"); };
        Result tr = redactDocument(multi.data(), multi.size(), "", {{0, {{90, 132, 210, 202}}}}, t);
        check(!tr.ok() && tr.pdf.empty(), "progress: a throwing callback fails the redaction closed with no output");
        // Throw only once serialisation has begun: `done` rests at 2 x pageCount (render finished) while the
        // output is written, so the first few callbacks at that value are the writer's heartbeat.
        int writeBeats = 0;
        Options t3;
        t3.onProgress = [&](int done, int total) {
            if (done == 2 * (total / 3) && ++writeBeats > 3) throw std::runtime_error("boom during write");
        };
        Result wr = redactDocument(multi.data(), multi.size(), "", {{0, {{90, 132, 210, 202}}}}, t3);
        check(!wr.ok() && wr.pdf.empty(), "progress: a callback throwing mid-serialisation fails closed with no output");

        // Output is unaffected by having a callback.
        Result plain = redactDocument(multi.data(), multi.size(), "", {{0, {{90, 132, 210, 202}}}});
        check(plain.ok() && r.ok() && plain.report.pageCount == r.report.pageCount && plain.report.pages[0].boxes.size() == r.report.pages[0].boxes.size(),
              "progress: report is identical with and without a callback");
    }

    // Teeth: the checkers must fail when the engine's guarantees are broken.
    std::cout << "[detector teeth]\n";
    {
        const Bytes basic = readFile(g_dir + "/basic.pdf");
        Result r = redactDocument(basic.data(), basic.size(), "", {{0, {{60, 124, 400, 152}}}});
        check(r.ok(), "teeth: base run ok");
        if (r.ok()) {
            // (a) enlarge a claimed box onto non-black pixels: both the engine's verifier and ours must object
            auto pages = r.report.pages;
            pages[0].boxes[0].x1 += 400;
            check(!verifyRedactedOutput(r.pdf, pages).empty(), "verifyRedactedOutput rejects a box covering non-black pixels");
            auto ins = inspect::inspectOutput(r.pdf);
            check(!inspect::regionAllBlack(ins.pageImages[0], pages[0].boxes[0].x0, pages[0].boxes[0].y0, pages[0].boxes[0].x1, pages[0].boxes[0].y1), "inspector pixel check rejects the same box");
            // (b) injecting forbidden structure must be caught by both
            for (const char* inj : {"/Annots", "/Names", "/Outlines", "/OpenAction", "/Metadata", "/Prev 0"}) {
                Bytes m = r.pdf;
                const std::string cat = "/Catalog";
                auto it = std::search(m.begin(), m.end(), cat.begin(), cat.end());
                check(it != m.end(), "teeth: catalog found");
                if (it == m.end()) continue;
                const std::string add = std::string(" ") + inj;
                m.insert(it + static_cast<long>(cat.size()), add.begin(), add.end());
                check(!inspect::inspectOutput(m).problems.empty(), std::string("inspector rejects injected ") + inj);
                check(!verifyRedactedOutput(m, r.report.pages).empty(), std::string("verifyRedactedOutput rejects injected ") + inj);
            }
            // (c) a second revision appended after %%EOF
            {
                Bytes m = r.pdf; const std::string tail = "\nxref\n0 1\n0000000000 65535 f \ntrailer\n<< /Size 1 >>\nstartxref\n0\n%%EOF\n";
                m.insert(m.end(), tail.begin(), tail.end());
                check(!inspect::inspectOutput(m).problems.empty(), "inspector rejects an appended revision");
                check(!verifyRedactedOutput(m, r.report.pages).empty(), "verifyRedactedOutput rejects an appended revision");
            }
            // (d) a text operator smuggled into the content stream (recompress with BT .. Tj .. ET)
            {
                auto objs = inspect::scanObjects(r.pdf);
                bool done = false;
                for (const auto& o : objs) {
                    if (!o.hasStream || !o.decodedOk || o.dict.find("/Image") != std::string::npos || done) continue;
                    std::string content(o.decoded.begin(), o.decoded.end());
                    content += "\nBT /F1 12 Tf 10 10 Td (CANARY-SMUGGLED) Tj ET\n";
                    Bytes z(compressBound(static_cast<uLong>(content.size())));
                    uLongf zl = static_cast<uLongf>(z.size());
                    compress(z.data(), &zl, reinterpret_cast<const Bytef*>(content.data()), static_cast<uLong>(content.size()));
                    z.resize(zl);
                    // rebuild: replace the object body bytes in place
                    std::string s(r.pdf.begin(), r.pdf.end());
                    size_t pos = s.find(std::string(o.raw.begin(), o.raw.end()));
                    if (pos == std::string::npos) continue;
                    Bytes m(r.pdf.begin(), r.pdf.begin() + static_cast<long>(pos));
                    m.insert(m.end(), z.begin(), z.end());
                    m.insert(m.end(), r.pdf.begin() + static_cast<long>(pos + o.raw.size()), r.pdf.end());
                    // fix /Length textually in the dictionary just before
                    const std::string oldLen = "/Length " + std::to_string(o.raw.size());
                    std::string ms(m.begin(), m.end());
                    size_t lp = ms.rfind(oldLen, pos);
                    if (lp == std::string::npos) continue;
                    ms.replace(lp, oldLen.size(), "/Length " + std::to_string(z.size()));
                    Bytes mb(ms.begin(), ms.end());
                    auto ins2 = inspect::inspectOutput(mb);
                    bool flagged = false;
                    for (const auto& p : ins2.problems) if (p.find("operator") != std::string::npos) flagged = true;
                    check(flagged, "inspector rejects a smuggled text operator in a page content stream");
                    check(ins2.everything.find("CANARY-SMUGGLED") != std::string::npos, "decoded search finds the smuggled canary (a raw grep would not)");
                    check(!inspect::containsBytes(mb, "CANARY-SMUGGLED"), "smuggled canary is invisible to a raw grep");
                    done = true;
                }
                check(done, "teeth: content-stream mutation was exercised");
            }
        }
    }
}

// ---------------------------------------------------------------- mutation fuzz (invariant-based)
void runFuzz() {
    std::cout << "[mutation fuzz]\n";
    int okCount = 0, refused = 0;
    for (const char* f : {"basic.pdf", "annots_forms.pdf", "metadata_all.pdf", "hidden_content.pdf", "signed.pdf"}) {
        Entry e;
        for (const Entry& m : loadManifest()) if (m.file == f && m.label.empty()) e = m;
        const Bytes base = readFile(g_dir + "/" + f);
        std::mt19937 rng(0xC0FFEE ^ static_cast<unsigned>(base.size()));
        for (int it = 0; it < 120; ++it) {
            Bytes m = base;
            const int edits = 1 + static_cast<int>(rng() % 4);
            for (int k = 0; k < edits; ++k) {
                const size_t p = rng() % m.size();
                switch (rng() % 3) {
                case 0: m[p] = static_cast<std::uint8_t>(rng()); break;
                case 1: m[p] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
                default: m.erase(m.begin() + static_cast<long>(p), m.begin() + static_cast<long>(std::min(m.size(), p + 1 + rng() % 16))); if (m.empty()) m.push_back(0); break;
                }
            }
            Options o; o.dpi = 72;
            Result r = redactDocument(m.data(), m.size(), "", toRedactions(e.rects), o);
            if (!r.ok()) { ++refused; check(r.pdf.empty(), std::string("fuzz ") + f + ": failure returned bytes"); continue; }
            ++okCount;
            auto ins = inspect::inspectOutput(r.pdf);
            bool clean = ins.problems.empty() && ins.mappedAllPages;
            for (size_t i = 0; clean && i < ins.pageImages.size() && i < r.report.pages.size(); ++i)
                for (const auto& b : r.report.pages[i].boxes) clean = clean && inspect::regionAllBlack(ins.pageImages[i], b.x0, b.y0, b.x1, b.y1);
            check(clean, std::string("fuzz ") + f + " iteration " + std::to_string(it) + ": output violates the invariants");
            if (!clean) break;
        }
    }
    std::cout << "  mutated inputs: ok=" << okCount << " refused=" << refused << "\n";
    check(okCount > 0 && refused > 0, "fuzz: both accepted and refused cases occurred");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: test_redaction <fixtures-dir>\n"; return 2; }
    g_dir = argv[1];
    g_tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/redact_test_" + std::to_string(::getpid());
    { std::string o; runCmd("mkdir -p '" + g_tmp + "'", o); }
    g_haveQpdf = haveTool("qpdf"); g_havePdftotext = haveTool("pdftotext"); g_havePdftoppm = haveTool("pdftoppm"); g_havePdfsig = haveTool("pdfsig");
    for (auto [ok, n] : {std::pair{g_haveQpdf, "qpdf"}, {g_havePdftotext, "pdftotext"}, {g_havePdftoppm, "pdftoppm"}, {g_havePdfsig, "pdfsig"}})
        if (!ok) { g_skipped.push_back(n); }
    std::cout << "external witnesses:";
    for (auto [ok, n] : {std::pair{g_haveQpdf, "qpdf"}, {g_havePdftotext, "pdftotext"}, {g_havePdftoppm, "pdftoppm"}, {g_havePdfsig, "pdfsig"}}) std::cout << " " << n << (ok ? "=yes" : "=MISSING(skipped)");
    std::cout << "\n";

    if (const char* req = std::getenv("REDACT_REQUIRE_WITNESSES"); req && std::string(req) == "1" && !g_skipped.empty()) {
        std::cerr << "FATAL: REDACT_REQUIRE_WITNESSES=1 is set, but the following external witnesses are missing:\n";
        for (const auto& m : g_skipped) std::cerr << "  - " << m << "\n";
        return 1;
    }

    FPDF_InitLibrary();
    runApiAbuse();
    runFixtures();
    runControls();
    runFuzz();

    { std::string o; runCmd("rm -rf '" + g_tmp + "'", o); }
    std::cout << "\nchecks passed: " << g_pass << ", failed: " << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
