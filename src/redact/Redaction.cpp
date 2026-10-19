// Burn-in redaction engine. Normative behaviour: docs/REDACTION.md.
//
// The output is built from an EMPTY document. The only data that crosses from the source
// into the output is a pixel array whose redacted regions were overwritten with black
// before it was attached to the new page.

#include "redact/Redaction.h"

#include "engine/PdfiumMutex.h"

#include <fpdf_attachment.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_annot.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pdf::redact {

const char* toString(Status s) {
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidInput: return "InvalidInput";
    case Status::InvalidFormat: return "InvalidFormat";
    case Status::PasswordRequired: return "PasswordRequired";
    case Status::BadPassword: return "BadPassword";
    case Status::InvalidRect: return "InvalidRect";
    case Status::PageOutOfRange: return "PageOutOfRange";
    case Status::NothingToRedact: return "NothingToRedact";
    case Status::LimitExceeded: return "LimitExceeded";
    case Status::RenderFailed: return "RenderFailed";
    case Status::BuildFailed: return "BuildFailed";
    case Status::VerificationFailed: return "VerificationFailed";
    case Status::InternalError: return "InternalError";
    }
    return "Unknown";
}

namespace {

// ---------------------------------------------------------------- RAII helpers
struct DocDeleter { void operator()(void* d) const { if (d) FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(d)); } };
struct PageDeleter { void operator()(void* p) const { if (p) FPDF_ClosePage(static_cast<FPDF_PAGE>(p)); } };
struct BitmapDeleter {
    void operator()(void* b) const {
        if (!b) return;
        // Best-effort scrub of pixels we own (L8). PDFium-internal copies are out of our reach.
        auto bmp = static_cast<FPDF_BITMAP>(b);
        if (void* buf = FPDFBitmap_GetBuffer(bmp)) {
            std::memset(buf, 0, static_cast<size_t>(FPDFBitmap_GetStride(bmp)) * FPDFBitmap_GetHeight(bmp));
        }
        FPDFBitmap_Destroy(bmp);
    }
};
struct FormDeleter { void operator()(void* f) const { if (f) FPDFDOC_ExitFormFillEnvironment(static_cast<FPDF_FORMHANDLE>(f)); } };
struct TextPageDeleter { void operator()(void* t) const { if (t) FPDFText_ClosePage(static_cast<FPDF_TEXTPAGE>(t)); } };

using DocPtr = std::unique_ptr<void, DocDeleter>;
using PagePtr = std::unique_ptr<void, PageDeleter>;
using BitmapPtr = std::unique_ptr<void, BitmapDeleter>;
using TextPagePtr = std::unique_ptr<void, TextPageDeleter>;
using FormPtr = std::unique_ptr<void, FormDeleter>;

struct MemoryWriter : FPDF_FILEWRITE {
    std::vector<std::uint8_t> bytes;
    std::size_t cap = 0;
    bool overflow = false;
};

int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<MemoryWriter*>(self);
    if (w->bytes.size() + size > w->cap) { w->overflow = true; return 0; }
    try {
        const auto* p = static_cast<const std::uint8_t*>(data);
        w->bytes.insert(w->bytes.end(), p, p + size);
    } catch (...) { return 0; }
    return 1;
}

Result fail(Status s, std::string msg) {
    Result r;
    r.status = s;
    r.message = std::move(msg);
    return r; // pdf stays empty (G9)
}

bool finite(double v) { return std::isfinite(v); }

// ---------------------------------------------------------------- output structure scan (G7/G8)
// Allowlist from docs/REDACTION.md section 6.
const std::set<std::string>& allowedNames() {
    static const std::set<std::string> k = {
        "Type", "Catalog", "Pages", "Page", "Kids", "Count", "Parent", "MediaBox", "Resources",
        "XObject", "Image", "Subtype", "Width", "Height", "BitsPerComponent", "ColorSpace",
        "DeviceRGB", "DeviceGray", "Filter", "FlateDecode", "Length", "Contents", "Rotate",
        "ExtGState", "ca", "CA", "BM", "Normal", "Info", "Creator", "CreationDate", "ID",
        "Root", "Size", "FXE1", "FXX1", "ProcSet", "PDF", "ImageC", "Producer"};
    return k;
}

