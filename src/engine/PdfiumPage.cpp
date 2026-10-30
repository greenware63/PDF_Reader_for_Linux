#include "PdfiumPage.h"
#include "PdfiumMutex.h"
#include "layout/DocumentLayout.h"
#include <fpdf_text.h>
#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace pdf {

static std::atomic<bool> s_simulateGetTextFailureForTesting{false};

void PdfiumPage::setSimulateGetTextFailureForTesting(bool simulate) noexcept {
    s_simulateGetTextFailureForTesting.store(simulate);
}

// Safety bounds applied ONLY to whole-page renders to prevent bad_alloc / OOM
static constexpr int MAX_WHOLE_PAGE_DIM = 8192;
static constexpr int64_t MAX_WHOLE_PAGE_PIXELS = 16 * 1024 * 1024; // 16 MP max per single buffer (~64 MB)

// Safety bounds on individual tile requests
static constexpr int MAX_TILE_DIM = 4096;
static constexpr int64_t MAX_TILE_PIXELS = 16 * 1024 * 1024;

PdfiumPage::PdfiumPage(DocumentHandle doc, int pageIndex, std::shared_ptr<FormEnv> form)
    : m_doc(std::move(doc)), m_form(std::move(form)), m_pageIndex(pageIndex) {
    if (m_doc) {
        std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
        m_page = FPDF_LoadPage(m_doc.get(), pageIndex);
        if (m_page && m_form && m_form->handle) {
            FORM_OnAfterLoadPage(m_page, m_form->handle);
        }
        if (m_page) {
            m_width = FPDF_GetPageWidthF(m_page);
            m_height = FPDF_GetPageHeightF(m_page);
        }
    }
}

PdfiumPage::~PdfiumPage() {
    if (m_page) {
        std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
        if (m_form && m_form->handle) {
            FORM_OnBeforeClosePage(m_page, m_form->handle);
        }
        FPDF_ClosePage(m_page);
        m_page = nullptr;
    }
}

// Caller holds the PDFium mutex. Draws widget (form field) appearances on top of the page render, using exactly
// the same placement/rotation/flags as the FPDF_RenderPageBitmap call it follows.
void PdfiumPage::drawFormFields(FPDF_BITMAP bitmap, int startX, int startY, int sizeX, int sizeY, int rotate, int flags) {
    if (m_form && m_form->handle) {
        FPDF_FFLDraw(m_form->handle, bitmap, m_page, startX, startY, sizeX, sizeY, rotate, flags);
    }
}

void PdfiumPage::getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const {
    // Single canonical calculation shared with DocumentLayout
    layout::computePagePixelDimensions(m_width, m_height, scale, devicePixelRatio, outWidth, outHeight);
}

