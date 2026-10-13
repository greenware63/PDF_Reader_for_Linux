#include "PdfiumPage.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pdf {

// Safety bounds against excessive memory consumption before tiling
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

QImage PdfiumPage::render(double scale, double devicePixelRatio) {
    if (!m_page || m_width <= 0.0 || m_height <= 0.0) {
        return {};
    }

    const double effectiveScale = scale * devicePixelRatio;
    int pixelW = static_cast<int>(std::round(m_width * effectiveScale));
    int pixelH = static_cast<int>(std::round(m_height * effectiveScale));

    if (pixelW <= 0 || pixelH <= 0) {
        return {};
    }

    // Clamp resolution if exceeding safe bounds to avoid bad_alloc / OOM
    const int64_t totalPixels = static_cast<int64_t>(pixelW) * pixelH;
    if (pixelW > MAX_DIMENSION || pixelH > MAX_DIMENSION || totalPixels > MAX_TOTAL_PIXELS) {
        double factorW = (pixelW > MAX_DIMENSION) ? static_cast<double>(MAX_DIMENSION) / pixelW : 1.0;
        double factorH = (pixelH > MAX_DIMENSION) ? static_cast<double>(MAX_DIMENSION) / pixelH : 1.0;
        double factorTotal = (totalPixels > MAX_TOTAL_PIXELS) ? std::sqrt(static_cast<double>(MAX_TOTAL_PIXELS) / totalPixels) : 1.0;
        double factor = std::min({factorW, factorH, factorTotal});

        pixelW = std::max(1, static_cast<int>(std::round(pixelW * factor)));
        pixelH = std::max(1, static_cast<int>(std::round(pixelH * factor)));
    }

    // Qt's Format_ARGB32_Premultiplied on little-endian x86_64 matches PDFium's BGRA memory format
    QImage image(pixelW, pixelH, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::white);

    // Direct zero-copy wrapper around QImage memory
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        pixelW,
        pixelH,
        FPDFBitmap_BGRA,
        image.bits(),
        static_cast<int>(image.bytesPerLine())
    );

    if (!bitmap) {
        return {};
    }

    // Render annotations and anti-aliased LCD text
    const int flags = FPDF_ANNOT | FPDF_LCD_TEXT;
    FPDF_RenderPageBitmap(bitmap, m_page, 0, 0, pixelW, pixelH, 0, flags);
    FPDFBitmap_Destroy(bitmap);

    image.setDevicePixelRatio(devicePixelRatio);
    return image;
}

} // namespace pdf
