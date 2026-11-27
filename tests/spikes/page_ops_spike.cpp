// Page Organization Spike (T43): verify pinned PDFium (chromium/8100) page-manipulation APIs
// against outlines, named destinations, page labels, /Link annotations, AcroForm fields,
// encrypted documents, and signed documents. Developer tool, not part of the product build.
//
// usage: page_ops_spike <outdir> [signed.pdf] [encrypted.pdf] [enc_password]

#include "fpdf_annot.h"
#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_formfill.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdf_signature.h"
#include "fpdf_transformpage.h"
#include "fpdfview.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::string utf16ToUtf8(const unsigned short* buf, unsigned long bytes) {
    if (!buf || bytes < 2) return {};
    size_t n = bytes / 2;
    if (n > 0 && buf[n - 1] == 0) --n;
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = buf[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            c = 0x10000 + ((c - 0xD800) << 10) + (buf[++i] - 0xDC00);
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

struct FdWriter : FPDF_FILEWRITE {
    int fd = -1;
    bool ok = true;
};

int writeBlockToFd(FPDF_FILEWRITE* pThis, const void* pData, unsigned long size) {
    auto* w = static_cast<FdWriter*>(pThis);
    const auto* ptr = static_cast<const char*>(pData);
    unsigned long remaining = size;
    while (remaining > 0) {
#ifdef _WIN32
        int written = _write(w->fd, ptr, static_cast<unsigned int>(remaining));
#else
        ssize_t written = ::write(w->fd, ptr, remaining);
#endif
        if (written <= 0) {
            w->ok = false;
            return 0;
        }
        ptr += written;
        remaining -= static_cast<unsigned long>(written);
    }
    return 1;
}

bool saveDocToPathViaFd(FPDF_DOCUMENT doc, const std::string& path, FPDF_DWORD flags) {
#ifdef _WIN32
    int fd = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (fd < 0) return false;
    FdWriter writer{};
    writer.version = 1;
    writer.WriteBlock = &writeBlockToFd;
    writer.fd = fd;
    FPDF_BOOL saved = FPDF_SaveAsCopy(doc, &writer, flags);
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
    return saved && writer.ok;
}

// Build a valid 3-page PDF with outlines, named destinations, page labels, /Link annotations, and AcroForm fields.
bool writeRichPagesFixture(const std::string& path) {
    // Object map:
    // 1: Catalog
    // 2: Pages
    // 3: Page 0
    // 4: Page 1
    // 5: Page 2
    // 6: Font F1 (Helvetica)
    // 7: Content Page 0
    // 8: Content Page 1
    // 9: Content Page 2
    // 10: Outlines root
    // 11: Outline 1 -> Page 0
    // 12: Outline 2 -> Page 1
    // 13: Outline 3 -> Page 2
    // 14: Names dict (/Dests 15)
    // 15: Dests name tree
    // 16: PageLabels number tree
    // 17: Link annot on Page 0 -> direct Page 1
    // 18: Link annot on Page 0 -> named dest_p3 (via GoTo)
    // 19: AcroForm dict
    // 20: Widget/Field field_p1 on Page 0
    // 21: Widget/Field field_p2 on Page 1
    std::vector<std::string> objs(22);
    objs[1] = "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R /Names 14 0 R /PageLabels 16 0 R /AcroForm 19 0 R >>";
    objs[2] = "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>";
    objs[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents 7 0 R /Annots [17 0 R 18 0 R 20 0 R] >>";
    objs[4] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents 8 0 R /Annots [21 0 R] >>";
    objs[5] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents 9 0 R >>";
    objs[6] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";

    auto makeStream = [](const std::string& s) {
        std::ostringstream os;
        os << "<< /Length " << s.size() << " >>\nstream\n" << s << "\nendstream";
        return os.str();
    };
    objs[7] = makeStream("BT /F1 18 Tf 72 720 Td (Page One) Tj ET");
    objs[8] = makeStream("BT /F1 18 Tf 72 720 Td (Page Two) Tj ET");
    objs[9] = makeStream("BT /F1 18 Tf 72 720 Td (Page Three) Tj ET");

    objs[10] = "<< /Type /Outlines /First 11 0 R /Last 13 0 R /Count 3 >>";
    objs[11] = "<< /Title (Bookmark P1) /Parent 10 0 R /Next 12 0 R /Dest [3 0 R /Fit] >>";
    objs[12] = "<< /Title (Bookmark P2) /Parent 10 0 R /Prev 11 0 R /Next 13 0 R /Dest [4 0 R /Fit] >>";
    objs[13] = "<< /Title (Bookmark P3) /Parent 10 0 R /Prev 12 0 R /Dest [5 0 R /Fit] >>";

    objs[14] = "<< /Dests 15 0 R >>";
    objs[15] = "<< /Names [(dest_p1) [3 0 R /Fit] (dest_p2) [4 0 R /Fit] (dest_p3) [5 0 R /Fit]] >>";

    objs[16] = "<< /Nums [0 << /S /D >> 1 << /S /R >> 2 << /S /r >>] >>";

    objs[17] = "<< /Type /Annot /Subtype /Link /Rect [72 600 200 630] /Border [0 0 0] /Dest [4 0 R /Fit] >>";
    objs[18] = "<< /Type /Annot /Subtype /Link /Rect [72 550 200 580] /Border [0 0 0] /A << /S /GoTo /D (dest_p3) >> >>";

    objs[19] = "<< /Fields [20 0 R 21 0 R] /DR << /Font << /F1 6 0 R >> >> /DA (/F1 12 Tf 0 g) >>";
    objs[20] = "<< /Type /Annot /Subtype /Widget /FT /Tx /T (field_p1) /V (val1) /Rect [72 450 250 480] /P 3 0 R >>";
    objs[21] = "<< /Type /Annot /Subtype /Widget /FT /Tx /T (field_p2) /V (val2) /Rect [72 450 250 480] /P 4 0 R >>";

    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<long> offsets(objs.size(), 0);
    for (size_t i = 1; i < objs.size(); ++i) {
        offsets[i] = static_cast<long>(pdf.str().size());
        pdf << i << " 0 obj\n" << objs[i] << "\nendobj\n";
    }
    long xrefPos = static_cast<long>(pdf.str().size());
    pdf << "xref\n0 " << objs.size() << "\n0000000000 65535 f \n";
    for (size_t i = 1; i < objs.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010ld 00000 n \n", offsets[i]);
        pdf << buf;
    }
    pdf << "trailer\n<< /Size " << objs.size() << " /Root 1 0 R >>\nstartxref\n" << xrefPos << "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out << pdf.str();
    return out.good();
}

bool writeTwoPageMergeSource(const std::string& path) {
    FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
    if (!doc) return false;
    FPDF_PAGE p0 = FPDFPage_New(doc, 0, 612, 792);
    FPDF_PAGE p1 = FPDFPage_New(doc, 1, 612, 792);
    FPDF_ClosePage(p0);
    FPDF_ClosePage(p1);
    bool ok = saveDocToPathViaFd(doc, path, 0);
    FPDF_CloseDocument(doc);
    return ok;
}

void inspectPdf(const std::string& label, const std::string& path, const char* password = nullptr) {
    FPDF_DOCUMENT doc = FPDF_LoadDocument(path.c_str(), password);
    if (!doc) {
        std::cout << "[" << label << "] FAILED to load " << path << " (err=" << FPDF_GetLastError() << ")\n";
        return;
    }
    int pageCount = FPDF_GetPageCount(doc);
    int sigCount = FPDF_GetSignatureCount(doc);
    std::cout << "[" << label << "] pages=" << pageCount << " sigs=" << sigCount;

    // Labels
    std::cout << " labels=[";
    for (int i = 0; i < pageCount; ++i) {
        unsigned short buf[64]{};
        unsigned long bytes = FPDF_GetPageLabel(doc, i, buf, sizeof(buf));
        if (i > 0) std::cout << ",";
        std::cout << utf16ToUtf8(buf, bytes);
    }
    std::cout << "]";

    // Outlines
    std::cout << " outlines=[";
    FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, nullptr);
    bool firstBm = true;
    while (bm) {
        unsigned short title[128]{};
        unsigned long tbytes = FPDFBookmark_GetTitle(bm, title, sizeof(title));
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
        int destPage = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -99;
        if (!firstBm) std::cout << ",";
        firstBm = false;
        std::cout << utf16ToUtf8(title, tbytes) << "->p" << destPage;
        bm = FPDFBookmark_GetNextSibling(doc, bm);
    }
    std::cout << "]";

    // Named destinations
    unsigned long ndCount = FPDF_CountNamedDests(doc);
    std::cout << " namedDests=" << ndCount << "[";
    for (unsigned long i = 0; i < ndCount; ++i) {
        long buflen = 0;
        FPDF_GetNamedDest(doc, static_cast<int>(i), nullptr, &buflen);
        std::vector<char> nameBuf(static_cast<size_t>(buflen) + 2, 0);
        FPDF_DEST dest = FPDF_GetNamedDest(doc, static_cast<int>(i), nameBuf.data(), &buflen);
        int destPage = dest ? FPDFDest_GetDestPageIndex(doc, dest) : -99;
        if (i > 0) std::cout << ",";
        std::cout << utf16ToUtf8(reinterpret_cast<const unsigned short*>(nameBuf.data()), static_cast<unsigned long>(buflen))
                  << "->p" << destPage;
    }
    std::cout << "]";

    // Page 0 links, rotation, cropbox, form fields
    if (pageCount > 0) {
        FPDF_PAGE p0 = FPDF_LoadPage(doc, 0);
        if (p0) {
            int rot = FPDFPage_GetRotation(p0);
            float l = 0, b = 0, r = 0, t = 0;
            bool hasCrop = FPDFPage_GetCropBox(p0, &l, &b, &r, &t);
            std::cout << " p0_rot=" << rot << " p0_crop=" << (hasCrop ? 1 : 0)
                      << "(" << l << "," << b << "," << r << "," << t << ")";

            int annotCount = FPDFPage_GetAnnotCount(p0);
            std::cout << " p0_annots=" << annotCount << " p0_links=[";
            bool firstLink = true;
            for (int a = 0; a < annotCount; ++a) {
                FPDF_ANNOTATION an = FPDFPage_GetAnnot(p0, a);
                if (an && FPDFAnnot_GetSubtype(an) == FPDF_ANNOT_LINK) {
                    FPDF_LINK lk = FPDFAnnot_GetLink(an);
                    FPDF_DEST d = lk ? FPDFLink_GetDest(doc, lk) : nullptr;
                    int dp = d ? FPDFDest_GetDestPageIndex(doc, d) : -99;
                    if (!firstLink) std::cout << ",";
                    firstLink = false;
                    std::cout << "p" << dp;
                }
                if (an) FPDFPage_CloseAnnot(an);
            }
            std::cout << "]";
            FPDF_ClosePage(p0);
        }
    }

    // Check AcroForm fields across pages
    FPDF_FORMFILLINFO ffi{};
    ffi.version = 2;
    FPDF_FORMHANDLE fh = FPDFDOC_InitFormFillEnvironment(doc, &ffi);
    int widgetCount = 0;
    for (int i = 0; i < pageCount; ++i) {
        FPDF_PAGE pg = FPDF_LoadPage(doc, i);
        if (pg) {
            if (FPDFPage_HasFormFieldAtPoint(fh, pg, 120.0, 465.0) > 0) {
                ++widgetCount;
            }
            FPDF_ClosePage(pg);
        }
    }
    std::cout << " formWidgetsAtProbe=" << widgetCount;
    FPDFDOC_ExitFormFillEnvironment(fh);

    std::cout << "\n";
    FPDF_CloseDocument(doc);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: page_ops_spike <outdir> [signed.pdf] [encrypted.pdf] [enc_password]\n";
        return 1;
    }
    std::string outdir = argv[1];
    std::string signedPdf = argc > 2 ? argv[2] : "";
    std::string encryptedPdf = argc > 3 ? argv[3] : "";
    std::string encPassword = argc > 4 ? argv[4] : "";

    FPDF_InitLibrary();

    std::string richPath = outdir + "/rich_pages.pdf";
    std::string mergeSrcPath = outdir + "/src_merge.pdf";
    writeRichPagesFixture(richPath);
    writeTwoPageMergeSource(mergeSrcPath);
    inspectPdf("baseline_rich", richPath);

    // 1. FPDFPage_Delete (delete page 1)
    {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(richPath.c_str(), nullptr);
        FPDFPage_Delete(doc, 1);
        std::string out = outdir + "/op_delete_p1.pdf";
        bool saved = saveDocToPathViaFd(doc, out, 0);
        FPDF_CloseDocument(doc);
        std::cout << "[op_delete_p1] saved=" << saved << "\n";
        inspectPdf("op_delete_p1", out);
    }

    // 2. FPDF_MovePages (move [2, 0] to 0)
    {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(richPath.c_str(), nullptr);
        const int idx[] = {2, 0};
        FPDF_BOOL moved = FPDF_MovePages(doc, idx, 2, 0);
        std::string out = outdir + "/op_move.pdf";
        bool saved = saveDocToPathViaFd(doc, out, 0);
        FPDF_CloseDocument(doc);
        std::cout << "[op_move] moved=" << moved << " saved=" << saved << "\n";
        inspectPdf("op_move", out);
    }

    // 3. FPDF_ImportPagesByIndex + FPDF_ImportPages
    {
        FPDF_DOCUMENT src = FPDF_LoadDocument(richPath.c_str(), nullptr);
        FPDF_DOCUMENT src2 = FPDF_LoadDocument(mergeSrcPath.c_str(), nullptr);
        FPDF_DOCUMENT dest = FPDF_CreateNewDocument();
        const int idx[] = {2, 0};
        FPDF_BOOL impIdx = FPDF_ImportPagesByIndex(dest, src, idx, 2, 0);
        FPDF_BOOL impRange = FPDF_ImportPages(dest, src2, "1-2", 2);
        std::string out = outdir + "/op_import.pdf";
        bool saved = saveDocToPathViaFd(dest, out, 0);
        FPDF_CloseDocument(dest);
        FPDF_CloseDocument(src2);
        FPDF_CloseDocument(src);
        std::cout << "[op_import] impByIndex=" << impIdx << " impRange=" << impRange << " saved=" << saved << "\n";
        inspectPdf("op_import", out);
    }

    // 4. FPDFPage_SetRotation + FPDFPage_SetCropBox (non-incremental vs incremental)
    {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(richPath.c_str(), nullptr);
        FPDF_PAGE p0 = FPDF_LoadPage(doc, 0);
        FPDFPage_SetRotation(p0, 1);
        FPDFPage_SetCropBox(p0, 50.0f, 50.0f, 500.0f, 700.0f);
        FPDF_ClosePage(p0);
        std::string outCopy = outdir + "/op_rot_crop_copy.pdf";
        std::string outInc = outdir + "/op_rot_crop_inc.pdf";
        bool s1 = saveDocToPathViaFd(doc, outCopy, 0);
        bool s2 = saveDocToPathViaFd(doc, outInc, FPDF_INCREMENTAL);
        FPDF_CloseDocument(doc);
        std::cout << "[op_rot_crop] savedCopy=" << s1 << " savedInc=" << s2 << "\n";
        inspectPdf("op_rot_crop_copy", outCopy);
        inspectPdf("op_rot_crop_inc", outInc);
    }

    // 5. Signed document
    if (!signedPdf.empty()) {
        inspectPdf("signed_before", signedPdf);
        FPDF_DOCUMENT doc = FPDF_LoadDocument(signedPdf.c_str(), nullptr);
        if (doc) {
            FPDF_PAGE p0 = FPDF_LoadPage(doc, 0);
            if (p0) {
                FPDFPage_SetRotation(p0, 1);
                FPDF_ClosePage(p0);
            }
            std::string out = outdir + "/op_signed_copy.pdf";
            bool saved = saveDocToPathViaFd(doc, out, 0);
            FPDF_CloseDocument(doc);
            std::cout << "[op_signed_copy] saved=" << saved << "\n";
            inspectPdf("op_signed_copy", out);
        }
    }

    // 6. Encrypted document
    if (!encryptedPdf.empty()) {
        FPDF_DOCUMENT docNoPw = FPDF_LoadDocument(encryptedPdf.c_str(), nullptr);
        std::cout << "[encrypted_no_pw] loaded=" << (docNoPw != nullptr) << " err=" << FPDF_GetLastError() << "\n";
        if (docNoPw) FPDF_CloseDocument(docNoPw);

        FPDF_DOCUMENT doc = FPDF_LoadDocument(encryptedPdf.c_str(), encPassword.empty() ? nullptr : encPassword.c_str());
        std::cout << "[encrypted_with_pw] loaded=" << (doc != nullptr) << "\n";
        if (doc) {
            FPDF_DOCUMENT dest = FPDF_CreateNewDocument();
            FPDF_BOOL imp = FPDF_ImportPagesByIndex(dest, doc, nullptr, 0, 0);
            std::string out = outdir + "/op_encrypted_import.pdf";
            bool saved = saveDocToPathViaFd(dest, out, 0);
            FPDF_CloseDocument(dest);
            FPDF_CloseDocument(doc);
            std::cout << "[op_encrypted_import] imp=" << imp << " saved=" << saved << "\n";
            inspectPdf("op_encrypted_import_no_pw", out, nullptr);
        }
    }

    FPDF_DestroyLibrary();
    return 0;
}