PixelBuffer PdfiumPage::renderTile(const PixelRect& rect, double scale, double devicePixelRatio, int rotation) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0 || !rect.isValid()) {
        return {};
    }

    // Guard requested tile dimensions against oversized requests
    if (rect.width > MAX_TILE_DIM || rect.height > MAX_TILE_DIM ||
        (static_cast<int64_t>(rect.width) * rect.height > MAX_TILE_PIXELS)) {
        return {};
    }

    const int rot = (rotation % 360 + 360) % 360;
    int pdfiumRotate = 0;
    if (rot == 90) pdfiumRotate = 1;
    else if (rot == 180) pdfiumRotate = 2;
    else if (rot == 270) pdfiumRotate = 3;

    int unrotatedW = 0;
    int unrotatedH = 0;
    getPixelDimensions(scale, devicePixelRatio, unrotatedW, unrotatedH);

    const int fullPageW = (pdfiumRotate == 1 || pdfiumRotate == 3) ? unrotatedH : unrotatedW;
    const int fullPageH = (pdfiumRotate == 1 || pdfiumRotate == 3) ? unrotatedW : unrotatedH;

    // Intersect requested rect with valid page pixel coordinates
    int x0 = std::clamp(rect.x, 0, fullPageW);
    int y0 = std::clamp(rect.y, 0, fullPageH);
    int x1 = std::clamp(rect.x + rect.width, 0, fullPageW);
    int y1 = std::clamp(rect.y + rect.height, 0, fullPageH);

    int tileW = x1 - x0;
    int tileH = y1 - y0;

    if (tileW <= 0 || tileH <= 0) {
        return {};
    }

    if (tileW > MAX_TILE_DIM || tileH > MAX_TILE_DIM || (static_cast<int64_t>(tileW) * tileH > MAX_TILE_PIXELS)) {
        return {};
    }

    PixelBuffer buffer = PixelBuffer::allocate(tileW, tileH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    // Pre-fill with opaque white (0xFFFFFFFF in 32-bit little-endian)
    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        tileW,
        tileH,
        FPDFBitmap_BGRx,
        buffer.data(),
        buffer.stride()
    );

    if (!bitmap) {
        return {};
    }

    // Offset origin by (-x0, -y0) so the tile region aligns at (0, 0) in the bitmap.
    // Drop FPDF_LCD_TEXT to avoid sub-pixel color fringing on scaled tiles.
    const int flags = FPDF_ANNOT;
    FPDF_RenderPageBitmap(
        bitmap,
        m_page,
        -x0,
        -y0,
        fullPageW,
        fullPageH,
        pdfiumRotate,
        flags
    );
    drawFormFields(bitmap, -x0, -y0, fullPageW, fullPageH, pdfiumRotate, flags);

    FPDFBitmap_Destroy(bitmap);
    return buffer;
}

PixelBuffer PdfiumPage::renderWholePage(double scale, double devicePixelRatio, int rotation) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0) {
        return {};
    }

    const int rot = (rotation % 360 + 360) % 360;
    int pdfiumRotate = 0;
    if (rot == 90) pdfiumRotate = 1;
    else if (rot == 180) pdfiumRotate = 2;
    else if (rot == 270) pdfiumRotate = 3;

    int unrotatedW = 0;
    int unrotatedH = 0;
    getPixelDimensions(scale, devicePixelRatio, unrotatedW, unrotatedH);

    int pixelW = (pdfiumRotate == 1 || pdfiumRotate == 3) ? unrotatedH : unrotatedW;
    int pixelH = (pdfiumRotate == 1 || pdfiumRotate == 3) ? unrotatedW : unrotatedH;

    // Apply safety caps ONLY to single-buffer whole-page renders
    int64_t totalPixels = static_cast<int64_t>(pixelW) * pixelH;
    if (pixelW > MAX_WHOLE_PAGE_DIM || pixelH > MAX_WHOLE_PAGE_DIM || totalPixels > MAX_WHOLE_PAGE_PIXELS) {
        double factorW = (pixelW > MAX_WHOLE_PAGE_DIM) ? static_cast<double>(MAX_WHOLE_PAGE_DIM) / static_cast<double>(pixelW) : 1.0;
        double factorH = (pixelH > MAX_WHOLE_PAGE_DIM) ? static_cast<double>(MAX_WHOLE_PAGE_DIM) / static_cast<double>(pixelH) : 1.0;
        double factorTotal = (totalPixels > MAX_WHOLE_PAGE_PIXELS) ? std::sqrt(static_cast<double>(MAX_WHOLE_PAGE_PIXELS) / static_cast<double>(totalPixels)) : 1.0;
        double factor = std::min({factorW, factorH, factorTotal});

        pixelW = std::max(1, static_cast<int>(std::floor(pixelW * factor)));
        pixelH = std::max(1, static_cast<int>(std::floor(pixelH * factor)));
        if (pixelW > MAX_WHOLE_PAGE_DIM) pixelW = MAX_WHOLE_PAGE_DIM;
        if (pixelH > MAX_WHOLE_PAGE_DIM) pixelH = MAX_WHOLE_PAGE_DIM;
        while (static_cast<int64_t>(pixelW) * pixelH > MAX_WHOLE_PAGE_PIXELS && pixelW > 1 && pixelH > 1) {
            if (pixelW >= pixelH) --pixelW;
            else --pixelH;
        }
    }

    PixelBuffer buffer = PixelBuffer::allocate(pixelW, pixelH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        pixelW,
        pixelH,
        FPDFBitmap_BGRx,
        buffer.data(),
        buffer.stride()
    );

    if (!bitmap) {
        return {};
    }

    const int flags = FPDF_ANNOT;
    FPDF_RenderPageBitmap(bitmap, m_page, 0, 0, pixelW, pixelH, pdfiumRotate, flags);
    drawFormFields(bitmap, 0, 0, pixelW, pixelH, pdfiumRotate, flags);

    FPDFBitmap_Destroy(bitmap);
    return buffer;
}

