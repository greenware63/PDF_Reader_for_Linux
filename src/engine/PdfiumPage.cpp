#include "PdfiumPage.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pdf {

// Safety bounds against excessive single-buffer memory consumption
static constexpr int MAX_DIMENSION = 8192;
static constexpr int64_t MAX_TOTAL_PIXELS = 16 * 1024 * 1024; // 16 MP max per single render (~64 MB)

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

    const double targetLogicalW = m_width * scale;
    const double targetLogicalH = m_height * scale;
    const double desiredDpr = (devicePixelRatio > 0.0) ? devicePixelRatio : 1.0;

    int pixelW = static_cast<int>(std::round(targetLogicalW * desiredDpr));
    int pixelH = static_cast<int>(std::round(targetLogicalH * desiredDpr));

    pixelW = std::max(1, pixelW);
    pixelH = std::max(1, pixelH);

    // Apply safety caps
    int64_t totalPixels = static_cast<int64_t>(pixelW) * pixelH;
    if (pixelW > MAX_DIMENSION || pixelH > MAX_DIMENSION || totalPixels > MAX_TOTAL_PIXELS) {
        double factorW = (pixelW > MAX_DIMENSION) ? static_cast<double>(MAX_DIMENSION) / pixelW : 1.0;
        double factorH = (pixelH > MAX_DIMENSION) ? static_cast<double>(MAX_DIMENSION) / pixelH : 1.0;
        double factorTotal = (totalPixels > MAX_TOTAL_PIXELS) ? std::sqrt(static_cast<double>(MAX_TOTAL_PIXELS) / totalPixels) : 1.0;
        double factor = std::min({factorW, factorH, factorTotal});

        pixelW = std::max(1, static_cast<int>(std::floor(pixelW * factor)));
        pixelH = std::max(1, static_cast<int>(std::floor(pixelH * factor)));

        if (pixelW > MAX_DIMENSION) pixelW = MAX_DIMENSION;
        if (pixelH > MAX_DIMENSION) pixelH = MAX_DIMENSION;
        while (static_cast<int64_t>(pixelW) * pixelH > MAX_TOTAL_PIXELS && pixelW > 1 && pixelH > 1) {
            if (pixelW >= pixelH) {
                --pixelW;
            } else {
                --pixelH;
            }
        }
    }

    outWidth = pixelW;
    outHeight = pixelH;
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

    PixelBuffer buffer = PixelBuffer::allocate(tileW, tileH, PixelFormat::BGRX8888);
    if (!buffer.isValid()) {
        return {};
    }

    // Pre-fill with opaque white (0xFFFFFFFF in 32-bit little-endian)
    std::memset(buffer.data(), 0xFF, buffer.sizeBytes());

    // FPDFBitmap_BGRx wraps BGRX 32-bit pixels (maps to QImage::Format_RGB32)
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

QImage PdfiumPage::render(double scale, double devicePixelRatio) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0) {
        return {};
    }

    int pixelW = 0;
    int pixelH = 0;
    getPixelDimensions(scale, devicePixelRatio, pixelW, pixelH);

    PixelBuffer buffer = renderTile({0, 0, pixelW, pixelH}, scale, devicePixelRatio);
    if (!buffer.isValid()) {
        return {};
    }

    // Wrap PixelBuffer into QImage with owner lifetime retention
    auto owner = buffer.owner();
    QImage image(
        buffer.data(),
        buffer.width(),
        buffer.height(),
        buffer.stride(),
        QImage::Format_RGB32,
        [](void* p) {
            auto* keepAlive = static_cast<std::shared_ptr<const void>*>(p);
            delete keepAlive;
        },
        new std::shared_ptr<const void>(owner)
    );

    // Calculate effective DPR so logical size remains invariant to clamping
    const double targetLogicalW = m_width * scale;
    const double effectiveDpr = static_cast<double>(pixelW) / targetLogicalW;
    image.setDevicePixelRatio(effectiveDpr);
    return image.copy(); // Return an independent copy for the legacy caller
}

} // namespace pdf