bool isNameDelimiter(unsigned char c) {
    return c <= ' ' || std::strchr("()<>[]{}/%", c) != nullptr;
}

// Returns empty string on success.
std::string scanStructure(const std::vector<std::uint8_t>& pdf) {
    if (pdf.size() < 8 || std::memcmp(pdf.data(), "%PDF-", 5) != 0) return "output does not start with %PDF-";

    // Split into "outside streams" text; skip stream bodies using their direct /Length.
    std::string outside;
    outside.reserve(pdf.size() < (1u << 20) ? pdf.size() : (1u << 20));
    const std::string_view all(reinterpret_cast<const char*>(pdf.data()), pdf.size());
    std::size_t pos = 0;
    long long lastLength = -1;
    while (pos < all.size()) {
        const std::size_t kw = all.find("stream", pos);
        if (kw == std::string_view::npos) { outside.append(all.substr(pos)); break; }
        // "endstream" is also matched by find("stream"); skip it, it is plain text outside streams.
        const bool isEnd = kw >= 3 && all.substr(kw - 3, 3) == "end";
        const std::size_t after = kw + 6;
        const bool eolOk = after < all.size() && (all[after] == '\n' || all[after] == '\r');
        if (isEnd || !eolOk) { outside.append(all.substr(pos, kw + 6 - pos)); pos = kw + 6; continue; }
        // Text up to and including the keyword, then find the most recent /Length in the dictionary.
        outside.append(all.substr(pos, kw + 6 - pos));
        const std::string_view dict = all.substr(pos, kw - pos);
        const std::size_t lp = dict.rfind("/Length");
        lastLength = -1;
        if (lp != std::string_view::npos) {
            std::size_t q = lp + 7;
            while (q < dict.size() && dict[q] == ' ') ++q;
            long long v = 0; bool any = false;
            while (q < dict.size() && dict[q] >= '0' && dict[q] <= '9') {
                v = v * 10 + (dict[q] - '0'); any = true;
                if (v > static_cast<long long>(all.size())) break;
                ++q;
            }
            // A trailing "0 R" would mean an indirect length; reject it below.
            if (any) {
                std::size_t r = q;
                while (r < dict.size() && dict[r] == ' ') ++r;
                const bool indirect = r + 2 < dict.size() + 1 && r < dict.size() && dict[r] >= '0' && dict[r] <= '9';
                if (!indirect) lastLength = v;
            }
        }
        if (lastLength < 0) return "stream with missing or indirect /Length: cannot be verified";
        std::size_t body = after;
        if (all[body] == '\r') ++body;
        if (body < all.size() && all[body] == '\n') ++body;
        if (static_cast<unsigned long long>(lastLength) > all.size() - body) return "stream /Length exceeds file";
        std::size_t end = body + static_cast<std::size_t>(lastLength);
        while (end < all.size() && (all[end] == '\r' || all[end] == '\n' || all[end] == ' ')) ++end;
        if (all.substr(end, 9) != "endstream") return "stream not terminated by endstream at /Length";
        pos = end; // continue scanning at "endstream" (plain text)
    }

    // Names
    for (std::size_t i = 0; i < outside.size(); ++i) {
        if (outside[i] != '/') continue;
        std::size_t j = i + 1;
        while (j < outside.size() && !isNameDelimiter(static_cast<unsigned char>(outside[j]))) ++j;
        const std::string name = outside.substr(i + 1, j - i - 1);
        if (!name.empty() && allowedNames().count(name) == 0) return "name outside allowlist: /" + name;
        i = j - 1;
    }
    auto count = [&](const char* needle) {
        std::size_t n = 0, p = 0;
        const std::string nd(needle);
        while ((p = outside.find(nd, p)) != std::string::npos) { ++n; p += nd.size(); }
        return n;
    };
    if (count("startxref") != 1) return "expected exactly one startxref (obsolete revision present?)";
    if (count("%%EOF") != 1) return "expected exactly one %%EOF";
    if (count("trailer") > 1) return "more than one trailer";
    return {};
}

std::string pdfiumMeta(FPDF_DOCUMENT doc, const char* tag) {
    const unsigned long n = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (n <= 2) return {};
    std::vector<char> buf(n);
    FPDF_GetMetaText(doc, tag, buf.data(), n);
    return std::string(buf.data(), n);
}

} // namespace

