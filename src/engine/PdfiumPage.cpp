#include "PdfiumPage.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pdf {

// Safety bounds applied ONLY to whole-page renders to prevent bad_alloc / OOM
static constexpr int MAX_WHOLE_PAGE_DIM = 8192;
static constexpr int64_t MAX_WHOLE_PAGE_PIXELS = 16 * 1024 * 1024; // 16 MP max per single buffer (~64 MB)

// Safety bounds on individual tile requests
static constexpr int MAX_TILE_DIM = 4096;
static constexpr int64_t MAX_TILE_PIXELS = 16 * 1024 * 1024;

PdfiumPage::PdfiumPage(DocumentHandle doc, int pageIndex)
    : m_doc(std::move(doc)), m_pageIndex(pageIndex) {
    if (m_doc) {
        m_page = FPDF_LoadPage(m_doc.get(), pageIndex);
        if (m_page) {
            m_width = FPDF_GetPageWidthF(m_page);
            m_height = FPDF_GetPageHeightF(m_page);
        }
    }
}

PdfiumPage::~PdfiumPage() {
    if (m_page) {
        FPDF_ClosePage(m_page);
        m_page = nullptr;
    }
}

void PdfiumPage::getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const {
    if (m_width <= 0.0 || m_height <= 0.0) {
        outWidth = 0;
        outHeight = 0;
        return;
    }

    const double desiredDpr = (devicePixelRatio > 0.0) ? devicePixelRatio : 1.0;
    // Compute unconstrained virtual pixel coordinates for tiling
    outWidth = std::max(1, static_cast<int>(std::round(m_width * scale * desiredDpr)));
    outHeight = std::max(1, static_cast<int>(std::round(m_height * scale * desiredDpr)));
}

PixelBuffer PdfiumPage::renderTile(const PixelRect& rect, double scale, double devicePixelRatio) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0 || !rect.isValid()) {
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

    // Guard individual tile dimensions against absurd requests
    if (tileW > MAX_TILE_DIM || tileH > MAX_TILE_DIM || (static_cast<int64_t>(tileW) * tileH > MAX_TILE_PIXELS)) {
        return {};
    }

    PixelBuffer buffer = PixelBuffer::allocate(tileW, tileH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    // Pre-fill with opaque white (0xFFFFFFFF in 32-bit little-endian)
    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

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
        double factorW = (pixelW > MAX_WHOLE_PAGE_DIM) ? static_cast<double>(MAX_WHOLE_PAGE_DIM) / pixelW : 1.0;
        double factorH = (pixelH > MAX_WHOLE_PAGE_DIM) ? static_cast<double>(MAX_WHOLE_PAGE_DIM) / pixelH : 1.0;
        double factorTotal = (totalPixels > MAX_WHOLE_PAGE_PIXELS) ? std::sqrt(static_cast<double>(MAX_WHOLE_PAGE_PIXELS) / totalPixels) : 1.0;
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

} // namespace pdf
