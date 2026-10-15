#include "PdfiumPage.h"
#include "PdfiumMutex.h"
#include "layout/DocumentLayout.h"
#include <fpdf_text.h>
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

PdfiumPage::PdfiumPage(DocumentHandle doc, int pageIndex)
    : m_doc(std::move(doc)), m_pageIndex(pageIndex) {
    if (m_doc) {
        std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
        m_page = FPDF_LoadPage(m_doc.get(), pageIndex);
        if (m_page) {
            m_width = FPDF_GetPageWidthF(m_page);
            m_height = FPDF_GetPageHeightF(m_page);
        }
    }
}

PdfiumPage::~PdfiumPage() {
    if (m_page) {
        std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
        FPDF_ClosePage(m_page);
        m_page = nullptr;
    }
}

void PdfiumPage::getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const {
    // Single canonical calculation shared with DocumentLayout
    layout::computePagePixelDimensions(m_width, m_height, scale, devicePixelRatio, outWidth, outHeight);
}

PixelBuffer PdfiumPage::renderTile(const PixelRect& rect, double scale, double devicePixelRatio) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0 || !rect.isValid()) {
        return {};
    }

    // Guard requested tile dimensions against oversized requests
    if (rect.width > MAX_TILE_DIM || rect.height > MAX_TILE_DIM ||
        (static_cast<int64_t>(rect.width) * rect.height > MAX_TILE_PIXELS)) {
        return {};
    }

    int fullPageW = 0;
    int fullPageH = 0;
    getPixelDimensions(scale, devicePixelRatio, fullPageW, fullPageH);

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
        0,
        flags
    );

    FPDFBitmap_Destroy(bitmap);
    return buffer;
}

PixelBuffer PdfiumPage::renderWholePage(double scale, double devicePixelRatio) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0) {
        return {};
    }

    int pixelW = 0;
    int pixelH = 0;
    getPixelDimensions(scale, devicePixelRatio, pixelW, pixelH);

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
    FPDF_RenderPageBitmap(bitmap, m_page, 0, 0, pixelW, pixelH, 0, flags);

    FPDFBitmap_Destroy(bitmap);
    return buffer;
}

PixelBuffer PdfiumPage::renderThumbnail(int targetWidth) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0 || targetWidth <= 0) {
        return {};
    }

    const int targetW = std::clamp(targetWidth, 16, 1024);
    const double scale = static_cast<double>(targetW) / m_width;
    const int targetH = std::max(1, static_cast<int>(std::round(m_height * scale)));

    PixelBuffer buffer = PixelBuffer::allocate(targetW, targetH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

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

    FPDF_RenderPageBitmap(bitmap, m_page, 0, 0, targetW, targetH, 0, FPDF_ANNOT);
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

} // namespace pdf