PixelBuffer PdfiumPage::renderThumbnail(int targetWidth, int rotation) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0 || targetWidth <= 0) {
        return {};
    }

    const int normalizedRotate = ((rotation % 360) + 360) % 360;
    const bool swapDimensions = (normalizedRotate == 90 || normalizedRotate == 270);
    const double dispW = swapDimensions ? m_height : m_width;
    const double dispH = swapDimensions ? m_width : m_height;

    const int targetW = std::clamp(targetWidth, 16, 1024);
    const double scale = static_cast<double>(targetW) / dispW;
    const double scaledHeight = dispH * scale;
    constexpr int MAX_THUMBNAIL_DIM = 2048;
    constexpr int64_t MAX_THUMBNAIL_PIXELS = 4 * 1024 * 1024;
    if (!std::isfinite(scaledHeight) || scaledHeight <= 0.0 ||
        scaledHeight > MAX_THUMBNAIL_DIM ||
        static_cast<int64_t>(targetW) * static_cast<int64_t>(std::ceil(scaledHeight)) > MAX_THUMBNAIL_PIXELS) {
        return {};
    }
    const int targetH = std::max(1, static_cast<int>(std::round(scaledHeight)));

    PixelBuffer buffer = PixelBuffer::allocate(targetW, targetH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

    int pdfiumRotate = 0;
    switch (normalizedRotate) {
        case 90:  pdfiumRotate = 1; break;
        case 180: pdfiumRotate = 2; break;
        case 270: pdfiumRotate = 3; break;
        default:  pdfiumRotate = 0; break;
    }

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        targetW,
        targetH,
        FPDFBitmap_BGRx,
        buffer.data(),
        buffer.stride()
    );

    if (!bitmap) {
        return {};
    }

    FPDF_RenderPageBitmap(bitmap, m_page, 0, 0, targetW, targetH, pdfiumRotate, FPDF_ANNOT);
    drawFormFields(bitmap, 0, 0, targetW, targetH, pdfiumRotate, FPDF_ANNOT);
    FPDFBitmap_Destroy(bitmap);
    return buffer;
}