// ---------------------------------------------------------------- self-verification
std::string verifyRedactedOutput(const std::vector<std::uint8_t>& pdf, const std::vector<PageReport>& pages) {
    if (std::string s = scanStructure(pdf); !s.empty()) return "structure: " + s;

    std::lock_guard<std::recursive_mutex> lock(pdf::getPdfiumMutex());
    DocPtr doc(FPDF_LoadMemDocument64(pdf.data(), pdf.size(), nullptr));
    if (!doc) return "output cannot be re-opened";
    FPDF_DOCUMENT d = static_cast<FPDF_DOCUMENT>(doc.get());

    if (FPDF_GetPageCount(d) != static_cast<int>(pages.size())) return "page count mismatch";
    if (FPDF_GetSecurityHandlerRevision(d) != -1) return "output is encrypted";
    if (FPDF_GetSignatureCount(d) > 0) return "output contains a signature";
    if (FPDFDoc_GetAttachmentCount(d) != 0) return "output contains attachments";
    if (FPDFBookmark_GetFirstChild(d, nullptr) != nullptr) return "output contains outline entries";
    for (const char* tag : {"Title", "Author", "Subject", "Keywords"}) {
        if (!pdfiumMeta(d, tag).empty()) return std::string("output has metadata ") + tag;
    }

    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PageReport& pr = pages[i];
        PagePtr page(FPDF_LoadPage(d, static_cast<int>(i)));
        if (!page) return "cannot load output page " + std::to_string(i);
        FPDF_PAGE p = static_cast<FPDF_PAGE>(page.get());
        if (FPDFPage_GetAnnotCount(p) != 0) return "output page has annotations";
        if (FPDFPage_GetRotation(p) != 0) return "output page rotated";
        if (FPDFPage_CountObjects(p) != 1) return "output page does not contain exactly one object";
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(p, 0);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) return "output page object is not an image";
        {
            TextPagePtr tp(FPDFText_LoadPage(p));
            if (tp && FPDFText_CountChars(static_cast<FPDF_TEXTPAGE>(tp.get())) != 0) return "output page has text";
        }

        FPDF_IMAGEOBJ_METADATA md{};
        if (!FPDFImageObj_GetImageMetadata(obj, p, &md)) return "no image metadata";
        if (static_cast<int>(md.width) != pr.widthPx || static_cast<int>(md.height) != pr.heightPx)
            return "stored image size differs from plan";
        if (md.bits_per_pixel != 24) return "stored image is not 24 bpp";

        // The decisive check: the bytes actually stored in the file, after filter decoding.
        const unsigned long need = FPDFImageObj_GetImageDataDecoded(obj, nullptr, 0);
        const unsigned long long expected = 3ull * md.width * md.height;
        if (need != expected) return "decoded image size is not width*height*3";
        std::vector<std::uint8_t> px(need);
        if (FPDFImageObj_GetImageDataDecoded(obj, px.data(), need) != need) return "cannot decode stored image";
        for (const PixelBox& b : pr.boxes) {
            for (int y = b.y0; y < b.y1; ++y) {
                const std::uint8_t* row = px.data() + (static_cast<std::size_t>(y) * md.width + b.x0) * 3;
                for (int x = 0; x < (b.x1 - b.x0) * 3; ++x) {
                    if (row[x] != 0) return "non-black pixel inside a redaction box in the stored image";
                }
            }
        }
        std::fill(px.begin(), px.end(), 0);
    }
    return {};
}

