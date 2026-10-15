#pragma once

#include "engine/IPage.h"
#include <fpdfview.h>
#include <memory>
#include <type_traits>
#include <atomic>

namespace pdf {

using DocumentHandle = std::shared_ptr<std::remove_pointer_t<FPDF_DOCUMENT>>;

class PdfiumPage : public IPage {
public:
    PdfiumPage(DocumentHandle doc, int pageIndex);
    ~PdfiumPage() override;

    bool isValid() const { return m_page != nullptr; }
    int pageIndex() const override { return m_pageIndex; }
    double widthPoints() const override { return m_width; }
    double heightPoints() const override { return m_height; }

    void getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const override;
    PixelBuffer renderTile(const PixelRect& rect, double scale, double devicePixelRatio) override;
    PixelBuffer renderWholePage(double scale, double devicePixelRatio) override;
    PixelBuffer renderThumbnail(int targetWidth) override;
    PageText extractText() override;

    static void setSimulateGetTextFailureForTesting(bool simulate) noexcept;

private:
    DocumentHandle m_doc;
    FPDF_PAGE m_page{nullptr};
    int m_pageIndex{0};
    double m_width{0.0};
    double m_height{0.0};
};

} // namespace pdf