PageText PdfiumPage::extractText() {
    PageText result;
    result.pageIndex = m_pageIndex;
    result.widthPoints = m_width;
    result.heightPoints = m_height;

    if (!m_page) {
        return result;
    }

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(m_page);
    if (!textPage) {
        return result;
    }

    const int count = FPDFText_CountChars(textPage);
    if (count <= 0) {
        FPDFText_ClosePage(textPage);
        return result;
    }

    result.chars.reserve(count);

    std::vector<unsigned short> buffer(count * 2 + 1, 0);
    int extracted = s_simulateGetTextFailureForTesting.load() ? 0 : FPDFText_GetText(textPage, 0, count, buffer.data());
    if (extracted > 1) {
        result.text.assign(reinterpret_cast<const char16_t*>(buffer.data()), extracted - 1);
    }

    // FPDFText_GetCharBox() reports boxes in PDF user space (bottom-left origin, unrotated, and offset by
    // the page's crop box origin). The page is rendered through FPDF_RenderPageBitmap, which applies the
    // crop box and /Rotate, so convert with the same transform: FPDF_PageToDevice() at 1 device pixel per
    // point yields top-down coordinates that match the rendered page for any crop box and rotation.
    // FPDF_PageToDevice returns integers, so use a device rect of 16 pixels per point (1/16 pt resolution)
    // and scale the results back to points.
    constexpr double DEVICE_PIXELS_PER_POINT = 16.0;
    const int devW = std::max(1, static_cast<int>(std::lround(m_width * DEVICE_PIXELS_PER_POINT)));
    const int devH = std::max(1, static_cast<int>(std::lround(m_height * DEVICE_PIXELS_PER_POINT)));
    const double sx = m_width / devW;
    const double sy = m_height / devH;
    auto userRectToTopDown = [&](double l, double r, double b, double t, PageCharBox& out) -> bool {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        FPDF_PageToDevice(m_page, 0, 0, devW, devH, 0, l, b, &x0, &y0);
        FPDF_PageToDevice(m_page, 0, 0, devW, devH, 0, r, t, &x1, &y1);
        out.left = std::min(x0, x1) * sx;
        out.right = std::max(x0, x1) * sx;
        out.top = std::min(y0, y1) * sy;
        out.bottom = std::max(y0, y1) * sy;
        return true;
    };

    for (int i = 0; i < count; ++i) {
        unsigned int u = FPDFText_GetUnicode(textPage, i);
        PageCharBox box;
        box.unicode = u;
        double left = 0.0, right = 0.0, bottom = 0.0, top = 0.0;
        if (FPDFText_GetCharBox(textPage, i, &left, &right, &bottom, &top)) {
            if (std::abs(top - bottom) < 1e-4) {
                FS_RECTF looseRect{};
                if (FPDFText_GetLooseCharBox(textPage, i, &looseRect) && std::abs(looseRect.top - looseRect.bottom) >= 1e-4) {
                    bottom = looseRect.bottom;
                    top = looseRect.top;
                } else {
                    double fontSize = FPDFText_GetFontSize(textPage, i);
                    if (fontSize <= 0.0) fontSize = 12.0;
                    top = bottom + fontSize * 0.8;
                    bottom = bottom - fontSize * 0.2;
                }
            }
            userRectToTopDown(left, right, bottom, top, box);
        }

        // If this character is a high surrogate and the following character is a low surrogate,
        // merge them into a single 32-bit Unicode code point so that selection indexes match glyphs.
        if (u >= 0xD800 && u <= 0xDBFF && (i + 1) < count) {
            unsigned int nextU = FPDFText_GetUnicode(textPage, i + 1);
            if (nextU >= 0xDC00 && nextU <= 0xDFFF) {
                box.unicode = 0x10000 + ((u - 0xD800) << 10) + (nextU - 0xDC00);
                double l2 = 0.0, r2 = 0.0, b2 = 0.0, t2 = 0.0;
                PageCharBox low;
                if (FPDFText_GetCharBox(textPage, i + 1, &l2, &r2, &b2, &t2)) {
                    if (std::abs(t2 - b2) < 1e-4) {
                        FS_RECTF looseRect{};
                        if (FPDFText_GetLooseCharBox(textPage, i + 1, &looseRect) && std::abs(looseRect.top - looseRect.bottom) >= 1e-4) {
                            b2 = looseRect.bottom;
                            t2 = looseRect.top;
                        }
                    }
                    if (userRectToTopDown(l2, r2, b2, t2, low)) {
                        box.left = std::min(box.left, low.left);
                        box.right = std::max(box.right, low.right);
                        box.top = std::min(box.top, low.top);
                        box.bottom = std::max(box.bottom, low.bottom);
                    }
                }
                ++i; // consume low surrogate
            }
        }

        result.chars.push_back(box);
    }

    // Spaces (and similar glyphs) come back from PDFium with a zero-thickness box: zero height on
    // horizontal text, zero width on text rotated by 90/270 degrees. Give them the extent of the nearest
    // neighbouring glyph across the line so word gaps can be hit-tested and highlighted like any other
    // character.
    for (size_t i = 0; i < result.chars.size(); ++i) {
        auto& c = result.chars[i];
        if (c.isValid()) {
            continue;
        }
        const bool flatVertically = c.right > c.left && !(c.bottom > c.top);
        const bool flatHorizontally = c.bottom > c.top && !(c.right > c.left);
        if (!flatVertically && !flatHorizontally) {
            continue; // no geometry at all (e.g. generated line break): leave as is
        }
        const PageCharBox* donor = nullptr;
        if (i > 0 && result.chars[i - 1].isValid()) {
            donor = &result.chars[i - 1];
        } else if (i + 1 < result.chars.size() && result.chars[i + 1].isValid()) {
            donor = &result.chars[i + 1];
        }
        if (!donor) {
            if (flatVertically) {
                double fontSize = FPDFText_GetFontSize(textPage, static_cast<int>(i));
                if (fontSize <= 0.0) fontSize = 12.0;
                c.bottom = c.top + fontSize;
            }
            continue;
        }
        if (flatVertically) {
            c.top = donor->top;
            c.bottom = donor->bottom;
        } else {
            c.left = donor->left;
            c.right = donor->right;
        }
    }

    if (result.text.empty() && !result.chars.empty()) {
        result.text.reserve(result.chars.size() * 2);
        for (const auto& ch : result.chars) {
            char32_t cp = ch.unicode;
            if (cp == 0) {
                continue;
            }
            if (cp <= 0xFFFF) {
                result.text.push_back(static_cast<char16_t>(cp));
            } else if (cp <= 0x10FFFF) {
                cp -= 0x10000;
                result.text.push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
                result.text.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            }
        }
    }

    FPDFText_ClosePage(textPage);
    return result;
}