// ---------------------------------------------------------------- main entry
Result redactDocument(const std::uint8_t* data, std::size_t size, const std::string& password,
                      const std::vector<PageRedactions>& redactions, const Options& opt) {
    try {
        const Limits& lim = opt.limits;

        // ---- 1. validate everything before touching PDFium
        if (!data || size == 0) return fail(Status::InvalidInput, "empty input");
        if (size > lim.maxInputBytes) return fail(Status::LimitExceeded, "input larger than limit");
        if (!finite(opt.dpi) || opt.dpi < 72.0 || opt.dpi > 300.0) return fail(Status::InvalidInput, "dpi out of range [72,300]");
        if (opt.edgeGrowPx < 0 || opt.edgeGrowPx > 16) return fail(Status::InvalidInput, "edgeGrowPx out of range [0,16]");
        if (redactions.empty()) return fail(Status::NothingToRedact, "no redactions supplied");

        std::size_t totalRects = 0;
        for (const PageRedactions& pr : redactions) {
            if (pr.pageIndex < 0) return fail(Status::PageOutOfRange, "negative page index");
            if (pr.rects.empty()) return fail(Status::NothingToRedact, "page entry without rectangles");
            if (pr.rects.size() > lim.maxRectsPerPage) return fail(Status::LimitExceeded, "too many rectangles on a page");
            totalRects += pr.rects.size();
            if (totalRects > lim.maxTotalRects) return fail(Status::LimitExceeded, "too many rectangles");
            for (const Rect& r : pr.rects) {
                if (!finite(r.left) || !finite(r.top) || !finite(r.right) || !finite(r.bottom))
                    return fail(Status::InvalidRect, "non-finite rectangle coordinate");
                constexpr double kMax = 1e7;
                if (std::fabs(r.left) > kMax || std::fabs(r.top) > kMax || std::fabs(r.right) > kMax || std::fabs(r.bottom) > kMax)
                    return fail(Status::InvalidRect, "rectangle coordinate magnitude too large");
                if (!(r.right > r.left) || !(r.bottom > r.top))
                    return fail(Status::InvalidRect, "inverted or zero-area rectangle");
            }
        }

        std::lock_guard<std::recursive_mutex> lock(pdf::getPdfiumMutex());
        FPDF_InitLibrary(); // idempotent in PDFium

        // ---- 2. open source
        DocPtr src(FPDF_LoadMemDocument64(data, size, password.empty() ? nullptr : password.c_str()));
        if (!src) {
            switch (FPDF_GetLastError()) {
            case FPDF_ERR_PASSWORD:
                return fail(password.empty() ? Status::PasswordRequired : Status::BadPassword, "password required or incorrect");
            default:
                return fail(Status::InvalidFormat, "cannot open input as PDF");
            }
        }
        FPDF_DOCUMENT sd = static_cast<FPDF_DOCUMENT>(src.get());

        // Form-fill environment: PDFium only draws widget (form field) appearances through FPDF_FFLDraw.
        // No JavaScript platform is installed and field highlighting is disabled, so nothing executes
        // and no tint is added. Declared after `src` so it is destroyed before the document closes.
        FPDF_FORMFILLINFO ffi{};
        ffi.version = 1;
        FormPtr form(FPDFDOC_InitFormFillEnvironment(sd, &ffi));
        if (form) {
            FPDF_SetFormFieldHighlightAlpha(static_cast<FPDF_FORMHANDLE>(form.get()), 0);
        }
        FPDF_FORMHANDLE fh = static_cast<FPDF_FORMHANDLE>(form.get());

        const int pageCount = FPDF_GetPageCount(sd);
        if (pageCount <= 0) return fail(Status::InvalidFormat, "document has no pages");
        if (pageCount > lim.maxPages) return fail(Status::LimitExceeded, "too many pages");
        for (const PageRedactions& pr : redactions) {
            if (pr.pageIndex >= pageCount) return fail(Status::PageOutOfRange, "redaction page index out of range");
        }

        Result res;
        Report& rep = res.report;
        rep.pageCount = pageCount;
        rep.dpi = opt.dpi;
        rep.sourceWasEncrypted = FPDF_GetSecurityHandlerRevision(sd) != -1;
        const int sigs = FPDF_GetSignatureCount(sd);
        rep.sourceSignatureCount = sigs > 0 ? sigs : 0;
        const int atts = FPDFDoc_GetAttachmentCount(sd);
        rep.sourceAttachmentCount = atts > 0 ? atts : 0;

        // ---- 3. plan: geometry and limits for ALL pages before any large allocation
        const double scale = opt.dpi / 72.0;
        rep.pages.resize(static_cast<std::size_t>(pageCount));
        std::uint64_t totalPixels = 0;
        for (int i = 0; i < pageCount; ++i) {
            PagePtr page(FPDF_LoadPage(sd, i));
            if (!page) return fail(Status::RenderFailed, "cannot load source page " + std::to_string(i));
            FPDF_PAGE p = static_cast<FPDF_PAGE>(page.get());
            const double wpt = FPDF_GetPageWidthF(p);
            const double hpt = FPDF_GetPageHeightF(p);
            if (!finite(wpt) || !finite(hpt) || wpt < 1.0 || hpt < 1.0)
                return fail(Status::RenderFailed, "page " + std::to_string(i) + " has invalid size");
            const double wpxD = std::round(wpt * scale);
            const double hpxD = std::round(hpt * scale);
            if (wpxD > lim.maxDimensionPx || hpxD > lim.maxDimensionPx)
                return fail(Status::LimitExceeded, "page " + std::to_string(i) + " exceeds max pixel dimension");
            const int wpx = std::max(1, static_cast<int>(wpxD));
            const int hpx = std::max(1, static_cast<int>(hpxD));
            const std::uint64_t px = static_cast<std::uint64_t>(wpx) * static_cast<std::uint64_t>(hpx);
            if (px > lim.maxPixelsPerPage) return fail(Status::LimitExceeded, "page " + std::to_string(i) + " exceeds max pixels");
            totalPixels += px;
            if (totalPixels > lim.maxTotalPixels) return fail(Status::LimitExceeded, "document exceeds max total pixels");
            PageReport& pr = rep.pages[static_cast<std::size_t>(i)];
            pr.pageIndex = i;
            pr.widthPt = wpt; pr.heightPt = hpt; pr.widthPx = wpx; pr.heightPx = hpx;
            const int ac = FPDFPage_GetAnnotCount(p);
            pr.sourceAnnotationCount = ac > 0 ? ac : 0;
        }

        // Convert rectangles to pixel boxes (outward rounding + edgeGrowPx), validating against the page.
        for (const PageRedactions& entry : redactions) {
            PageReport& pr = rep.pages[static_cast<std::size_t>(entry.pageIndex)];
            const double sx = pr.widthPx / pr.widthPt;
            const double sy = pr.heightPx / pr.heightPt;
            for (const Rect& r : entry.rects) {
                if (r.right <= 0.0 || r.bottom <= 0.0 || r.left >= pr.widthPt || r.top >= pr.heightPt)
                    return fail(Status::InvalidRect, "rectangle lies entirely outside page " + std::to_string(entry.pageIndex));
                long long x0 = static_cast<long long>(std::floor(r.left * sx)) - opt.edgeGrowPx;
                long long y0 = static_cast<long long>(std::floor(r.top * sy)) - opt.edgeGrowPx;
                long long x1 = static_cast<long long>(std::ceil(r.right * sx)) + opt.edgeGrowPx;
                long long y1 = static_cast<long long>(std::ceil(r.bottom * sy)) + opt.edgeGrowPx;
                x0 = std::max<long long>(0, x0); y0 = std::max<long long>(0, y0);
                x1 = std::min<long long>(pr.widthPx, x1); y1 = std::min<long long>(pr.heightPx, y1);
                if (x1 <= x0 || y1 <= y0) return fail(Status::InvalidRect, "rectangle collapses to nothing after clamping");
                pr.boxes.push_back({static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1)});
            }
        }

        // ---- 4. build output from an EMPTY document (G7)
        DocPtr out(FPDF_CreateNewDocument());
        if (!out) return fail(Status::BuildFailed, "cannot create output document");
        FPDF_DOCUMENT od = static_cast<FPDF_DOCUMENT>(out.get());

        for (int i = 0; i < pageCount; ++i) {
            PageReport& pr = rep.pages[static_cast<std::size_t>(i)];
            PagePtr page(FPDF_LoadPage(sd, i));
            if (!page) return fail(Status::RenderFailed, "cannot load source page " + std::to_string(i));
            FPDF_PAGE p = static_cast<FPDF_PAGE>(page.get());

            BitmapPtr bmp(FPDFBitmap_CreateEx(pr.widthPx, pr.heightPx, FPDFBitmap_BGR, nullptr, 0));
            if (!bmp) return fail(Status::RenderFailed, "cannot allocate bitmap");
            FPDF_BITMAP b = static_cast<FPDF_BITMAP>(bmp.get());
            if (!FPDFBitmap_FillRect(b, 0, 0, pr.widthPx, pr.heightPx, 0xFFFFFFFFu))
                return fail(Status::RenderFailed, "cannot clear bitmap");

            // Visible annotation appearances are burned in with the page; the annotation objects are not copied (G3).
            if (fh) FORM_OnAfterLoadPage(p, fh);
            FPDF_RenderPageBitmap(b, p, 0, 0, pr.widthPx, pr.heightPx, 0, FPDF_ANNOT);
            if (fh) {
                FPDF_FFLDraw(fh, b, p, 0, 0, pr.widthPx, pr.heightPx, 0, FPDF_ANNOT);
                FORM_OnBeforeClosePage(p, fh);
            }

            // Destroy the pixels BEFORE the bitmap goes anywhere near the writer (G1).
            for (const PixelBox& bx : pr.boxes) {
                if (!FPDFBitmap_FillRect(b, bx.x0, bx.y0, bx.x1 - bx.x0, bx.y1 - bx.y0, 0xFF000000u))
                    return fail(Status::RenderFailed, "cannot paint redaction box");
            }

            FPDF_PAGE np = FPDFPage_New(od, i, pr.widthPt, pr.heightPt);
            if (!np) return fail(Status::BuildFailed, "cannot create output page");
            PagePtr newPage(np);
            FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(od);
            if (!img) return fail(Status::BuildFailed, "cannot create image object");
            if (!FPDFImageObj_SetBitmap(nullptr, 0, img, b)) {
                FPDFPageObj_Destroy(img);
                return fail(Status::BuildFailed, "cannot attach bitmap");
            }
            if (!FPDFImageObj_SetMatrix(img, pr.widthPt, 0, 0, pr.heightPt, 0, 0)) {
                FPDFPageObj_Destroy(img);
                return fail(Status::BuildFailed, "cannot position image");
            }
            FPDFPage_InsertObject(np, img); // page owns img from here
            if (!FPDFPage_GenerateContent(np)) return fail(Status::BuildFailed, "cannot generate page content");
        }

        // ---- 5. serialize as a fresh, non-incremental file
        MemoryWriter w;
        w.version = 1;
        w.WriteBlock = &writeBlock;
        w.cap = lim.maxOutputBytes;
        const bool saved = FPDF_SaveAsCopy(od, &w, FPDF_NO_INCREMENTAL);
        // PDFium can report success after a short write, so the overflow flag is authoritative.
        if (w.overflow) return fail(Status::LimitExceeded, "output larger than limit");
        if (!saved) return fail(Status::BuildFailed, "cannot serialize output");
        out.reset();
        src.reset();

        // ---- 6. fail-closed self-verification (G9)
        std::string v = verifyRedactedOutput(w.bytes, rep.pages);
        if (!v.empty()) return fail(Status::VerificationFailed, v);
        rep.verification = {
            "exactly one revision (one startxref/%%EOF, no /Prev)",
            "all dictionary names within the output allowlist; stream lengths consistent",
            "no text, annotations, attachments, outlines, signatures, encryption or source metadata (PDFium view)",
            "one 24-bpp image per page; every pixel of every redaction box is zero in the stored (decoded) image"};

        rep.outputBytes = w.bytes.size();
        if (rep.sourceSignatureCount > 0)
            rep.warnings.push_back("The source contained " + std::to_string(rep.sourceSignatureCount) +
                " digital signature(s). Redaction creates a new file: all signatures are invalidated and removed. "
                "The output is NOT signed.");
        if (rep.sourceWasEncrypted)
            rep.warnings.push_back("The source was encrypted; the output is NOT encrypted.");
        rep.warnings.push_back("Output is raster-only: text is not selectable, searchable or accessible.");
        rep.warnings.push_back("Only content covered by the supplied rectangles is removed; everything else visible remains visible.");
        rep.warnings.push_back("Output contains PDFium-generated /Creator and /CreationDate (time of redaction).");

        res.pdf = std::move(w.bytes);
        res.status = Status::Ok;
        res.message = "ok";
        return res;
    } catch (const std::bad_alloc&) {
        return fail(Status::LimitExceeded, "out of memory");
    } catch (const std::exception& e) {
        return fail(Status::InternalError, e.what());
    } catch (...) {
        return fail(Status::InternalError, "unknown error");
    }
}

} // namespace pdf::redact
