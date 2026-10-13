#pragma once

#include "engine/IPage.h"
#include <fpdfview.h>

namespace pdf {

class PdfiumPage : public IPage {
public:
    PdfiumPage(FPDF_DOCUMENT doc, int pageIndex);
    ~PdfiumPage() override;

    int pageIndex() const override { return m_pageIndex; }
    double widthPoints() const override { return m_width; }
    double heightPoints() const override { return m_height; }

    QImage render(double scale, double devicePixelRatio) override;

private:
    FPDF_PAGE m_page{nullptr};
    int m_pageIndex{0};
    double m_width{0.0};
    double m_height{0.0};
};

} // namespace pdf