std::vector<AnnotationItem> PdfiumPage::listAnnotations(bool* truncated) {
    bool clippedAny = false;
    uint64_t budgetUsed = 0;
    struct TruncGuard {
        bool* out; const bool& flag;
        ~TruncGuard() { if (out) *out = flag; }
    } truncGuard{truncated, clippedAny};
    if (!m_page) {
        return {};
    }

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    int annotCount = FPDFPage_GetAnnotCount(m_page);
    if (annotCount <= 0) {
        return {};
    }
    // Hard bound against hostile-PDF resource exhaustion (RFC §6: reply <= 10 000 items)
    if (annotCount > 10000) {
        annotCount = 10000;
        clippedAny = true;
    }

    std::vector<AnnotationItem> result;
    result.reserve(static_cast<size_t>(annotCount));

    for (int i = 0; i < annotCount; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(m_page, i);
        if (!annot) {
            continue;
        }

        AnnotationItem item;
        item.pageIndex = m_pageIndex;
        item.annotIndex = i;

        FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(annot);
        switch (st) {
            case FPDF_ANNOT_TEXT: item.subtype = AnnotationSubtype::Text; break;
            case FPDF_ANNOT_LINK: item.subtype = AnnotationSubtype::Link; break;
            case FPDF_ANNOT_FREETEXT: item.subtype = AnnotationSubtype::FreeText; break;
            case FPDF_ANNOT_LINE: item.subtype = AnnotationSubtype::Line; break;
            case FPDF_ANNOT_SQUARE: item.subtype = AnnotationSubtype::Square; break;
            case FPDF_ANNOT_CIRCLE: item.subtype = AnnotationSubtype::Circle; break;
            case FPDF_ANNOT_POLYGON: item.subtype = AnnotationSubtype::Polygon; break;
            case FPDF_ANNOT_POLYLINE: item.subtype = AnnotationSubtype::Polyline; break;
            case FPDF_ANNOT_HIGHLIGHT: item.subtype = AnnotationSubtype::Highlight; break;
            case FPDF_ANNOT_UNDERLINE: item.subtype = AnnotationSubtype::Underline; break;
            case FPDF_ANNOT_SQUIGGLY: item.subtype = AnnotationSubtype::Squiggly; break;
            case FPDF_ANNOT_STRIKEOUT: item.subtype = AnnotationSubtype::StrikeOut; break;
            case FPDF_ANNOT_STAMP: item.subtype = AnnotationSubtype::Stamp; break;
            case FPDF_ANNOT_CARET: item.subtype = AnnotationSubtype::Caret; break;
            case FPDF_ANNOT_INK: item.subtype = AnnotationSubtype::Ink; break;
            case FPDF_ANNOT_POPUP: item.subtype = AnnotationSubtype::Popup; break;
            case FPDF_ANNOT_FILEATTACHMENT: item.subtype = AnnotationSubtype::FileAttachment; break;
            case FPDF_ANNOT_SOUND: item.subtype = AnnotationSubtype::Sound; break;
            case FPDF_ANNOT_MOVIE: item.subtype = AnnotationSubtype::Movie; break;
            case FPDF_ANNOT_WIDGET: item.subtype = AnnotationSubtype::Widget; break;
            case FPDF_ANNOT_SCREEN: item.subtype = AnnotationSubtype::Screen; break;
            case FPDF_ANNOT_PRINTERMARK: item.subtype = AnnotationSubtype::PrinterMark; break;
            case FPDF_ANNOT_TRAPNET: item.subtype = AnnotationSubtype::TrapNet; break;
            case FPDF_ANNOT_WATERMARK: item.subtype = AnnotationSubtype::Watermark; break;
            case FPDF_ANNOT_THREED: item.subtype = AnnotationSubtype::ThreeD; break;
            case FPDF_ANNOT_RICHMEDIA: item.subtype = AnnotationSubtype::RichMedia; break;
            case FPDF_ANNOT_XFAWIDGET: item.subtype = AnnotationSubtype::XfaWidget; break;
            case FPDF_ANNOT_REDACT: item.subtype = AnnotationSubtype::Redact; break;
            default: item.subtype = AnnotationSubtype::Unknown; break;
        }

        FS_RECTF rect{};
        if (FPDFAnnot_GetRect(annot, &rect)) {
            item.left = rect.left;
            item.top = rect.top;
            item.right = rect.right;
            item.bottom = rect.bottom;
        }

        item.flags = static_cast<uint32_t>(FPDFAnnot_GetFlags(annot));

        unsigned int r = 0, g = 0, b = 0, a = 0;
        if (FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a)) {
            item.hasColor = true;
            item.colorArgb = (static_cast<uint32_t>(a) << 24) |
                             (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) |
                             (static_cast<uint32_t>(b));
        }

        auto readStringKey = [&](const char* key, size_t maxChars) -> QString {
            unsigned long reqLen = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
            if (reqLen <= 2) {
                return {};
            }
            // Defense against unbounded hostile allocations (cap at 1 MiB)
            if (reqLen > 1024 * 1024) {
                clippedAny = true;
                return {};
            }
            std::vector<unsigned char> buf(reqLen, 0);
            unsigned long actual = FPDFAnnot_GetStringValue(annot, key, reinterpret_cast<FPDF_WCHAR*>(buf.data()), reqLen);
            if (actual != reqLen || actual <= 2 || actual > buf.size()) {
                clippedAny = true;
                return {};
            }
            const char16_t* u16 = reinterpret_cast<const char16_t*>(buf.data());
            size_t charCount = (actual - 2) / sizeof(char16_t);
            QString str = QString::fromUtf16(u16, static_cast<qsizetype>(charCount));
            if (static_cast<size_t>(str.size()) > maxChars) {
                clippedAny = true;
                str.truncate(static_cast<qsizetype>(maxChars));
            }
            return str;
        };

        item.contents = readStringKey("Contents", 32768);
        item.author = readStringKey("T", 256);
        item.name = readStringKey("NM", 1024);
        item.modificationDate = readStringKey("M", 256);

        if (FPDFAnnot_HasAttachmentPoints(annot)) {
            size_t qCount = FPDFAnnot_CountAttachmentPoints(annot);
            if (qCount > 4096) { qCount = 4096; clippedAny = true; }
            item.quadPoints.reserve(qCount);
            for (size_t q = 0; q < qCount; ++q) {
                FS_QUADPOINTSF quad{};
                if (FPDFAnnot_GetAttachmentPoints(annot, q, &quad)) {
                    AnnotationQuad aq{quad.x1, quad.y1, quad.x2, quad.y2, quad.x3, quad.y3, quad.x4, quad.y4};
                    if (aq.isValid()) {
                        item.quadPoints.push_back(aq);
                    }
                }
            }
        }

        if (item.subtype == AnnotationSubtype::Link && m_doc) {
            FPDF_LINK link = FPDFAnnot_GetLink(annot);
            if (link) {
                FPDF_ACTION act = FPDFLink_GetAction(link);
                if (act && FPDFAction_GetType(act) == PDFACTION_URI) {
                    unsigned long uriLen = FPDFAction_GetURIPath(m_doc.get(), act, nullptr, 0);
                    if (uriLen > 1 && uriLen <= 4096) {
                        std::vector<char> uriBuf(uriLen, 0);
                        unsigned long actualUri = FPDFAction_GetURIPath(m_doc.get(), act, uriBuf.data(), uriLen);
                        if (actualUri > 0 && actualUri <= uriBuf.size()) {
                            item.uri = QString::fromUtf8(uriBuf.data(), static_cast<qsizetype>(std::min<size_t>(actualUri, uriBuf.size()) - 1));
                        }
                    }
                }
            }
        }

        FPDFPage_CloseAnnot(annot);

        if (item.isValid()) {
            const uint64_t cost = annotationBudgetCost(item);
            if (budgetUsed + cost > kMaxAnnotationListBytes) {
                clippedAny = true;   // stop here: the rest would not fit in one IPC frame
                break;
            }
            budgetUsed += cost;
            result.push_back(std::move(item));
        }
    }

    return result;
}

