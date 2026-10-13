#include "PdfiumPage.h"
#include <algorithm>
#include <cmath>

namespace pdf {

PdfiumPage::PdfiumPage(FPDF_DOCUMENT doc, int pageIndex)
    : m_pageIndex(pageIndex) {
    if (doc) {
        m_page = FPDF_LoadPage(doc, pageIndex);
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
    const int pixelW = std::max(1, static_cast<int>(std::round(m_width * effectiveScale)));
    const int pixelH = std::max(1, static_cast<int>(std::round(m_height * effectiveScale)));

    // Qt's Format_ARGB32_Premultiplied on little-endian x86_64 uses BGRA byte order in memory
    QImage image(pixelW, pixelH, QImage::Format_ARGB32_Premultiplied);
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