bool PdfiumPage::hitTestAnnotation(double xPoints, double yPoints, AnnotationItem& out) {
    if (!m_page || std::isnan(xPoints) || std::isnan(yPoints) || std::isinf(xPoints) || std::isinf(yPoints)) {
        return false;
    }

    // List all annotations on the page
    const auto annotations = listAnnotations();
    // Hit-test in reverse order: the topmost annotation (drawn last) wins
    for (auto it = annotations.rbegin(); it != annotations.rend(); ++it) {
        if (it->contains(xPoints, yPoints)) {
            out = *it;
            return true;
        }
    }
    return false;
}

bool PdfiumPage::addHighlightAnnotation(const AnnotationItem& item) {
    if (!m_page || !item.isValid()) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    // 1. Create the annotation in PDFium
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(m_page, FPDF_ANNOT_HIGHLIGHT);
    if (!annot) {
        return false;
    }

    // 2. Set /Rect (Rule 1: FPDFAnnot_SetAP returns false unless /Rect is already set)
    FS_RECTF rect{};
    rect.left = static_cast<float>(item.left);
    rect.top = static_cast<float>(item.top);
    rect.right = static_cast<float>(item.right);
    rect.bottom = static_cast<float>(item.bottom);
    if (!FPDFAnnot_SetRect(annot, &rect)) {
        FPDFPage_CloseAnnot(annot);
        return false;
    }

    // 3. Set QuadPoints (Rule 3: Poppler requires quadpoints for Highlight)
    if (!item.quadPoints.empty()) {
        for (const auto& q : item.quadPoints) {
            FS_QUADPOINTSF qp{};
            qp.x1 = static_cast<float>(q.x1);
            qp.y1 = static_cast<float>(q.y1);
            qp.x2 = static_cast<float>(q.x2);
            qp.y2 = static_cast<float>(q.y2);
            qp.x3 = static_cast<float>(q.x3);
            qp.y3 = static_cast<float>(q.y3);
            qp.x4 = static_cast<float>(q.x4);
            qp.y4 = static_cast<float>(q.y4);
            FPDFAnnot_AppendAttachmentPoints(annot, &qp);
        }
    } else {
        FS_QUADPOINTSF qp{};
        qp.x1 = rect.left;  qp.y1 = rect.top;
        qp.x2 = rect.right; qp.y2 = rect.top;
        qp.x3 = rect.left;  qp.y3 = rect.bottom;
        qp.x4 = rect.right; qp.y4 = rect.bottom;
        FPDFAnnot_AppendAttachmentPoints(annot, &qp);
    }

    // 4. Set Color (/C) (Rule 3: Highlight additionally requires /C)
    uint32_t a = (item.colorArgb >> 24) & 0xFF;
    uint32_t r = (item.colorArgb >> 16) & 0xFF;
    uint32_t g = (item.colorArgb >> 8) & 0xFF;
    uint32_t b = item.colorArgb & 0xFF;
    if (a == 0 && item.colorArgb == 0) {
        r = 255; g = 255; b = 0; a = 255;
    }
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, a);

    // 5. Set Flags (/F)
    uint32_t flags = item.flags ? item.flags : FPDF_ANNOT_FLAG_PRINT;
    FPDFAnnot_SetFlags(annot, flags);

    // 6. Set /NM (unique identifier string)
    if (!item.name.isEmpty()) {
        const std::u16string u16 = item.name.toStdU16String();
        FPDFAnnot_SetStringValue(annot, "NM", reinterpret_cast<FPDF_WIDESTRING>(u16.c_str()));
    }

    // 7. Set /Contents
    if (!item.contents.isEmpty()) {
        const std::u16string u16 = item.contents.toStdU16String();
        FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(u16.c_str()));
    }

    // 8. Set /T (Author)
    if (!item.author.isEmpty()) {
        const std::u16string u16 = item.author.toStdU16String();
        FPDFAnnot_SetStringValue(annot, "T", reinterpret_cast<FPDF_WIDESTRING>(u16.c_str()));
    }

    // 9. Generate Appearance Stream (/AP) in page coordinates (Rule 2 from M0)
    std::string apStream;
    float rf = static_cast<float>(r) / 255.0f;
    float gf = static_cast<float>(g) / 255.0f;
    float bf = static_cast<float>(b) / 255.0f;
    char colBuf[128];
    std::snprintf(colBuf, sizeof(colBuf), "%.3f %.3f %.3f rg ", rf, gf, bf);
    apStream += colBuf;

    if (!item.quadPoints.empty()) {
        for (const auto& q : item.quadPoints) {
            float minX = static_cast<float>(std::min({q.x1, q.x2, q.x3, q.x4}));
            float maxX = static_cast<float>(std::max({q.x1, q.x2, q.x3, q.x4}));
            float minY = static_cast<float>(std::min({q.y1, q.y2, q.y3, q.y4}));
            float maxY = static_cast<float>(std::max({q.y1, q.y2, q.y3, q.y4}));
            char rectBuf[128];
            std::snprintf(rectBuf, sizeof(rectBuf), "%.2f %.2f %.2f %.2f re f ", minX, minY, maxX - minX, maxY - minY);
            apStream += rectBuf;
        }
    } else {
        float x = rect.left;
        float y = rect.bottom;
        float w = rect.right - rect.left;
        float h = rect.top - rect.bottom;
        char rectBuf[128];
        std::snprintf(rectBuf, sizeof(rectBuf), "%.2f %.2f %.2f %.2f re f ", x, y, w, h);
        apStream += rectBuf;
    }

    QString qap = QString::fromStdString(apStream);
    const std::u16string apU16 = qap.toStdU16String();
    FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, reinterpret_cast<FPDF_WIDESTRING>(apU16.c_str()));

    FPDFPage_CloseAnnot(annot);
    return true;
}

} // namespace pdf
